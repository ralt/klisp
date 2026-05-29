// SPDX-License-Identifier: MIT
/*
 * klisp — a Lisp machine inside the Linux kernel.
 *
 * Milestone M3: a SWANK server so Emacs/SLIME can connect, alongside the raw
 * line REPL from M2. On connect the protocol is auto-detected: SLIME sends a
 * length-prefixed message immediately, a human at `nc` does not — so the same
 * TCP port serves both (DESIGN.md §5).
 *
 *   SLIME:  M-x slime-connect RET localhost RET 4005
 *   raw:    nc localhost 4005    then type e.g.  (+ 1 2)
 *
 * Lisp errors (bad input, unbound symbols, divide by zero, runaway recursion)
 * are reported to the client and the session recovers; they must never crash
 * the kernel.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <linux/objtool.h>
#include <net/sock.h>

#include "fe.h"

#define KLISP_BACKLOG	8
#define KLISP_POLL_MS	500
#define KLISP_DETECT_MS	500		/* wait for the first byte to pick protocol */
#define KLISP_FE_HEAP	(256 * 1024)	/* fe object arena, per connection */

#define RBUFSZ		512
#define WBUFSZ		1024
#define MSGSZ		4096		/* max SWANK message / response payload */
#define SCRATCHSZ	1024		/* eval input / printed result */
#define CAPSZ		1024		/* captured (print ...) output */

static ushort port = 4005;
module_param(port, ushort, 0444);
MODULE_PARM_DESC(port, "TCP port to listen on (default 4005)");

static char *bind_addr = "127.0.0.1";
module_param(bind_addr, charp, 0444);
MODULE_PARM_DESC(bind_addr, "IPv4 address to bind (default 127.0.0.1)");

static struct socket *listen_sock;
static struct task_struct *listen_thread;

/* Per-connection state. One connection is served at a time (the listener runs
 * the session inline), so a single global pointer reaches the callbacks fe
 * invokes without a udata channel (error, print). */
struct repl_io {
	struct socket *sock;
	klisp_jmp_buf errjmp;		/* session top level, jumped to on error */
	char rbuf[RBUFSZ];
	int rlen, rpos;
	bool eof;
	char wbuf[WBUFSZ];
	int wlen;
	char errmsg[160];		/* pending error, reported after unwind */
	/* SWANK */
	bool swank;
	long cur_id;			/* :emacs-rex id being handled, -1 if none */
	char msgbuf[MSGSZ];		/* request payload / response builder */
	char scratch[SCRATCHSZ];	/* eval input then printed result */
	char capbuf[CAPSZ];		/* captured (print ...) output */
	int caplen;
	bool capturing;
};

static struct repl_io *g_io;

/* ---- socket I/O -------------------------------------------------------- */

static void repl_flush(struct repl_io *io)
{
	int off = 0;

	while (off < io->wlen && !kthread_should_stop()) {
		struct msghdr msg = { };
		struct kvec vec = {
			.iov_base = io->wbuf + off,
			.iov_len = io->wlen - off,
		};
		int s = kernel_sendmsg(io->sock, &msg, &vec, 1, io->wlen - off);

		if (s == -EAGAIN || s == -EWOULDBLOCK || s == -ERESTARTSYS)
			continue;
		if (s <= 0)
			break;
		off += s;
	}
	io->wlen = 0;
}

static void repl_putc(struct repl_io *io, char c)
{
	if (io->wlen >= WBUFSZ)
		repl_flush(io);
	io->wbuf[io->wlen++] = c;
}

static void repl_puts(struct repl_io *io, const char *s)
{
	while (*s)
		repl_putc(io, *s++);
}

/* Next byte from the connection, refilling from the socket and blocking (with a
 * timeout so unload can interrupt) when the buffer drains. Returns -1 on EOF. */
static int io_getc(struct repl_io *io)
{
	while (io->rpos >= io->rlen) {
		struct msghdr msg = { };
		struct kvec vec = { .iov_base = io->rbuf, .iov_len = RBUFSZ };
		int n;

		if (kthread_should_stop()) {
			io->eof = true;
			return -1;
		}
		n = kernel_recvmsg(io->sock, &msg, &vec, 1, RBUFSZ, 0);
		if (n == -EAGAIN || n == -EWOULDBLOCK || n == -ERESTARTSYS)
			continue;
		if (n <= 0) {
			io->eof = true;
			return -1;
		}
		io->rlen = n;
		io->rpos = 0;
	}
	return (unsigned char)io->rbuf[io->rpos++];
}

/* fe_ReadFn for the raw REPL. */
static char repl_readfn(fe_Context *ctx, void *udata)
{
	int c = io_getc(udata);

	(void)ctx;
	return c < 0 ? '\0' : (char)c;
}

/* ---- callbacks fe needs (declared in vendor/fe/fe_port.h) -------------- */

/* fe_WriteFn for (print ...) and result printing. In SWANK mode during eval,
 * output is captured to be wrapped in a :write-string message. */
void klisp_write_char(fe_Context *ctx, void *udata, char chr)
{
	(void)ctx;
	(void)udata;
	if (!g_io)
		return;
	if (g_io->capturing) {
		if (g_io->caplen < CAPSZ)
			g_io->capbuf[g_io->caplen++] = chr;
	} else {
		repl_putc(g_io, chr);
	}
}

void klisp_emergency_longjmp(void)
{
	if (g_io)
		__builtin_longjmp(g_io->errjmp, 1);
	pr_err("fe error with no active REPL context\n");
}
STACK_FRAME_NON_STANDARD(klisp_emergency_longjmp);

long klisp_strtod(const char *s, char **end)
{
	const char *p = s;
	int neg = 0, got = 0;
	long v = 0;

	if (*p == '+' || *p == '-') {
		neg = (*p == '-');
		p++;
	}
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p - '0');
		p++;
		got = 1;
	}
	if (!got) {
		*end = (char *)s;
		return 0;
	}
	*end = (char *)p;
	return neg ? -v : v;
}

/* fe error handler. Stash the message and unwind to the session top level; do
 * NOT touch the socket here — fe_error can fire deep in the evaluator's
 * recursion, where the multi-KB network TX path would overflow the kernel
 * stack. The top-level handler reports the message after the stack unwinds. */
static void repl_onerror(fe_Context *ctx, const char *msg, fe_Object *cl)
{
	(void)ctx;
	(void)cl;
	if (g_io) {
		strscpy(g_io->errmsg, msg, sizeof(g_io->errmsg));
		__builtin_longjmp(g_io->errjmp, 1);
	}
}
STACK_FRAME_NON_STANDARD(repl_onerror);

/* ---- shared eval ------------------------------------------------------- */

struct strrd { const char *s; int pos, len; };

static char strrd_fn(fe_Context *ctx, void *udata)
{
	struct strrd *r = udata;

	(void)ctx;
	return r->pos < r->len ? r->s[r->pos++] : '\0';
}

/* Read and evaluate every form in `code`, returning the last result. May
 * longjmp via fe_error on a Lisp error. */
static fe_Object *eval_string(fe_Context *ctx, const char *code)
{
	struct strrd r = { .s = code, .pos = 0, .len = (int)strlen(code) };
	fe_Object *obj, *res = fe_bool(ctx, 0); /* nil */
	int gc = fe_savegc(ctx);

	while ((obj = fe_read(ctx, strrd_fn, &r)) != NULL) {
		fe_restoregc(ctx, gc);
		fe_pushgc(ctx, obj);
		res = fe_eval(ctx, obj);
	}
	return res;
}

/* ---- raw line REPL (M2) ------------------------------------------------ */

static void klisp_raw_repl(struct repl_io *io, fe_Context *ctx)
{
	int gc;

	repl_puts(io, "klisp Lisp REPL — try (+ 1 2), (= x 10), (* x x); errors recover\n> ");
	repl_flush(io);

	gc = fe_savegc(ctx);
	if (__builtin_setjmp(io->errjmp)) {
		fe_restoregc(ctx, gc);
		if (io->errmsg[0]) {
			repl_puts(io, "error: ");
			repl_puts(io, io->errmsg);
			repl_putc(io, '\n');
			io->errmsg[0] = '\0';
		}
		repl_puts(io, "> ");
		repl_flush(io);
	}

	for (;;) {
		fe_Object *obj;

		fe_restoregc(ctx, gc);
		if (kthread_should_stop() || io->eof)
			break;
		obj = fe_read(ctx, repl_readfn, io);
		if (!obj)
			break;
		obj = fe_eval(ctx, obj);
		fe_write(ctx, obj, klisp_write_char, NULL, 0);
		repl_puts(io, "\n> ");
		repl_flush(io);
	}
}
STACK_FRAME_NON_STANDARD(klisp_raw_repl);

/* ---- SWANK server ------------------------------------------------------ */

/* Send one SWANK message: 6-hex length header + payload. */
static void swank_emit(struct repl_io *io, const char *payload)
{
	char hdr[8];
	int n = (int)strlen(payload);

	snprintf(hdr, sizeof(hdr), "%06x", n);
	repl_puts(io, hdr);
	repl_puts(io, payload);
	repl_flush(io);
}

/* bounded string builders for assembling response payloads */
static void bput(char *b, int cap, int *p, char c)
{
	if (*p < cap - 1)
		b[(*p)++] = c;
}
static void bputs(char *b, int cap, int *p, const char *s)
{
	while (*s)
		bput(b, cap, p, *s++);
}
/* append s as an escaped Lisp string literal: "..." */
static void bputstr(char *b, int cap, int *p, const char *s)
{
	bput(b, cap, p, '"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			bput(b, cap, p, '\\');
		bput(b, cap, p, *s);
	}
	bput(b, cap, p, '"');
}

static int ishex(int c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}

/* Read one SWANK message body into io->msgbuf (NUL-terminated). Returns false
 * on EOF. Oversized messages are truncated (SWANK control messages are small). */
static bool swank_read_msg(struct repl_io *io)
{
	char hdr[6];
	int i, len = 0;

	for (i = 0; i < 6; i++) {
		int c = io_getc(io);

		if (c < 0)
			return false;
		hdr[i] = c;
	}
	for (i = 0; i < 6; i++) {
		int c = hdr[i];

		len <<= 4;
		if (c >= '0' && c <= '9')
			len |= c - '0';
		else if (c >= 'a' && c <= 'f')
			len |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			len |= c - 'A' + 10;
	}
	for (i = 0; i < len; i++) {
		int c = io_getc(io);

		if (c < 0)
			return false;
		if (i < MSGSZ - 1)
			io->msgbuf[i] = c;
	}
	io->msgbuf[len < MSGSZ - 1 ? len : MSGSZ - 1] = '\0';
	return true;
}

static fe_Object *list_nth(fe_Context *ctx, fe_Object *lst, int n)
{
	while (n-- > 0 && !fe_isnil(ctx, lst))
		lst = fe_cdr(ctx, lst);
	return fe_car(ctx, lst);
}

static void swank_return_ok_nil(struct repl_io *io, long id)
{
	int p = 0;
	char t[24];

	bputs(io->msgbuf, MSGSZ, &p, "(:return (:ok nil) ");
	snprintf(t, sizeof(t), "%ld)", id);
	bputs(io->msgbuf, MSGSZ, &p, t);
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);
}

static void swank_abort(struct repl_io *io, long id, const char *msg)
{
	int p = 0;
	char t[24];

	bputs(io->msgbuf, MSGSZ, &p, "(:return (:abort ");
	bputstr(io->msgbuf, MSGSZ, &p, msg);
	snprintf(t, sizeof(t), ") %ld)", id);
	bputs(io->msgbuf, MSGSZ, &p, t);
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);
}

static void swank_connection_info(struct repl_io *io, long id)
{
	int p = 0;
	char t[32];

	bputs(io->msgbuf, MSGSZ, &p,
	      "(:return (:ok (:pid ");
	snprintf(t, sizeof(t), "%d", task_pid_nr(current));
	bputs(io->msgbuf, MSGSZ, &p, t);
	bputs(io->msgbuf, MSGSZ, &p,
	      " :package (:name \"klisp-user\" :prompt \"klisp\")"
	      " :lisp-implementation (:type \"klisp\" :name \"klisp\""
	      " :version \"" FE_VERSION "\")"
	      " :version \"2.26\")) ");
	snprintf(t, sizeof(t), "%ld)", id);
	bputs(io->msgbuf, MSGSZ, &p, t);
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);
}

static void swank_create_repl(struct repl_io *io, long id)
{
	int p = 0;
	char t[24];

	bputs(io->msgbuf, MSGSZ, &p,
	      "(:return (:ok (\"klisp-user\" \"klisp\")) ");
	snprintf(t, sizeof(t), "%ld)", id);
	bputs(io->msgbuf, MSGSZ, &p, t);
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);
}

/* Send (:return (:ok VAL) id) for a fixed VAL literal. */
static void swank_return_ok(struct repl_io *io, long id, const char *val)
{
	int p = 0;
	char t[24];

	bputs(io->msgbuf, MSGSZ, &p, "(:return (:ok ");
	bputs(io->msgbuf, MSGSZ, &p, val);
	snprintf(t, sizeof(t), ") %ld)", id);
	bputs(io->msgbuf, MSGSZ, &p, t);
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);
}

/* autodoc (eldoc-on-type): SLIME destructures the result as (doc &optional
 * cache-p), so it must be a non-empty list — returning nil makes SLIME's
 * process filter error and breaks typing. We have no docs, so :not-available. */
static void swank_autodoc(struct repl_io *io, long id)
{
	swank_return_ok(io, id, "(:not-available)");
}

/* completions (TAB): SLIME destructures as (completions partial). */
static void swank_completions(struct repl_io *io, long id)
{
	swank_return_ok(io, id, "(nil \"\")");
}

/* listener-eval: evaluate the user string, stream any (print ...) output, then
 * the printed result as a :repl-result, then close the rex with :ok. */
static void swank_listener_eval(struct repl_io *io, fe_Context *ctx,
				fe_Object *form, long id)
{
	fe_Object *arg = list_nth(ctx, form, 1);	/* the code string */
	fe_Object *res;
	int p;

	fe_tostring(ctx, arg, io->scratch, SCRATCHSZ);	/* user code */

	io->caplen = 0;
	io->capturing = true;
	res = eval_string(ctx, io->scratch);		/* may longjmp on error */
	io->capturing = false;

	if (io->caplen) {				/* (print ...) output */
		io->capbuf[io->caplen < CAPSZ ? io->caplen : CAPSZ - 1] = '\0';
		p = 0;
		bputs(io->msgbuf, MSGSZ, &p, "(:write-string ");
		bputstr(io->msgbuf, MSGSZ, &p, io->capbuf);
		bputs(io->msgbuf, MSGSZ, &p, ")");
		io->msgbuf[p] = '\0';
		swank_emit(io, io->msgbuf);
	}

	fe_tostring(ctx, res, io->scratch, SCRATCHSZ);	/* printed result */
	p = 0;
	bputs(io->msgbuf, MSGSZ, &p, "(:write-string ");
	{
		/* append result + newline as one escaped string */
		int q = (int)strlen(io->scratch);

		if (q < SCRATCHSZ - 1) {
			io->scratch[q] = '\n';
			io->scratch[q + 1] = '\0';
		}
	}
	bputstr(io->msgbuf, MSGSZ, &p, io->scratch);
	bputs(io->msgbuf, MSGSZ, &p, " :repl-result)");
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);

	swank_return_ok_nil(io, id);
}

/* interactive-eval (C-x C-e etc.): return the printed result as a string. */
static void swank_interactive_eval(struct repl_io *io, fe_Context *ctx,
				   fe_Object *form, long id)
{
	fe_Object *arg = list_nth(ctx, form, 1);
	fe_Object *res;
	int p;

	fe_tostring(ctx, arg, io->scratch, SCRATCHSZ);
	io->capturing = false;
	res = eval_string(ctx, io->scratch);
	fe_tostring(ctx, res, io->scratch, SCRATCHSZ);

	p = 0;
	bputs(io->msgbuf, MSGSZ, &p, "(:return (:ok ");
	bputstr(io->msgbuf, MSGSZ, &p, io->scratch);
	{
		char t[24];

		snprintf(t, sizeof(t), ") %ld)", id);
		bputs(io->msgbuf, MSGSZ, &p, t);
	}
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);
}

/* Dispatch one (:emacs-rex FORM PACKAGE THREAD ID). Matches on a substring of
 * the operator name so it works across SLIME's swank: / swank-repl: packages. */
static void swank_handle_rex(struct repl_io *io, fe_Context *ctx)
{
	struct strrd r = { .s = io->msgbuf, .pos = 0,
			   .len = (int)strlen(io->msgbuf) };
	fe_Object *rex = fe_read(ctx, strrd_fn, &r);
	fe_Object *form, *op;
	char opname[96];
	long id;

	if (!rex)
		return;
	form = list_nth(ctx, rex, 1);			/* the swank: form */
	op = fe_car(ctx, form);
	fe_tostring(ctx, op, opname, sizeof(opname));
	id = (long)fe_tonumber(ctx, list_nth(ctx, rex, 4));
	io->cur_id = id;

	if (strstr(opname, "connection-info"))
		swank_connection_info(io, id);
	else if (strstr(opname, "create-repl"))
		swank_create_repl(io, id);
	else if (strstr(opname, "listener-eval"))
		swank_listener_eval(io, ctx, form, id);
	else if (strstr(opname, "interactive-eval"))
		swank_interactive_eval(io, ctx, form, id);
	else if (strstr(opname, "autodoc"))
		swank_autodoc(io, id);
	else if (strstr(opname, "completions"))
		swank_completions(io, id);
	else
		swank_return_ok_nil(io, id);		/* require/arglist/... */
}

static void klisp_swank_serve(struct repl_io *io, fe_Context *ctx)
{
	int gc = fe_savegc(ctx);

	for (;;) {
		io->cur_id = -1;
		io->capturing = false;
		if (__builtin_setjmp(io->errjmp)) {
			fe_restoregc(ctx, gc);
			io->capturing = false;
			if (io->cur_id >= 0)
				swank_abort(io, io->cur_id,
					    io->errmsg[0] ? io->errmsg : "error");
			io->errmsg[0] = '\0';
			continue;
		}
		fe_restoregc(ctx, gc);
		if (kthread_should_stop() || io->eof)
			break;
		if (!swank_read_msg(io))
			break;
		swank_handle_rex(io, ctx);
	}
}
STACK_FRAME_NON_STANDARD(klisp_swank_serve);

/* ---- connection dispatch ----------------------------------------------- */

static void klisp_conn(struct socket *sock)
{
	struct repl_io *io;
	void *heap;
	fe_Context *ctx;
	int n;

	io = kzalloc(sizeof(*io), GFP_KERNEL);
	if (!io)
		return;
	heap = vmalloc(KLISP_FE_HEAP);
	if (!heap) {
		kfree(io);
		return;
	}

	io->sock = sock;
	ctx = fe_open(heap, KLISP_FE_HEAP);
	fe_handlers(ctx)->error = repl_onerror;
	g_io = io;

	/* Protocol detection: SLIME sends a length-prefixed message immediately,
	 * a human at nc does not. Peek the first chunk with a short timeout. */
	sock->sk->sk_rcvtimeo = msecs_to_jiffies(KLISP_DETECT_MS);
	{
		struct msghdr msg = { };
		struct kvec vec = { .iov_base = io->rbuf, .iov_len = RBUFSZ };

		n = kernel_recvmsg(sock, &msg, &vec, 1, RBUFSZ, 0);
		if (n > 0) {
			io->rlen = n;
			io->rpos = 0;
			/* SWANK: 6 hex digits then a '(' starting the sexp */
			if (n >= 7 && ishex(io->rbuf[0]) && ishex(io->rbuf[1]) &&
			    ishex(io->rbuf[2]) && ishex(io->rbuf[3]) &&
			    ishex(io->rbuf[4]) && ishex(io->rbuf[5]) &&
			    io->rbuf[6] == '(')
				io->swank = true;
		} else if (n == 0) {
			goto out;	/* closed before sending */
		}
	}
	sock->sk->sk_rcvtimeo = msecs_to_jiffies(KLISP_POLL_MS);

	if (io->swank)
		klisp_swank_serve(io, ctx);
	else
		klisp_raw_repl(io, ctx);

out:
	g_io = NULL;
	fe_close(ctx);
	vfree(heap);
	kfree(io);
}

/* ---- listener ---------------------------------------------------------- */

static int klisp_listen_fn(void *data)
{
	pr_info("listener thread up\n");

	while (!kthread_should_stop()) {
		struct socket *client = NULL;
		int err = kernel_accept(listen_sock, &client, 0);

		if (err == -EAGAIN || err == -EWOULDBLOCK ||
		    err == -ERESTARTSYS)
			continue;
		if (err < 0) {
			if (kthread_should_stop())
				break;
			msleep(KLISP_POLL_MS);
			continue;
		}

		klisp_conn(client);
		sock_release(client);
	}

	pr_info("listener thread exiting\n");
	return 0;
}

static int __init klisp_init(void)
{
	struct sockaddr_in addr = { };
	int err;

	if (in4_pton(bind_addr, -1, (u8 *)&addr.sin_addr.s_addr,
		     -1, NULL) != 1) {
		pr_err("invalid bind_addr '%s'\n", bind_addr);
		return -EINVAL;
	}
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &listen_sock);
	if (err < 0) {
		pr_err("sock_create_kern failed: %d\n", err);
		return err;
	}

	sock_set_reuseaddr(listen_sock->sk);
	listen_sock->sk->sk_rcvtimeo = msecs_to_jiffies(KLISP_POLL_MS);

	err = kernel_bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (err < 0) {
		pr_err("bind %s:%u failed: %d\n", bind_addr, port, err);
		goto err_release;
	}

	err = kernel_listen(listen_sock, KLISP_BACKLOG);
	if (err < 0) {
		pr_err("listen failed: %d\n", err);
		goto err_release;
	}

	listen_thread = kthread_run(klisp_listen_fn, NULL, "klispd");
	if (IS_ERR(listen_thread)) {
		err = PTR_ERR(listen_thread);
		pr_err("kthread_run failed: %d\n", err);
		listen_thread = NULL;
		goto err_release;
	}

	pr_info("listening on %s:%u (SWANK + raw REPL)\n", bind_addr, port);
	return 0;

err_release:
	sock_release(listen_sock);
	listen_sock = NULL;
	return err;
}

static void __exit klisp_exit(void)
{
	if (listen_thread) {
		if (listen_sock)
			kernel_sock_shutdown(listen_sock, SHUT_RDWR);
		kthread_stop(listen_thread);
		listen_thread = NULL;
	}
	if (listen_sock) {
		sock_release(listen_sock);
		listen_sock = NULL;
	}
	pr_info("unloaded\n");
}

module_init(klisp_init);
module_exit(klisp_exit);

/* Source is MIT (see LICENSE). "Dual MIT/GPL" is the kernel ident that keeps
 * access to GPL-only exported symbols without adding a license taint. */
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("klisp");
MODULE_DESCRIPTION("A Lisp machine inside the Linux kernel (M3: SWANK)");
MODULE_VERSION("0.0.3-m3");

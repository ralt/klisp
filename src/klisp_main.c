// SPDX-License-Identifier: MIT
/*
 * klisp — a Lisp machine inside the Linux kernel.
 *
 * Milestone M2: a Lisp REPL over TCP. A kthread accepts a connection and runs
 * a read-eval-print loop driven by the vendored `fe` interpreter (see
 * vendor/fe, DESIGN.md §4-5). Errors (bad input, unbound symbols, divide by
 * zero, runaway recursion) are caught and returned to the client; they must
 * never crash the kernel.
 *
 * Connect from the host with:  nc localhost 4005   then type e.g.  (+ 1 2)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
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
#define KLISP_FE_HEAP	(256 * 1024)	/* fe object arena, per connection */

static ushort port = 4005;
module_param(port, ushort, 0444);
MODULE_PARM_DESC(port, "TCP port to listen on (default 4005)");

static char *bind_addr = "127.0.0.1";
module_param(bind_addr, charp, 0444);
MODULE_PARM_DESC(bind_addr, "IPv4 address to bind (default 127.0.0.1)");

static struct socket *listen_sock;
static struct task_struct *listen_thread;

/* Per-connection REPL I/O state. Serves one connection at a time (the listener
 * handles connections inline), so a single global pointer suffices for the
 * callbacks fe invokes without a udata channel (error/print). */
struct repl_io {
	struct socket *sock;
	klisp_jmp_buf errjmp;	/* REPL top-level, jumped to on error */
	char rbuf[512];
	int rlen, rpos;
	bool eof;
	char wbuf[1024];
	int wlen;
	char errmsg[160];	/* pending error, reported at the REPL top level */
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
	if (io->wlen >= (int)sizeof(io->wbuf))
		repl_flush(io);
	io->wbuf[io->wlen++] = c;
}

static void repl_puts(struct repl_io *io, const char *s)
{
	while (*s)
		repl_putc(io, *s++);
}

/* fe_ReadFn: hand fe one character at a time, refilling from the socket and
 * blocking (with a timeout so unload can interrupt) when the buffer drains. */
static char repl_readfn(fe_Context *ctx, void *udata)
{
	struct repl_io *io = udata;

	(void)ctx;
	while (io->rpos >= io->rlen) {
		struct msghdr msg = { };
		struct kvec vec = {
			.iov_base = io->rbuf,
			.iov_len = sizeof(io->rbuf),
		};
		int n;

		if (kthread_should_stop()) {
			io->eof = true;
			return '\0';
		}
		n = kernel_recvmsg(io->sock, &msg, &vec, 1, sizeof(io->rbuf), 0);
		if (n == -EAGAIN || n == -EWOULDBLOCK || n == -ERESTARTSYS)
			continue;
		if (n <= 0) {
			io->eof = true;
			return '\0';
		}
		io->rlen = n;
		io->rpos = 0;
	}
	return io->rbuf[io->rpos++];
}

/* ---- callbacks fe needs (declared in vendor/fe/fe_port.h) -------------- */

/* fe_WriteFn used for (print ...) and for printing eval results. */
void klisp_write_char(fe_Context *ctx, void *udata, char chr)
{
	(void)ctx;
	(void)udata;
	if (g_io)
		repl_putc(g_io, chr);
}

void klisp_emergency_longjmp(void)
{
	if (g_io)
		__builtin_longjmp(g_io->errjmp, 1);
	pr_err("fe error with no active REPL context\n");
}
/* __builtin_longjmp is a non-local jump objtool can't model (it sees a sibling
 * call with a modified stack frame). The jump is intentional; exempt it. */
STACK_FRAME_NON_STANDARD(klisp_emergency_longjmp);

/* Integer strtod replacement (base 10, optional sign). On no digits, *end == s
 * so fe treats the token as a symbol (e.g. the primitives +, -, *, /). */
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

/* fe error handler. Stash the message and unwind to the REPL top level; do NOT
 * touch the socket here. fe_error can fire deep in the evaluator's recursion
 * (e.g. the stack-depth guard), and the network TX path needs several KB of
 * stack — sending from here risks overflowing the kernel stack. The top-level
 * setjmp handler reports the message once the stack has unwound. */
static void repl_onerror(fe_Context *ctx, const char *msg, fe_Object *cl)
{
	(void)ctx;
	(void)cl;
	if (g_io) {
		strscpy(g_io->errmsg, msg, sizeof(g_io->errmsg));
		__builtin_longjmp(g_io->errjmp, 1);
	}
}
STACK_FRAME_NON_STANDARD(repl_onerror); /* see klisp_emergency_longjmp */

/* ---- the REPL ---------------------------------------------------------- */

static void klisp_repl_conn(struct socket *sock)
{
	struct repl_io *io;
	void *heap;
	fe_Context *ctx;
	int gc;

	io = kzalloc(sizeof(*io), GFP_KERNEL);
	if (!io)
		return;
	heap = vmalloc(KLISP_FE_HEAP);
	if (!heap) {
		kfree(io);
		return;
	}

	io->sock = sock;
	sock->sk->sk_rcvtimeo = msecs_to_jiffies(KLISP_POLL_MS);
	ctx = fe_open(heap, KLISP_FE_HEAP);
	fe_handlers(ctx)->error = repl_onerror;
	g_io = io;

	repl_puts(io, "klisp Lisp REPL — try (+ 1 2), (= x 10), (* x x); errors recover\n> ");
	repl_flush(io);

	gc = fe_savegc(ctx);
	if (__builtin_setjmp(io->errjmp)) {
		/* arrived here after an error; report it now that the stack has
		 * unwound (safe to do socket I/O), then re-prompt */
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
			break;	/* EOF / disconnect */
		obj = fe_eval(ctx, obj);
		fe_write(ctx, obj, klisp_write_char, NULL, 0);
		repl_puts(io, "\n> ");
		repl_flush(io);
	}

	g_io = NULL;
	fe_close(ctx);
	vfree(heap);
	kfree(io);
}
/* Contains __builtin_setjmp (the longjmp target); objtool can't model it. */
STACK_FRAME_NON_STANDARD(klisp_repl_conn);

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

		klisp_repl_conn(client);
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

	pr_info("listening on %s:%u (Lisp REPL)\n", bind_addr, port);
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
MODULE_DESCRIPTION("A Lisp machine inside the Linux kernel (M2: Lisp REPL)");
MODULE_VERSION("0.0.2-m2");

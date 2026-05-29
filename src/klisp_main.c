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
#include <linux/sched/signal.h>		/* for_each_process */
#include <linux/mm.h>			/* si_meminfo */
#include <linux/sysinfo.h>
#include <linux/utsname.h>
#include <linux/netdevice.h>		/* for_each_netdev_rcu */
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <linux/objtool.h>
#include <net/net_namespace.h>		/* init_net */
#include <net/sock.h>

#include "fe.h"

#define KLISP_BACKLOG	8
#define KLISP_POLL_MS	500
#define KLISP_DETECT_MS	500		/* wait for the first byte to pick protocol */
#define KLISP_FE_HEAP	(256 * 1024)	/* fe object arena, per connection */

#define RBUFSZ		512
#define WBUFSZ		1024
#define MSGSZ		8192		/* max SWANK message / response payload */
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
	void *kobj_scratch;		/* kmalloc'd snapshot, freed on error unwind */
	bool presentations;		/* client requested SLIME presentations */
	long pres_next;			/* next presentation id */
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

/* ---- read-only kernel objects (DESIGN.md §6) --------------------------- *
 * Each builtin snapshots live kernel state under the right lock into a plain C
 * array, releases the lock, then builds immutable Lisp plists from the copy.
 * Lisp never sees a raw kernel pointer, so a REPL bug cannot corrupt or
 * use-after-free kernel state. The kmalloc'd snapshot is parked in
 * g_io->kobj_scratch so the error-unwind path frees it if fe_error fires while
 * building the result.
 */

#define KOBJ_MAX	1024

static const char *task_state_str(unsigned int s)
{
	if (s == TASK_RUNNING)			return "R";
	if (s & TASK_UNINTERRUPTIBLE)		return "D";
	if (s & TASK_INTERRUPTIBLE)		return "S";
	if (s & __TASK_STOPPED)			return "T";
	if (s & TASK_DEAD)			return "X";
	return "?";
}

/* Build (key val key val ...) from interleaved fe objects. */
static fe_Object *plist(fe_Context *ctx, fe_Object **kv, int n)
{
	return fe_list(ctx, kv, n);
}

struct psnap { pid_t pid, ppid; unsigned int state; char comm[TASK_COMM_LEN]; };

static fe_Object *cf_list_processes(fe_Context *ctx, fe_Object *args)
{
	struct psnap *arr;
	struct task_struct *p;
	fe_Object *res;
	int n = 0, i, gc;

	(void)args;
	arr = kmalloc_array(KOBJ_MAX, sizeof(*arr), GFP_KERNEL);
	if (!arr)
		fe_error(ctx, "out of memory");
	g_io->kobj_scratch = arr;

	rcu_read_lock();
	for_each_process(p) {
		if (n >= KOBJ_MAX)
			break;
		arr[n].pid = task_pid_nr(p);
		arr[n].ppid = task_ppid_nr(p);
		arr[n].state = READ_ONCE(p->__state);
		memcpy(arr[n].comm, p->comm, TASK_COMM_LEN);
		arr[n].comm[TASK_COMM_LEN - 1] = '\0';
		n++;
	}
	rcu_read_unlock();

	res = fe_bool(ctx, 0);			/* nil */
	gc = fe_savegc(ctx);
	for (i = n - 1; i >= 0; i--) {
		fe_Object *kv[8];

		fe_restoregc(ctx, gc);
		fe_pushgc(ctx, res);
		kv[0] = fe_symbol(ctx, ":pid");   kv[1] = fe_number(ctx, arr[i].pid);
		kv[2] = fe_symbol(ctx, ":comm");  kv[3] = fe_string(ctx, arr[i].comm);
		kv[4] = fe_symbol(ctx, ":ppid");  kv[5] = fe_number(ctx, arr[i].ppid);
		kv[6] = fe_symbol(ctx, ":state");
		kv[7] = fe_string(ctx, task_state_str(arr[i].state));
		res = fe_cons(ctx, plist(ctx, kv, 8), res);
	}

	g_io->kobj_scratch = NULL;
	kfree(arr);
	return res;
}

struct nsnap { char name[IFNAMSIZ]; int ifindex; unsigned int mtu, flags; };

static fe_Object *cf_list_netdevs(fe_Context *ctx, fe_Object *args)
{
	struct nsnap *arr;
	struct net_device *dev;
	fe_Object *res;
	int n = 0, i, gc;

	(void)args;
	arr = kmalloc_array(KOBJ_MAX, sizeof(*arr), GFP_KERNEL);
	if (!arr)
		fe_error(ctx, "out of memory");
	g_io->kobj_scratch = arr;

	rcu_read_lock();
	for_each_netdev_rcu(&init_net, dev) {
		if (n >= KOBJ_MAX)
			break;
		memcpy(arr[n].name, dev->name, IFNAMSIZ);
		arr[n].name[IFNAMSIZ - 1] = '\0';
		arr[n].ifindex = dev->ifindex;
		arr[n].mtu = dev->mtu;
		arr[n].flags = dev->flags;
		n++;
	}
	rcu_read_unlock();

	res = fe_bool(ctx, 0);
	gc = fe_savegc(ctx);
	for (i = n - 1; i >= 0; i--) {
		fe_Object *kv[10];

		fe_restoregc(ctx, gc);
		fe_pushgc(ctx, res);
		kv[0] = fe_symbol(ctx, ":name");  kv[1] = fe_string(ctx, arr[i].name);
		kv[2] = fe_symbol(ctx, ":index"); kv[3] = fe_number(ctx, arr[i].ifindex);
		kv[4] = fe_symbol(ctx, ":mtu");   kv[5] = fe_number(ctx, arr[i].mtu);
		kv[6] = fe_symbol(ctx, ":up");
		kv[7] = fe_bool(ctx, arr[i].flags & IFF_UP);
		kv[8] = fe_symbol(ctx, ":flags"); kv[9] = fe_number(ctx, arr[i].flags);
		res = fe_cons(ctx, plist(ctx, kv, 10), res);
	}

	g_io->kobj_scratch = NULL;
	kfree(arr);
	return res;
}

static fe_Object *cf_meminfo(fe_Context *ctx, fe_Object *args)
{
	struct sysinfo si = {};
	fe_Object *kv[8];
	unsigned long unit;

	(void)args;
	si_meminfo(&si);
	unit = si.mem_unit ? si.mem_unit : 1;
	/* report in KiB */
	kv[0] = fe_symbol(ctx, ":totalram-kb");
	kv[1] = fe_number(ctx, (long)((si.totalram * unit) >> 10));
	kv[2] = fe_symbol(ctx, ":freeram-kb");
	kv[3] = fe_number(ctx, (long)((si.freeram * unit) >> 10));
	kv[4] = fe_symbol(ctx, ":bufferram-kb");
	kv[5] = fe_number(ctx, (long)((si.bufferram * unit) >> 10));
	kv[6] = fe_symbol(ctx, ":sharedram-kb");
	kv[7] = fe_number(ctx, (long)((si.sharedram * unit) >> 10));
	return plist(ctx, kv, 8);
}

static fe_Object *cf_uname(fe_Context *ctx, fe_Object *args)
{
	struct new_utsname *u = utsname();
	fe_Object *kv[8];

	(void)args;
	kv[0] = fe_symbol(ctx, ":sysname"); kv[1] = fe_string(ctx, u->sysname);
	kv[2] = fe_symbol(ctx, ":release"); kv[3] = fe_string(ctx, u->release);
	kv[4] = fe_symbol(ctx, ":version"); kv[5] = fe_string(ctx, u->version);
	kv[6] = fe_symbol(ctx, ":machine"); kv[7] = fe_string(ctx, u->machine);
	return plist(ctx, kv, 8);
}

/* (env): every interned symbol; (functions): just the callable ones. */
static fe_Object *cf_env(fe_Context *ctx, fe_Object *args)
{
	fe_Object *res = fe_bool(ctx, 0), *s;
	int gc = fe_savegc(ctx);

	(void)args;
	for (s = fe_symbols(ctx); !fe_isnil(ctx, s); s = fe_cdr(ctx, s)) {
		fe_restoregc(ctx, gc);
		fe_pushgc(ctx, res);
		res = fe_cons(ctx, fe_car(ctx, s), res);
	}
	return res;
}

static int sym_is_callable(fe_Context *ctx, fe_Object *sym)
{
	int t = fe_type(ctx, fe_eval(ctx, sym));

	return t == FE_TPRIM || t == FE_TCFUNC ||
	       t == FE_TFUNC || t == FE_TMACRO;
}

static fe_Object *cf_functions(fe_Context *ctx, fe_Object *args)
{
	fe_Object *res = fe_bool(ctx, 0), *s, *sym;
	int gc = fe_savegc(ctx);

	(void)args;
	for (s = fe_symbols(ctx); !fe_isnil(ctx, s); s = fe_cdr(ctx, s)) {
		fe_restoregc(ctx, gc);
		fe_pushgc(ctx, res);
		sym = fe_car(ctx, s);
		if (sym_is_callable(ctx, sym))
			res = fe_cons(ctx, sym, res);
	}
	return res;
}

/* List helpers fe lacks. Iterative (no recursion), so big lists are safe. */
static fe_Object *cf_length(fe_Context *ctx, fe_Object *args)
{
	fe_Object *l = fe_nextarg(ctx, &args);
	long n = 0;

	while (fe_type(ctx, l) == FE_TPAIR) {
		n++;
		l = fe_cdr(ctx, l);
	}
	return fe_number(ctx, n);
}

static fe_Object *cf_nth(fe_Context *ctx, fe_Object *args)
{
	long n = (long)fe_tonumber(ctx, fe_nextarg(ctx, &args));
	fe_Object *l = fe_nextarg(ctx, &args);

	while (n-- > 0 && fe_type(ctx, l) == FE_TPAIR)
		l = fe_cdr(ctx, l);
	return fe_car(ctx, l);
}

static void klisp_register_kernel_builtins(fe_Context *ctx)
{
	int gc = fe_savegc(ctx);
	struct { const char *name; fe_CFunc fn; } tbl[] = {
		{ "list-processes", cf_list_processes },
		{ "list-netdevs",   cf_list_netdevs },
		{ "meminfo",        cf_meminfo },
		{ "uname",          cf_uname },
		{ "env",            cf_env },
		{ "functions",      cf_functions },
		{ "length",         cf_length },
		{ "nth",            cf_nth },
	};
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(tbl); i++) {
		fe_set(ctx, fe_symbol(ctx, tbl[i].name),
		       fe_cfunc(ctx, tbl[i].fn));
		fe_restoregc(ctx, gc);
	}
}

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
		if (io->kobj_scratch) {		/* freed here if error hit mid-build */
			kfree(io->kobj_scratch);
			io->kobj_scratch = NULL;
		}
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

/* completions (TAB): return ((match ...) prefix) of interned symbols whose
 * name starts with the typed prefix. SLIME destructures as (completions
 * partial), so the value is always a 2-list. */
static void swank_completions(struct repl_io *io, fe_Context *ctx,
			     fe_Object *form, long id)
{
	fe_Object *arg = list_nth(ctx, form, 1);	/* prefix string */
	char pref[96], name[96];
	fe_Object *s;
	int plen, p = 0;

	fe_tostring(ctx, arg, pref, sizeof(pref));
	plen = (int)strlen(pref);

	bputs(io->msgbuf, MSGSZ, &p, "(:return (:ok ((");
	for (s = fe_symbols(ctx); !fe_isnil(ctx, s); s = fe_cdr(ctx, s)) {
		fe_tostring(ctx, fe_car(ctx, s), name, sizeof(name));
		if (!strncmp(name, pref, plen)) {
			bputstr(io->msgbuf, MSGSZ, &p, name);
			bput(io->msgbuf, MSGSZ, &p, ' ');
		}
	}
	bputs(io->msgbuf, MSGSZ, &p, ") ");
	bputstr(io->msgbuf, MSGSZ, &p, pref);
	{
		char t[24];

		snprintf(t, sizeof(t), ")) %ld)", id);
		bputs(io->msgbuf, MSGSZ, &p, t);
	}
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);
}

static long present(struct repl_io *io, fe_Context *ctx, fe_Object *obj);

/* listener-eval: evaluate the user string, stream any (print ...) output, then
 * the printed result as a :repl-result, then close the rex with :ok. When the
 * client supports presentations, the result is bracketed so it's clickable. */
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

	if (io->presentations) {
		/* Bracket the value so SLIME makes it a clickable presentation
		 * mapped to this object; the trailing newline stays outside. */
		long pid = present(io, ctx, res);
		char t[40];

		p = 0;
		snprintf(t, sizeof(t), "(:presentation-start %ld :repl-result)", pid);
		bputs(io->msgbuf, MSGSZ, &p, t);
		io->msgbuf[p] = '\0';
		swank_emit(io, io->msgbuf);

		p = 0;
		bputs(io->msgbuf, MSGSZ, &p, "(:write-string ");
		bputstr(io->msgbuf, MSGSZ, &p, io->scratch);
		bputs(io->msgbuf, MSGSZ, &p, " :repl-result)");
		io->msgbuf[p] = '\0';
		swank_emit(io, io->msgbuf);

		p = 0;
		snprintf(t, sizeof(t), "(:presentation-end %ld :repl-result)", pid);
		bputs(io->msgbuf, MSGSZ, &p, t);
		io->msgbuf[p] = '\0';
		swank_emit(io, io->msgbuf);

		swank_emit(io, "(:write-string \"\\n\" :repl-result)");
	} else {
		int q = (int)strlen(io->scratch);

		if (q < SCRATCHSZ - 1) {	/* append newline */
			io->scratch[q] = '\n';
			io->scratch[q + 1] = '\0';
		}
		p = 0;
		bputs(io->msgbuf, MSGSZ, &p, "(:write-string ");
		bputstr(io->msgbuf, MSGSZ, &p, io->scratch);
		bputs(io->msgbuf, MSGSZ, &p, " :repl-result)");
		io->msgbuf[p] = '\0';
		swank_emit(io, io->msgbuf);
	}

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

/* ---- inspector --------------------------------------------------------- *
 * Inspector state lives in two interned symbols so the GC keeps it alive
 * across rex round-trips (symbols are GC roots; a symbol's value is marked):
 *   %insp-cur%   - the object currently being inspected
 *   %insp-stack% - list of previously-inspected objects, for "pop"
 * Lists render with one clickable (:value ...) per element; clicking sends
 * inspect-nth-part, which drills into that already-snapshotted sub-object.
 */
#define INSP_CUR	"%insp-cur%"
#define INSP_STACK	"%insp-stack%"

static fe_Object *sym_get(fe_Context *ctx, const char *n)
{
	return fe_eval(ctx, fe_symbol(ctx, n));
}
static void sym_put(fe_Context *ctx, const char *n, fe_Object *v)
{
	fe_set(ctx, fe_symbol(ctx, n), v);
}

/* Presentation table: %present% holds an alist ((id . obj) ...), newest first,
 * capped at PRESENT_MAX so a long session can't pin the whole heap. GC-rooted
 * via the symbol. Used to make REPL-result output clickable/inspectable. */
#define PRESENT_SYM	"%present%"
#define PRESENT_MAX	64

static long present(struct repl_io *io, fe_Context *ctx, fe_Object *obj)
{
	long id = io->pres_next++;
	fe_Object *items[PRESENT_MAX], *p;
	int n = 0;

	items[n++] = fe_cons(ctx, fe_number(ctx, id), obj);	/* (id . obj) */
	for (p = sym_get(ctx, PRESENT_SYM);
	     n < PRESENT_MAX && fe_type(ctx, p) == FE_TPAIR; p = fe_cdr(ctx, p))
		items[n++] = fe_car(ctx, p);
	sym_put(ctx, PRESENT_SYM, fe_list(ctx, items, n));
	return id;
}

static fe_Object *present_lookup(fe_Context *ctx, long id)
{
	fe_Object *p, *cell;

	for (p = sym_get(ctx, PRESENT_SYM); fe_type(ctx, p) == FE_TPAIR;
	     p = fe_cdr(ctx, p)) {
		cell = fe_car(ctx, p);
		if ((long)fe_tonumber(ctx, fe_car(ctx, cell)) == id)
			return fe_cdr(ctx, cell);
	}
	return fe_bool(ctx, 0);	/* nil — evicted or unknown */
}

/* Emit (:return (:ok (:title T :id 0 :content (PIECES LEN 0 LEN))) id). */
static void inspector_render(struct repl_io *io, fe_Context *ctx,
			     fe_Object *o, long id)
{
	int p = 0, npieces = 0;
	char buf[160], t[64];
	int is_pair = (fe_type(ctx, o) == FE_TPAIR);

	if (is_pair)
		strscpy(buf, "List", sizeof(buf));
	else
		fe_tostring(ctx, o, buf, sizeof(buf));

	bputs(io->msgbuf, MSGSZ, &p, "(:return (:ok (:title ");
	bputstr(io->msgbuf, MSGSZ, &p, buf);
	bputs(io->msgbuf, MSGSZ, &p, " :id 0 :content ((");

	if (is_pair) {
		fe_Object *e = o;
		int i = 0;

		while (fe_type(ctx, e) == FE_TPAIR && p < MSGSZ - 256) {
			fe_tostring(ctx, fe_car(ctx, e), buf, sizeof(buf));
			bputs(io->msgbuf, MSGSZ, &p, "(:value ");
			bputstr(io->msgbuf, MSGSZ, &p, buf);
			snprintf(t, sizeof(t), " %d) ", i);
			bputs(io->msgbuf, MSGSZ, &p, t);
			bputstr(io->msgbuf, MSGSZ, &p, "\n");
			bput(io->msgbuf, MSGSZ, &p, ' ');
			npieces += 2;
			e = fe_cdr(ctx, e);
			i++;
		}
	} else {
		fe_tostring(ctx, o, buf, sizeof(buf));
		bputstr(io->msgbuf, MSGSZ, &p, buf);
		npieces = 1;
	}

	snprintf(t, sizeof(t), ") %d 0 %d))) %ld)", npieces, npieces, id);
	bputs(io->msgbuf, MSGSZ, &p, t);
	io->msgbuf[p] = '\0';
	swank_emit(io, io->msgbuf);
}

static void swank_init_inspector(struct repl_io *io, fe_Context *ctx,
				 fe_Object *form, long id)
{
	fe_Object *o;

	fe_tostring(ctx, list_nth(ctx, form, 1), io->scratch, SCRATCHSZ);
	o = eval_string(ctx, io->scratch);	/* may longjmp on error */
	sym_put(ctx, INSP_CUR, o);
	sym_put(ctx, INSP_STACK, fe_bool(ctx, 0));
	inspector_render(io, ctx, o, id);
}

static void swank_inspect_nth_part(struct repl_io *io, fe_Context *ctx,
				   fe_Object *form, long id)
{
	long n = (long)fe_tonumber(ctx, list_nth(ctx, form, 1));
	fe_Object *cur = sym_get(ctx, INSP_CUR);
	fe_Object *part = list_nth(ctx, cur, (int)n);

	sym_put(ctx, INSP_STACK, fe_cons(ctx, cur, sym_get(ctx, INSP_STACK)));
	sym_put(ctx, INSP_CUR, part);
	inspector_render(io, ctx, part, id);
}

static void swank_inspector_pop(struct repl_io *io, fe_Context *ctx, long id)
{
	fe_Object *st = sym_get(ctx, INSP_STACK);

	if (fe_isnil(ctx, st)) {
		swank_return_ok_nil(io, id);
		return;
	}
	sym_put(ctx, INSP_CUR, fe_car(ctx, st));
	sym_put(ctx, INSP_STACK, fe_cdr(ctx, st));
	inspector_render(io, ctx, sym_get(ctx, INSP_CUR), id);
}

/* SLIME quotes some args, e.g. (swank:inspect-presentation 'ID nil). Unwrap a
 * leading (quote X) -> X. */
static fe_Object *unquote(fe_Context *ctx, fe_Object *o)
{
	char head[8];

	if (fe_type(ctx, o) == FE_TPAIR) {
		fe_tostring(ctx, fe_car(ctx, o), head, sizeof(head));
		if (!strcmp(head, "quote"))
			return fe_car(ctx, fe_cdr(ctx, o));
	}
	return o;
}

/* Clicking a REPL-output presentation: inspect the object behind that id. */
static void swank_inspect_presentation(struct repl_io *io, fe_Context *ctx,
				       fe_Object *form, long id)
{
	long pid = (long)fe_tonumber(ctx, unquote(ctx, list_nth(ctx, form, 1)));
	fe_Object *o = present_lookup(ctx, pid);

	sym_put(ctx, INSP_CUR, o);
	sym_put(ctx, INSP_STACK, fe_bool(ctx, 0));
	inspector_render(io, ctx, o, id);
}

/* swank-require: note whether the client wants presentations so we only emit
 * presentation markers to clients that understand them. */
static void swank_swank_require(struct repl_io *io, fe_Context *ctx,
				fe_Object *form, long id)
{
	char buf[256];

	fe_tostring(ctx, form, buf, sizeof(buf));
	if (strstr(buf, "presentation"))
		io->presentations = true;
	swank_return_ok_nil(io, id);
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
		swank_completions(io, ctx, form, id);
	else if (strstr(opname, "init-inspector"))
		swank_init_inspector(io, ctx, form, id);
	else if (strstr(opname, "inspect-presentation"))
		swank_inspect_presentation(io, ctx, form, id);
	else if (strstr(opname, "nth-part"))
		swank_inspect_nth_part(io, ctx, form, id);
	else if (strstr(opname, "inspector-pop"))
		swank_inspector_pop(io, ctx, id);
	else if (strstr(opname, "swank-require"))
		swank_swank_require(io, ctx, form, id);
	else if (strstr(opname, "quit-inspector"))
		swank_return_ok_nil(io, id);		/* state is per-eval; nothing to drop */
	else
		swank_return_ok_nil(io, id);		/* require/arglist/inspector-range/... */
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
			if (io->kobj_scratch) {
				kfree(io->kobj_scratch);
				io->kobj_scratch = NULL;
			}
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
	klisp_register_kernel_builtins(ctx);
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
	if (io->kobj_scratch)
		kfree(io->kobj_scratch);
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

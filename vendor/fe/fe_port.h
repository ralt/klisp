/* klisp: kernel-compatibility shim for the vendored `fe` Lisp.
 *
 * fe is written for a hosted libc environment; the kernel has none. This header
 * replaces what fe pulls from <stdlib.h>/<stdio.h>/<string.h>/<setjmp.h> with
 * kernel equivalents. It is included from fe.h (in place of the libc headers),
 * so both fe.c and the module see it. See vendor/fe/UPSTREAM and DESIGN.md §4.
 */
#ifndef KLISP_FE_PORT_H
#define KLISP_FE_PORT_H

#include <linux/types.h>
#include <linux/string.h>   /* memcpy, memset, strlen, strchr, strcmp */
#include <linux/kernel.h>   /* snprintf */
#include <linux/printk.h>   /* pr_* */
#include <linux/sched.h>    /* current, task_stack_page */
#include <linux/sched/task_stack.h>

struct fe_Context;

/* __builtin_setjmp/longjmp need a 5-word buffer and require no libc. We use
 * them as the interpreter's non-local exit back to the REPL top level. */
typedef void *klisp_jmp_buf[5];

/* Integer replacement for strtod, used by fe's reader to detect numbers.
 * Mirrors strtod's contract: on no valid number, *end == s. */
long klisp_strtod(const char *s, char **end);
#define strtod(s, e) klisp_strtod((s), (e))

/* (print ...) output is routed to the active REPL connection (defined in the
 * module). Signature matches fe_WriteFn. */
void klisp_write_char(struct fe_Context *ctx, void *udata, char chr);

/* Non-local exit to the REPL top level when fe_error has no installed handler
 * (defensive — a handler is normally always set). */
void klisp_emergency_longjmp(void);

/* Stack-exhaustion guard for the recursive evaluator: the kernel stack is tiny
 * (~16 KB), so deep/runaway recursion must raise a Lisp error before it
 * overflows the C stack and oopses. */
#define KLISP_STACK_MARGIN 2048
static inline int klisp_stack_low(void)
{
	char probe;
	return ((unsigned long)&probe -
		(unsigned long)task_stack_page(current)) < KLISP_STACK_MARGIN;
}

#endif /* KLISP_FE_PORT_H */

// SPDX-License-Identifier: MIT
/*
 * klisp — a Lisp machine inside the Linux kernel.
 *
 * Milestone M1: prove the dangerous plumbing. This module stands up a TCP
 * listener in a kthread and echoes whatever a client sends. No Lisp yet — the
 * point is a working, cleanly-unloadable kernel socket server we can build the
 * SWANK/REPL layers on top of (see DESIGN.md §3, §9).
 *
 * Connect from the host with:  nc localhost 4005
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <net/sock.h>

#define KLISP_BACKLOG	8
#define KLISP_BUFSZ	1024
/* Poll granularity so blocking accept()/recvmsg() periodically return and we
 * can observe kthread_should_stop() during module unload. */
#define KLISP_POLL_MS	500

static ushort port = 4005;
module_param(port, ushort, 0444);
MODULE_PARM_DESC(port, "TCP port to listen on (default 4005)");

/*
 * Default to loopback: an in-kernel echo/eval endpoint must not be exposed on
 * the network (DESIGN.md §8 "Deployment target"). QEMU dev passes
 * bind_addr=0.0.0.0 because SLIRP host-forwarding lands on the guest's NIC
 * address, not 127.0.0.1.
 */
static char *bind_addr = "127.0.0.1";
module_param(bind_addr, charp, 0444);
MODULE_PARM_DESC(bind_addr, "IPv4 address to bind (default 127.0.0.1)");

static struct socket *listen_sock;
static struct task_struct *listen_thread;

/* Echo everything from one connected client until it disconnects or we unload. */
static void klisp_echo_conn(struct socket *sock)
{
	char *buf;

	buf = kmalloc(KLISP_BUFSZ, GFP_KERNEL);
	if (!buf)
		return;

	/* Time out recvmsg so the loop can notice kthread_should_stop(). */
	sock->sk->sk_rcvtimeo = msecs_to_jiffies(KLISP_POLL_MS);

	while (!kthread_should_stop()) {
		struct msghdr msg = { };
		struct kvec vec = { .iov_base = buf, .iov_len = KLISP_BUFSZ };
		int n, off;

		n = kernel_recvmsg(sock, &msg, &vec, 1, KLISP_BUFSZ, 0);
		if (n == -EAGAIN || n == -EWOULDBLOCK || n == -ERESTARTSYS)
			continue;
		if (n <= 0)	/* 0 = orderly close, <0 = error */
			break;

		for (off = 0; off < n && !kthread_should_stop(); ) {
			struct msghdr smsg = { };
			struct kvec svec = {
				.iov_base = buf + off,
				.iov_len = n - off,
			};
			int s = kernel_sendmsg(sock, &smsg, &svec, 1, n - off);

			if (s == -EAGAIN || s == -EWOULDBLOCK ||
			    s == -ERESTARTSYS)
				continue;
			if (s <= 0)
				goto out;
			off += s;
		}
	}
out:
	kfree(buf);
}

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
			/* Listen socket was shut down (unload) or transient. */
			if (kthread_should_stop())
				break;
			msleep(KLISP_POLL_MS);
			continue;
		}

		klisp_echo_conn(client);
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
	/* Bound accept() latency so unload doesn't hang up to forever. */
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

	pr_info("listening on %s:%u (echo)\n", bind_addr, port);
	return 0;

err_release:
	sock_release(listen_sock);
	listen_sock = NULL;
	return err;
}

static void __exit klisp_exit(void)
{
	if (listen_thread) {
		/* Interrupt a blocked accept(), then join the thread before
		 * tearing down the socket it was using. */
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
MODULE_DESCRIPTION("A Lisp machine inside the Linux kernel (M1: TCP echo)");
MODULE_VERSION("0.0.1-m1");

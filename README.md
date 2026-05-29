# klisp

*A Lisp machine inside the Linux kernel.*

klisp is a Linux kernel module that embeds a small Lisp interpreter and exposes
the running kernel as a live Lisp environment. You load the module, point Emacs
(SLIME) at a TCP port, and get a REPL whose world **is** the kernel — you can
list and inspect kernel objects (processes, modules, network devices, memory)
as ordinary Lisp data, and navigate them with the SLIME inspector.

## What it's for

Exploring and understanding a live kernel, interactively, the way you'd poke at
a running image in a Lisp REPL — instead of writing a one-off module, recompiling,
and reading `dmesg` every time you want to ask the kernel a question.

- **Inspect, don't guess.** Walk the process list, open a `task_struct` as a
  plist, click into its parent and children in the SLIME inspector.
- **A REPL, not a dump.** Evaluate expressions against kernel state and build up
  queries incrementally, keeping results around as Lisp values.
- **Read-only first, safe by design.** v1 never hands Lisp a raw kernel pointer:
  objects are *snapshotted* into immutable Lisp data under the proper locks, so
  a REPL typo can't corrupt kernel state. (Calling methods / mutating state is a
  deliberate later step.)

It's equal parts a practical kernel-exploration tool and an answer to a
long-standing "could you actually turn the kernel into a Lisp machine?" itch.

## Why this is interesting (and hard)

The kernel has no libc, no floating point, a tiny stack, and a bug can take down
the machine. So klisp:

- embeds [`fe`](https://github.com/rxi/fe), a tiny embeddable C Lisp, patched to
  run in-kernel (integer-only, no `setjmp`, GC over a `kmalloc`'d buffer);
- hand-rolls a **minimal SWANK server** (the SLIME protocol) rather than porting
  the real one, which assumes a full Common Lisp with stream I/O;
- treats resilience as a goal: errors should *warn and recover*, not panic —
  Lisp-level mistakes are caught by construction, and the module is cleanly
  reloadable.

The full architecture and rationale live in [`DESIGN.md`](./DESIGN.md).

## How it works

```
  Emacs / SLIME  ──TCP (hostfwd)──▶  klisp.ko in the kernel
                                       ├─ kthread: accept loop + I/O
                                       ├─ SWANK layer (framing, REPL, inspector)
                                       ├─ Lisp core (fe, patched)
                                       └─ kernel-object layer (snapshot → plist)
```

Development happens in a QEMU VM (a module bug there costs a VM restart, not your
machine); running on the real host kernel is the eventual release goal.

## Status

Early. Built in milestones:

- **M1 — done.** A TCP echo server in a kernel module, built in Docker and
  verified in QEMU (clean `rmmod`/reload, no panics). This is the socket+thread
  foundation; there is no Lisp in it yet.
- **M2+ — upcoming.** Vendor and port `fe`, then the SWANK REPL, read-only
  kernel objects, and the inspector. See `DESIGN.md` §9 for the roadmap.

## Quickstart (M1: build → boot → echo)

Prerequisites: Docker (you can run `docker`), `qemu-system-x86_64`, a Debian/
`apt` host (scripts use `apt-get download`, no root needed), and ideally
read/write on `/dev/kvm` for speed.

```sh
scripts/build.sh          # build klisp.ko in a container matching your kernel
scripts/fetch-image.sh    # one-time: fetch a bootable vmlinuz + NIC + busybox (~108MB)
scripts/mk-initramfs.sh   # build the initramfs that loads the module
scripts/run-qemu.sh       # boot it (quit QEMU with Ctrl-A then X)
```

From another terminal, talk to the in-kernel server through the forwarded port:

```sh
printf 'hello\n' | nc localhost 4005     # echoes 'hello' back
```

Inside the VM you can `rmmod klisp` / `insmod /klisp.ko bind_addr=0.0.0.0` to
test reload, or boot with the `klisp_selftest` kernel cmdline token to run an
automated rmmod/reload self-test.

Once the SWANK server lands (M3), connecting will be
`M-x slime-connect RET localhost RET 4005 RET`.

## Module parameters

| param | default | notes |
|---|---|---|
| `port` | `4005` | TCP port to listen on. |
| `bind_addr` | `127.0.0.1` | Bind address. Loopback by default — an in-kernel eval endpoint must not be network-exposed. The QEMU initramfs passes `0.0.0.0` because hostfwd lands on the guest NIC IP, not loopback. |

## License

MIT — see [`LICENSE`](./LICENSE). The module declares `MODULE_LICENSE("Dual
MIT/GPL")`, the kernel ident that keeps access to GPL-only exported symbols
without a license taint while the source stays MIT. Vendored `fe` is also MIT
(its `vendor/fe/LICENSE` will be retained when it's added in M2).

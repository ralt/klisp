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

- **M1 — done.** TCP server in a kernel module (kthread + sockets).
- **M2 — done.** Embedded `fe` Lisp interpreter as a raw line REPL.
- **M3 — done.** Minimal SWANK server so Emacs/SLIME can connect; same port
  also serves the raw REPL (protocol auto-detected).
- **M4 — done.** Read-only kernel objects as Lisp data: `(list-processes)`,
  `(list-netdevs)`, `(meminfo)`, `(uname)` — snapshotted under lock into
  immutable plists.
- **M5 — done.** SLIME inspector (`C-c I` / `M-x slime-inspect`) with
  click-through navigation; discovery via `(functions)` / `(env)` and TAB
  completion.
- **Upcoming.** More kernel objects; clickable REPL-output presentations. See
  `DESIGN.md` §9.

## Quickstart (build → boot → connect)

Prerequisites: Docker (you can run `docker`), `qemu-system-x86_64`, a Debian/
`apt` host (scripts use `apt-get download`, no root needed), and ideally
read/write on `/dev/kvm` for speed.

```sh
scripts/play.sh           # build (if needed), boot the VM, and print how to connect
```

`play.sh` is the easy path — it ensures everything is built, boots klisp in
QEMU, and prints the address to connect to (`localhost:4005`). Set `PORT=NNNN`
to use a different host port. The individual steps are also available:

```sh
scripts/build.sh          # build klisp.ko in a container matching your kernel
scripts/fetch-image.sh    # one-time: fetch a bootable vmlinuz + NIC + busybox (~108MB)
scripts/mk-initramfs.sh   # build the initramfs that loads the module
scripts/run-qemu.sh       # boot it (quit QEMU with Ctrl-A then X)
```

Then connect, either way (same port; the protocol is auto-detected):

```sh
# From Emacs:
M-x slime-connect RET localhost RET 4005 RET

# Or a raw REPL over netcat:
nc localhost 4005     # then type: (+ 1 2)  or inspect the kernel:
                      #   (car (list-processes))  =>  (:pid 1 :comm "init" ...)
                      #   (list-netdevs)  (meminfo)  (uname)  (= sq (fn (n) (* n n)))  (sq 9)
```

In Emacs once connected:

- **Discover** what's available: evaluate `(functions)` (or `(env)`), or press
  `TAB` to complete a partially-typed name.
- **Inspect** an object: `C-c I` (or `M-x slime-inspect`), type an expression
  like `(list-processes)`, then `RET`/click the parts to drill in and `l` to go
  back. (Clicking a value directly in REPL *output* isn't wired up yet — use
  `C-c I` for now.)

Inside the VM you can `rmmod klisp` / `insmod /klisp.ko bind_addr=0.0.0.0` to
test reload, or boot with the `klisp_selftest` kernel cmdline token to run an
automated rmmod/reload self-test.

## Testing

`scripts/test.sh` is the end-to-end regression suite: it builds the module in
Docker, boots it in QEMU, and runs two TCP drivers against the live module,
failing if any case is wrong or the kernel Oopses:

- `scripts/repl_test.py` — raw REPL, ~40 primitive assertions (arithmetic,
  predicates, control flow, lists, functions/recursion, error recovery).
- `scripts/swank_test.py` — the SWANK wire protocol the way SLIME speaks it
  (handshake + `listener-eval` over the same primitives, `:abort` on errors).
- `scripts/emacs_slime_test.el` — a **real Emacs + SLIME** client (run in batch)
  that `slime-connect`s and evaluates, exercising the actual client path. Run
  automatically when `emacs` and SLIME are installed, skipped otherwise.

Run it before adding new primitives or abstractions:

```sh
scripts/test.sh          # per-case ok/FAIL, then PASS/FAIL; exit code reflects it
```

The cases live in `scripts/repl_test.py`; add to `CASES` as the language grows.

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

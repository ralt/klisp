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
- **M5 — done.** SLIME inspector with click-through navigation — including
  **clickable REPL-output presentations** (click a result to inspect it);
  discovery via `(functions)` / `(env)` and TAB completion.
- **M6 — done.** Resilience: a watchdog aborts runaway evals (e.g. `(while 1
  1)`) instead of hanging; a supervisor kthread restarts the worker on request;
  `debugfs` control/status; snapshot reads use `copy_from_kernel_nofault`.
- **Upcoming.** More kernel objects and richer inspector rendering. See
  `DESIGN.md` §9.

## Quickstart (build → boot → connect)

Prerequisites: Docker (you can run `docker`), `qemu-system-x86_64`, a Debian/
`apt` host (scripts use `apt-get download`, no root needed), and ideally
read/write on `/dev/kvm` for speed.

```sh
make play          # build what's needed, boot the VM, print how to connect
```

`make` drives everything with dependency tracking, so it rebuilds only what
changed (module, then initramfs) and never boots a stale artifact:

```sh
make               # build klisp.ko (only if sources changed)
make run           # build as needed, then boot in QEMU (Ctrl-A X to quit)
make play          # same as run, but prints the connect address (PORT=NNNN to change)
make test          # build as needed, boot, and run the test suite
make clean
```

(The underlying steps live in `scripts/` — `build.sh`, `fetch-image.sh`,
`mk-initramfs.sh`, `run-qemu.sh` — and the module itself is described in `Kbuild`.)

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
- **Inspect** an object: either **click the result** of a REPL evaluation, or
  `C-c I` (or `M-x slime-inspect`) and type an expression like
  `(list-processes)`. Then `RET`/click the parts to drill in, and `l` to go
  back.

Inside the VM you can `rmmod klisp` / `insmod /klisp.ko bind_addr=0.0.0.0` to
test reload, or boot with the `klisp_selftest` kernel cmdline token to run an
automated rmmod/reload self-test.

Recovery without a reload (debugfs, in the guest):

```sh
cat /sys/kernel/debug/klisp/status        # bind, worker state, restarts, ...
echo 1 > /sys/kernel/debug/klisp/reset    # supervisor restarts the worker
```

A runaway evaluation (e.g. `(while 1 1)`) is aborted automatically after
`eval_timeout_s` seconds rather than wedging the session.

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
make test          # builds as needed, then per-case ok/FAIL, then PASS/FAIL
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

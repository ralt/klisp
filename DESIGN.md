# klisp — a Lisp machine inside the Linux kernel

> **Goal.** Load a kernel module that turns the running Linux kernel into a
> Lisp environment. From Emacs, `M-x slime-connect` to a TCP port and get a
> live REPL whose environment is the kernel: you can list and **inspect**
> kernel objects (processes, modules, network devices, memory) as Lisp
> objects, navigating them with the SLIME inspector. **Read-only first.**
>
> **Resilience is a goal, not an afterthought.** The intended end state is
> something usable on a real machine, so the design target is: a bug should
> *warn and recover* (`pr_warn` + clean Lisp error, or worker restart), **not**
> crash the kernel. See §7 for the fault model. "A bug panics the box" is
> explicitly rejected as a design stance.

This document is the plan and the rationale. No code yet — it captures the
architecture, the genuinely hard parts, the chosen dev setup, and a milestone
order so we always have something testable.

---

## 1. Scope — where the difficulty actually is

It's worth being precise about which parts are hard, because the intuition
("the SWANK server will be the wall") is wrong. Ranked by actual difficulty:

1. **Running safely in ring 0** — fault containment, memory/stack discipline,
   locking when touching live kernel state. This is the real work (§7).
2. **The interpreter core** — reader, printer, eval, and especially memory
   management (a GC or arena) under kernel constraints. Largely solved by
   adopting an existing embeddable Lisp; see §4.
3. **The kernel-object layer** — snapshotting live objects to safe Lisp values
   (§6).
4. **SWANK** — the smallest piece. It is a well-defined wire protocol with a
   handful of message types; we implement the handlers directly rather than
   reusing `swank.lisp` (which assumes a full CL + streams). Described plainly
   in §5 — it's a shim, not a research problem.

So most of the line count and *all* of the risk live in 1–3; SWANK is a bounded
serialization layer on top.

---

## 2. Environment & constraints (from the target machine)

- Host kernel: `6.12.x` (Debian 13), but **no kernel headers installed** and
  **not root** — so we can't build-and-`insmod` against the live host kernel
  yet, and *during development* we wouldn't want to anyway (an unproven module
  can take down the box). Running on this host kernel **is** the release goal
  (§8 "Deployment target"); it just comes after the code is hardened in QEMU.
- Available: `gcc`, `clang`, `make`, `qemu-system-x86_64`, `emacs`, `sbcl`,
  SLIME (`~/.emacs.d/elpa/slime-20250121.1632`).

**Decision: develop entirely in QEMU.** Build the module out-of-tree against a
Debian distro kernel + matching headers, boot a small VM, `insmod` inside it,
and `slime-connect` from host Emacs to a forwarded TCP port. A crash costs a
`qemu` restart, not the machine. This is as "real kernel" as the end goal wants.

### Kernel-specific constraints that shape the design

| Constraint | Consequence |
|---|---|
| **No libc** | Reimplement string↔int, formatting, our own allocator. |
| **No FPU** (can't use floats freely in kernel) | **Integer-only Lisp.** Don't fight it. |
| **Tiny kernel stack (~16 KB)** | A naive recursive `eval` will silently overflow → panic on deep input. Start **depth-limited**; consider explicit-stack/trampoline later. |
| **Leaks persist until reboot** | `fe` already GCs within a fixed buffer we provide (§4) — we just size/feed that buffer from `kmalloc`/`vmalloc` and tune the GC trigger; soft reset drops the whole context. |
| **Live kernel objects have lifetimes/locking (RCU)** | Don't expose raw pointers to Lisp. **Snapshot under the right lock, release, hand Lisp immutable copies** (see §6). |
| **Bugs run in ring 0** | **Warn and recover, don't crash** — see the fault model in §7. Lisp-level errors are caught by construction; pointer-touching C is tiny, audited, and uses non-faulting accessors. QEMU-only during dev regardless. |

---

## 3. Architecture overview

```
  Emacs (host)                         QEMU guest: klisp.ko
  ┌──────────┐   TCP (forwarded)   ┌──────────────────────────────────────┐
  │  SLIME    │◄───── port ───────►│  kthread: accept loop + per-conn I/O  │
  │ (client)  │  6-hex len + sexp  │            │                          │
  └──────────┘                     │  ┌─────────▼──────────┐               │
                                   │  │     SWANK layer     │  (§5)         │
                                   │  │  - framing/IO       │               │
                                   │  │  - :emacs-rex demux │               │
                                   │  │  - REPL + inspector │               │
                                   │  └─────────┬──────────┘               │
                                   │  ┌─────────▼──────────┐               │
                                   │  │  Lisp core (§4)     │  fe: reader,  │
                                   │  │  fe + our builtins, │  printer,     │
                                   │  │  fe GC over buffer  │  eval, GC     │
                                   │  └─────────┬──────────┘               │
                                   │  ┌─────────▼──────────┐               │
                                   │  │ kernel-object layer │  (§6)         │
                                   │  │ snapshot→plist      │  read-only    │
                                   │  └─────────────────────┘               │
                                   └──────────────────────────────────────┘
```

**Transport: TCP, not netlink/unix.** `slime-connect` only wants host+port,
and QEMU forwards a TCP port trivially. Netlink would force a custom Emacs
client — avoid. Kernel sockets: `sock_create_kern` →
`kernel_bind`/`kernel_listen`/`kernel_accept`, served from a `kthread`, I/O via
`kernel_recvmsg`/`kernel_sendmsg`.

---

## 4. The Lisp core (subset, not full Common Lisp)

### Chosen core: `fe` (rxi) — locked in
We use an existing tiny embeddable C Lisp for the reader/printer/eval/GC rather
than building those from zero. **Decision: `fe` by rxi**
(`https://github.com/rxi/fe`, MIT, <800 sloc, two files `src/fe.c` + `src/fe.h`).
It fits our constraints almost exactly:
- **No dynamic allocation** — it runs entirely inside a buffer you pass to
  `fe_open(ptr, size)`, with a mark-sweep GC over that buffer. Maps directly to
  a `kmalloc`/`vmalloc`'d arena. This is the single most valuable property; GC
  under kernel constraints is otherwise the hardest part of §1.2.
- **I/O via callbacks** (`fe_ReadFn`/`fe_WriteFn`), so the core never touches
  `stdio`; we wire those to the socket.

Required surgery (modest, well-scoped):
1. **Numbers float→`long`.** `fe`'s number type is floating point and uses
   `sprintf("%.7g")`; the kernel forbids FP, so retype to `long` and supply our
   own integer→string formatter.
2. **Replace `setjmp`/`longjmp`** error handling (unavailable in-kernel) with
   our condition mechanism (below).
3. **Strip residual libc** from the core (the optional `FILE*` helpers; any
   `sprintf`). Kernel `linux/string.h` already provides `memcpy`/`memset`/etc.

### Integration: vendored, not a submodule
We **copy `fe.c`/`fe.h` into the tree** (`vendor/fe/`), not a git submodule.
Rationale: a submodule is the right tool for a large, *unmodified*,
independently-versioned dependency — fe is the opposite. It's a single tiny
file that **won't even compile in-kernel unmodified** (floats, `setjmp`,
`sprintf`), so we must patch it invasively; submodules make local edits painful
(you'd need your own fork repo and an out-of-tree patch dance) and buy nothing,
since Kbuild compiles the source in-tree either way (`klisp-objs += vendor/fe/fe.o`).

Vendoring discipline (keeps it clean and re-syncable, and honors MIT):
- `vendor/fe/` holds `fe.c`, `fe.h`, and upstream's `LICENSE` (MIT requires
  retaining the copyright + license text).
- `vendor/fe/UPSTREAM` records the origin and pinned commit:
  `https://github.com/rxi/fe @ ed4cda96bd582cbb08520964ba627efb40f3dd91`.
- Our kernel changes live as a single `vendor/fe/kernel.patch` (or clearly
  `/* klisp: ... */`-marked diffs) so re-syncing to a newer upstream is a
  re-apply, not an archaeology dig.

Alternatives considered: **minilisp** (rui314) is integer-only and very
readable but uses `mmap`/`printf`/`exit` and has no clear license — more to
excise and a licensing question. **uLisp** is the closest in spirit (bare-metal,
fixed cell workspace, optional integer-only, MIT) but is Arduino/C++ and tied to
Arduino serial — more porting effort. NaN-boxing Lisps (e.g. tinylisp) are
disqualified: the boxing *is* floating point.

**Honest scoping note:** adopting `fe` removes the trickiest core work (GC) and
maybe a third of the code, but the kernel plumbing (§3), snapshot layer (§6),
SWANK (§5), and fault model (§7) don't exist off-the-shelf — they're the
majority of klisp and we write them regardless.

### Language surface
Enough to be a usable REPL and to model kernel objects. Integer-only.

- **Data types:** cons, symbol, fixnum (integer-only), string, nil, t, builtin
  function, closure, and a tagged **foreign/snapshot object** (§6).
- **Reader:** tokenizer + recursive-descent s-expression parser. Supports
  `'quote`, lists, integers, strings, symbols, dotted pairs optional.
- **Printer:** `prin1`/`princ`; must handle our snapshot objects (`#<proc 42 bash>`).
- **Evaluator:** special forms `quote if lambda let let* setq defun progn cond
  and or` + function application. **Depth-limited** to protect the kernel stack
  (the limit raises a clean Lisp error *before* the C stack overflows).
- **Condition system (the safety backbone):** a non-local exit to REPL
  top-level (our own saved-context "throw", since there is no libc
  `setjmp`/`longjmp`). Every primitive validates type/arity; bad input,
  unbound symbol, divide-by-zero, depth-exceeded, etc. all `signal` a Lisp
  error that unwinds cleanly and returns to Emacs as `(:abort "...")`. **No
  oops** for any Lisp-level mistake — this is what makes the REPL safe to abuse.
- **Builtins (initial):** `car cdr cons list eq eql equal null atom consp
  symbolp + - * < > = not mapcar length nth assoc getf` plus printing helpers.
- **Environment:** global symbol table + lexical frames for `let`/lambda.
- **Memory:** `fe`'s built-in mark-sweep GC, operating inside one buffer we
  allocate (`kmalloc`/`vmalloc`) at module load and pass to `fe_open`. Our work
  is sizing the buffer, tuning the GC trigger, and dropping/re-creating the
  context on soft reset (§7) — no separate allocator to write.

Most of this list (reader, printer, eval, GC, closures, and the basic builtins)
comes from `fe`. Our additions on top are the integer retype, the condition
system, the snapshot objects (§6), and the kernel-fact builtins
(`list-processes`, …).

---

## 5. SWANK server

A bounded protocol layer: parse framed messages, dispatch `swank:` calls
against our Lisp, serialize replies. We implement the handlers directly rather
than reusing `swank.lisp` (it assumes a full Common Lisp with streams). The
handler set below is everything needed for a working REPL plus the inspector.

### Wire framing
Each message is a **6-hex-digit byte-length prefix** followed by that many
bytes of an s-expression. Read prefix → read N bytes → parse with our reader →
dispatch. Replies are framed the same way.

### Message flow
- Emacs sends: `(:emacs-rex FORM PACKAGE THREAD ID)`.
- We evaluate `FORM` (a `swank:` call) and reply `(:return (:ok VALUE) ID)`
  (or `(:abort ...)`).
- Output/streaming uses `(:write-string "...")` pushed to the client.

### Handlers needed for a working REPL
| swank call | Minimal behavior |
|---|---|
| `swank:connection-info` | plist: `:pid`, `:lisp-implementation (:type :name :version)`, `:package (:name :prompt)`, `:version "..."`. |
| `swank:swank-require` | return the requested module list (stub). |
| `swank:create-repl` | return `("COMMON-LISP-USER" "CL-USER")`. |
| `swank:listener-eval` | read+eval the string, push output as `:write-string`, return `(:ok (:values "..."))`. |
| `swank:interactive-eval` | eval, return printed result string. |
| `swank:autodoc` | `(:not-available . t)` (stub ok). |
| `swank:operator-arglist` | `nil`/stub. |

That's enough for a live REPL in Emacs.

### Handlers needed for the **inspector**
The inspector needs the server, given an object, to return a *page*: a title
plus a list of **parts** (strings interleaved with `(:value "printed" INDEX)`
references). Clicking a reference sends the index back; the server pushes that
sub-object and returns its page. The server keeps an **inspector state**: a
stack of inspected objects + a parts table mapping `INDEX → object`.

| swank call | Minimal behavior |
|---|---|
| `swank:init-inspector "FORM"` | eval FORM, inspect result → return first page. |
| `swank:inspect-nth-part N` | look up part N, push it, return its page. |
| `swank:inspector-pop` / `inspector-next` | navigate the inspector stack. |
| `swank:quit-inspector` | drop inspector state. |
| `swank:inspector-reinspect` | re-snapshot current object (refresh). |

The per-type `inspect` logic (what parts a process/module/netdev exposes) lives
in the kernel-object layer (§6), so adding a new inspectable kernel type is
local.

---

## 6. Kernel objects, read-only — and how the inspector stays safe

**Design rule: never hand Lisp a raw live kernel pointer.** Instead,
**snapshot to immutable Lisp plists** under the correct lock, then release.

When the user evaluates `(list-processes)`, we walk `for_each_process` under the
proper lock, copy the fields we want into freshly-allocated immutable plists,
and release. No dangling pointers, no RCU hazards leaking into Lisp, no way for
a REPL typo to corrupt kernel state.

Reads of addresses that might already be freed (e.g. a re-snapshot of an object
that just exited) use **`copy_from_kernel_nofault()`** so a bad address returns
`-EFAULT` instead of oopsing — surfaced to Lisp as a normal error. This is the
second pillar of §7's fault model: the only pointer-touching code is here, and
it's written so even a stale pointer can't crash the kernel.

### Does this break the SLIME inspector? **No.**
The inspector only needs *parts + sub-object references*, which snapshot plists
provide. A process snapshot:

```lisp
(:pid 42 :comm "bash" :state :running
 :parent  #<proc 1 systemd>          ; clickable → drills into parent
 :children (#<proc 99 ...> ...)      ; each clickable
 :vsize 1234567 ...)
```

renders with each reference as a navigable part. You get a full object graph,
**frozen at snapshot time** — for read-only exploration that's a feature
(stable, consistent view).

### Refinement: re-snapshot on drill-in
The held object carries scalar fields + identifying keys (e.g. a pid). When you
expand a reference part, the server takes a **fresh bounded snapshot** of the
referenced object. This keeps memory bounded and the view current-ish, while
still never holding a live pointer across REPL calls.
- **Caveat:** pid/handle reuse between snapshots → you might drill into a
  different object than expected. Acceptable for v1; surface gracefully
  (`#<proc 1234 — no longer exists>`).
- **Later (optional):** true live-pointer inspection for real-time view. Not
  needed for the inspector to feel great; carries the use-after-free risk we're
  deliberately avoiding in v1.

### Good initial "root objects"
`(list-processes)`, `(list-modules)`, `(list-netdevs)` (`for_each_netdev`),
`(meminfo)` / page stats, `(current-task)` fields.

---

## 7. Fault model & recovery — warn and recover, never crash

Design stance: **a bug should `pr_warn` and recover, not panic the box.** This
is achievable because we control the whole interpreter and most "bugs" are
logical, not memory-unsafe. Defense in depth, outermost → innermost:

1. **Lisp-level errors are caught by construction (the 99% case).** Type/arity
   checks, divide-by-zero, unbound symbols, and the eval depth limit all
   `signal` a condition (§4) that unwinds to REPL top-level and returns
   `(:abort "...")` to Emacs. The kernel never notices.
2. **Lisp can't synthesize a kernel pointer.** Snapshot-only objects (§6) mean
   a Lisp bug *structurally cannot* cause a wild dereference. The dangerous
   surface is confined to the small, audited snapshot/parser/socket C.
3. **The pointer-touching C is non-faulting.** `copy_from_kernel_nofault()` +
   correct RCU/locking turn "object freed under me" into a returned error, not
   an oops.
4. **`WARN_ON` continues; oops ≠ panic.** A `WARN()` prints a trace and keeps
   running (unless `panic_on_warn=1`). Even an oops, by default, kills only the
   offending thread (unless `panic_on_oops=1`) — it does not take down the box.
5. **Recovery without reboot, three levels:**
   - **Soft reset** — a command (REPL form + a `debugfs`/`sysfs` trigger) that
     drops all Lisp state and re-inits the arena/environment from scratch.
   - **Worker restart** — a small **supervisor** that detects a dead worker
     `kthread` (e.g. it oopsed) and respawns it; sockets are re-established.
   - **Reload** — clean `rmmod` + `insmod` as the backstop; teardown is written
     to tolerate half-dead/partial state (thread already gone, socket dangling).

**The one honest caveat.** A bug in *our own C* that does a wild **write** to
unrelated kernel memory can still corrupt the kernel — this residual risk is
inherent to any code in ring 0 and cannot be reduced to zero, only minimized:
keep that C surface tiny, audit it, fuzz the reader and the wire framing, and
rely on read-only semantics so we mostly *read* kernel memory, never scribble
on it. For higher isolation than an in-kernel module can ever offer, the
fallback is the userspace-helper split (interpreter in userspace, thin kernel
shim only for data) — noted but **not** chosen, since it dilutes the
kernel-as-Lisp-machine vision.

---

## 8. Dev & test setup (distro kernel, Docker build, QEMU run)

Repo layout (✓ = exists as of M1):
```
klisp/
  DESIGN.md
  Makefile                  # ✓ out-of-tree build (klisp-y := src/klisp_main.o; += vendor/fe/fe.o later)
  src/klisp_main.c          # ✓ M1: socket/kthread; later: swank, snapshots, glue
  docker/Dockerfile         # ✓ debian:trixie + matching linux-headers (build env)
  scripts/                  # ✓ build.sh, fetch-image.sh, mk-initramfs.sh, run-qemu.sh
  vendor/fe/                # (M2) fe.c, fe.h, LICENSE, UPSTREAM, kernel.patch  (see §4)
  .devkernel/               # (gitignored) fetched vmlinuz, e1000.ko, busybox, initramfs.gz
  klisp.ko                  # (gitignored) build output
```

**Build — in Docker** (chosen over local header-extraction; see the rationale
below). `scripts/build.sh`:
- builds `docker/Dockerfile` (a `debian:trixie` image that `apt-get install`s
  `linux-headers-$KVER`). A container shares the host kernel, so `uname -r`
  inside == the host kernel and `/lib/modules/$KVER/build` resolves to real,
  prepared headers — no symlink fixups.
- runs `make` in that image as the host uid (so `klisp.ko` isn't root-owned).

*Why Docker, not local `dpkg-deb -x`?* Extracting Debian header debs into a
non-root sysroot leaves dangling absolute symlinks into `/usr/lib/linux-kbuild`
that need hand-fixing. Installing as root *inside a container* sidesteps all of
it. (`apt-get download` of the debs still works without root and is the
fallback if Docker is unavailable.)

**Run — QEMU on the host** (Docker can't run the VM without `/dev/kvm`
passthrough). Host-side scripts:
1. `scripts/fetch-image.sh` — `apt-get download` the matching `linux-image`,
   extract `vmlinuz` + the `e1000` NIC driver (decompressed — busybox `insmod`
   can't `unxz`); also fetch a static busybox. Lands in `.devkernel/`.
2. `scripts/mk-initramfs.sh` — busybox initramfs whose `/init` loads `e1000`,
   configures `eth0` (SLIRP: guest `10.0.2.15`, gw `10.0.2.2`), and
   `insmod`s `klisp.ko bind_addr=0.0.0.0` (hostfwd lands on the guest NIC IP,
   not loopback). A `klisp_selftest` cmdline token triggers an rmmod/reload
   self-test.
3. `scripts/run-qemu.sh` — boots with `-enable-kvm` (if `/dev/kvm` writable)
   and `-netdev user,hostfwd=tcp::4005-:4005`, so the host reaches the guest's
   port at `localhost:4005`. Quit with `Ctrl-A X`.
4. **Connect:** `nc localhost 4005` (M1 echo) → later `M-x slime-connect RET
   localhost RET 4005 RET`.

Iteration loop: edit → `scripts/build.sh` → `scripts/run-qemu.sh` (seconds) →
connect. See `README.md` for copy-paste commands.

> **M1 status: done & verified.** Builds clean against the `6.12.74+deb13+1`
> headers; boots in QEMU; `nc` echo works across multiple connections incl.
> binary payloads; two `rmmod`→`insmod` cycles complete with **no Oops / BUG /
> panic** (only the expected unsigned-out-of-tree taint).

### Deployment target: the real host kernel (first-release goal)
QEMU is the dev sandbox; the **first release runs `klisp.ko` on the host's live
kernel** — that's the point of the project, and it's what the §7 fault model
(warn-and-recover, read-only, soft-reset/reload) is designed to make safe.
Going from QEMU to host needs three things the current host lacks:
1. **Matching headers** — install `linux-headers-$(uname -r)` so the module is
   built against the *exact* running kernel (`vermagic` must match or `insmod`
   refuses it). Today: not installed.
2. **Privilege** — `sudo insmod klisp.ko` / `rmmod` (loading a module needs
   `CAP_SYS_MODULE`). Today: uid 1000.
3. **Module signing if Secure Boot is on** — a locked-down kernel rejects
   unsigned modules; either sign with an enrolled MOK key or the load fails.
   Check with `mokutil --sb-state`.

Deployment discipline: only load on the host after it survives hammering in
QEMU; keep it read-only; bind the SWANK port to **loopback only** (it's an
in-kernel eval endpoint — do not expose it on the network); keep `rmmod` and
the soft-reset trigger at hand. The same `klisp.ko` artifact runs in both
places — only the build's target kernel headers differ.

---

## 9. Milestones (each independently testable)

1. **M1 — Plumbing. ✓ DONE.** Module skeleton + TCP echo `kthread` in QEMU;
   connect with `nc` from host. *Proved the dangerous part: kernel sockets +
   threads, and clean rmmod/reload teardown.*
2. **M2 — Integrate `fe` + condition system.** Vendor `fe`, make it build
   in-kernel (numbers→`long`, strip libc, GC buffer from `kmalloc`/`vmalloc`),
   wire its I/O callbacks to the socket, and replace its `setjmp` error path
   with our non-local-exit condition machinery (so a bad form returns an error,
   never crashes). Result: a raw Lisp REPL over the socket.
3. **M3 — Minimal SWANK REPL.** The ~7 REPL handlers + framing → real
   `M-x slime-connect` working REPL.
4. **M4 — Read-only kernel objects.** `(list-processes)`, `(list-modules)`,
   `(list-netdevs)` returning snapshot plists; printer shows `#<proc ...>`.
5. **M5 — Inspector.** Inspector handlers + per-type `inspect` parts +
   re-snapshot-on-drill-in → click-through object navigation in SLIME.
6. **M6 — Resilience hardening.** Soft-reset command, worker-restart
   supervisor, `copy_from_kernel_nofault` snapshot reads, robust teardown — the
   §7 recovery story made real and tested (kill the worker, watch it come back).
7. **Later.** Mark-sweep GC; explicit-stack evaluator; more builtins;
   autodoc/completion; optionally live (non-snapshot) objects and method calls.

---

## 10. Explicit non-goals / steer-aways (v1)
- ❌ Reusing real `swank.lisp` or a real Common Lisp.
- ❌ eBPF (can't host an interpreter or arbitrary sockets).
- ❌ Floating point.
- ❌ Live mutable kernel-pointer objects (snapshot instead).
- ❌ Write/mutate operations on kernel state (read-only first).

(Note: running on the **real host kernel** is *not* a non-goal — it's the
intended deployment target for the first release. QEMU is the development
sandbox, not the only place this ever runs. See §8 "Deployment target.")

---

## 11. Top risks & mitigations
| Risk | Mitigation |
|---|---|
| Crash from interpreter bug | Caught as a Lisp condition (§7) → `(:abort)`, not an oops; QEMU-only during dev. |
| Worker thread dies (oops in our C) | Supervisor respawns it; soft-reset or reload as backstop (§7). |
| Wild write in our own C corrupts kernel | Residual ring-0 risk; minimize+audit+fuzz the small C surface; read-only semantics (§7). |
| Kernel stack overflow on deep recursion | Depth limit raises a Lisp error before overflow; explicit-stack eval later. |
| Memory leak persisting until reboot | Arena-per-eval now; GC later. |
| Use-after-free on live kernel objects | Snapshot-under-lock, never expose raw pointers. |
| SLIME protocol version mismatch | Report a compatible `:version` in `connection-info`; modern SLIME is lenient. |
| Malicious/garbage wire input | Strict framing limits, length caps, defensive parser. |

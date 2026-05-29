#!/usr/bin/env python3
"""Drive the klisp REPL over TCP and assert primitive behaviour.

Usage: repl_test.py [host] [port]   (default localhost 4005)

Connects once, then for each case sends an expression and matches the response
(everything up to the next "> " prompt) against an expected substring. Exits
non-zero if any case fails. Intended to be run by scripts/test.sh against a
QEMU-booted module, so regressions in the underlying primitives are caught as
higher-level abstractions are added.
"""
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "localhost"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4005

# (expression, expected substring in the response)
CASES = [
    # arithmetic (integer-only)
    ("(+ 1 2)", "3"),
    ("(- 10 3 2)", "5"),
    ("(* 2 3 4)", "24"),
    ("(/ 100 5 2)", "10"),
    ("(+ (* 2 3) (- 10 4))", "12"),
    # comparison / predicates
    ("(< 1 2)", "t"),
    ("(< 3 2)", "nil"),
    ("(<= 2 2)", "t"),
    ("(is 1 1)", "t"),
    ("(is 1 2)", "nil"),
    ("(not nil)", "t"),
    ("(not 1)", "nil"),
    ("(atom 1)", "t"),
    ("(atom (list 1 2))", "nil"),
    # control flow
    ("(if 1 (quote yes) (quote no))", "yes"),
    ("(if nil 1 2)", "2"),
    ("(and 1 2 3)", "3"),
    ("(or nil 5)", "5"),
    ("(do 1 2 3)", "3"),
    # data
    ("(cons 1 2)", "(1 . 2)"),
    ("(car (cons 1 2))", "1"),
    ("(cdr (cons 1 2))", "2"),
    ("(list 1 2 3)", "(1 2 3)"),
    ("(quote (a b c))", "(a b c)"),
    ('"hello"', "hello"),
    # variables persist within a connection
    ("(= x 21)", "nil"),
    ("(* x 2)", "42"),
    # functions, lexical scope, recursion
    ("(= sq (fn (n) (* n n)))", "nil"),
    ("(sq 9)", "81"),
    ("(= fact (fn (n) (if (< n 2) 1 (* n (fact (- n 1))))))", "nil"),
    ("(fact 5)", "120"),
    ("(do (let a 3) (* a a))", "9"),
    # error recovery: each error must be reported AND the REPL keep working
    ("(/ 1 0)", "error: division by zero"),
    ("(+ 40 2)", "42"),
    ("(car 5)", "error: expected pair, got number"),
    ("(* 7 6)", "42"),
    # stack guard: runaway recursion must error, not overflow the kernel stack
    ("(= loop (fn () (loop)))", "nil"),
    ("(loop)", "recursion too deep"),
    ("(+ 2 2)", "4"),
    # runaway-eval watchdog: an infinite loop must abort, not hang the worker
    ("(while 1 1)", "timed out"),
    ("(+ 3 3)", "6"),                            # recovered after timeout
    # read-only kernel objects (values are live, so assert structure)
    ("(atom (list-processes))", "nil"),          # non-empty list, not an atom
    ("(car (list-processes))", ":pid"),          # first proc is a plist
    ("(car (list-processes))", ":comm"),
    ("(list-netdevs)", "lo"),                    # loopback always present
    ("(meminfo)", "totalram"),
    ("(uname)", "6.12"),
    ("(uname)", "Linux"),
    # list helpers
    ("(length (list 1 2 3))", "3"),
    ("(nth 1 (list 10 20 30))", "20"),
    ("(< 0 (length (list-processes)))", "t"),    # at least one process
    # getf on plists (literal, missing key, and real kernel-object snapshots)
    ("(getf (list :a 1 :b 2) :b)", "2"),
    ("(getf (list :a 1) :missing)", "nil"),
    ("(getf (uname) :machine)", "x86_64"),
    ("(< 0 (getf (car (list-processes)) :pid))", "t"),
    # mapcar (incl. applying a closure over kernel-object plists)
    ("(mapcar (fn (n) (* n n)) (list 1 2 3))", "(1 4 9)"),
    ("(mapcar (fn (d) (getf d :name)) (list-netdevs))", "lo"),
    # discovery
    ("(atom (functions))", "nil"),               # functions returns a list
    ("(functions)", "car"),                      # a primitive
    ("(functions)", "list-processes"),           # a kernel builtin
    ("(env)", "uname"),
]


def recv_until_prompt(sock, timeout=10):
    sock.settimeout(timeout)
    buf = b""
    while True:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            break
        if not data:
            break
        buf += data
        if buf.endswith(b"> "):
            break
    return buf.decode("utf-8", "replace")


def connect(retries=30):
    last = None
    for _ in range(retries):
        try:
            return socket.create_connection((HOST, PORT), timeout=10)
        except OSError as e:
            last = e
            time.sleep(0.5)
    raise SystemExit(f"could not connect to {HOST}:{PORT}: {last}")


def main():
    s = connect()
    recv_until_prompt(s)  # consume banner + first prompt
    passed = failed = 0
    for expr, expect in CASES:
        s.sendall((expr + "\n").encode())
        resp = recv_until_prompt(s)
        body = resp[:-2] if resp.endswith("> ") else resp
        ok = expect in body
        shown = body.strip().replace("\n", " | ")
        print(f"[{'ok  ' if ok else 'FAIL'}] {expr:48s} -> {shown!r:32s} (want {expect!r})")
        if ok:
            passed += 1
        else:
            failed += 1
    s.close()
    print(f"\n{passed} passed, {failed} failed, {len(CASES)} total")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

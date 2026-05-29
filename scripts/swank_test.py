#!/usr/bin/env python3
"""Integration test for the klisp SWANK server.

Usage: swank_test.py [host] [port]   (default localhost 4005)

Speaks the real SWANK wire protocol (6-hex length prefix + s-expression) the
way SLIME does: handshake (connection-info, create-repl), then a series of
swank-repl:listener-eval requests, asserting the :repl-result and :return for
each. Covers the primitives end-to-end through the protocol plus error
(:abort) handling. Exits non-zero on any failure.
"""
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "localhost"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4005

# (code, expected, kind):
#   kind "ok"    -> expected substring must appear in the :repl-result
#   kind "abort" -> the :return must be (:abort ...) containing expected
#   kind "print" -> expected must appear in a non-repl-result :write-string
CASES = [
    ("(+ 1 2)", "3", "ok"),
    ("(* 6 7)", "42", "ok"),
    ("(- 10 3 2)", "5", "ok"),
    ("(/ 100 5 2)", "10", "ok"),
    ("(< 1 2)", "t", "ok"),
    ("(if nil 1 2)", "2", "ok"),
    ("(list 1 2 3)", "(1 2 3)", "ok"),
    ("(cons 1 2)", "(1 . 2)", "ok"),
    ("(print 99)", "99", "print"),
    # variables persist within the connection
    ("(= x 21)", "nil", "ok"),
    ("(* x 2)", "42", "ok"),
    # functions + recursion
    ("(= sq (fn (n) (* n n)))", "nil", "ok"),
    ("(sq 9)", "81", "ok"),
    ("(= fact (fn (n) (if (< n 2) 1 (* n (fact (- n 1))))))", "nil", "ok"),
    ("(fact 5)", "120", "ok"),
    # errors abort the rex, and the session keeps working afterwards
    ("(/ 1 0)", "division by zero", "abort"),
    ("(+ 40 2)", "42", "ok"),
    ("(car 5)", "expected pair", "abort"),
    ("(* 7 6)", "42", "ok"),
    ("(= loop (fn () (loop)))", "nil", "ok"),
    ("(loop)", "recursion too deep", "abort"),
    ("(+ 2 2)", "4", "ok"),
]


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise SystemExit("connection closed mid-message")
        buf += chunk
    return buf


def recv_msg(s):
    n = int(recv_exact(s, 6).decode(), 16)
    return recv_exact(s, n).decode("utf-8", "replace")


def send_rex(s, form, rid):
    payload = f'(:emacs-rex {form} "klisp-user" t {rid})'
    data = payload.encode()
    s.sendall(("%06x" % len(data)).encode() + data)


def lisp_string(code):
    return '"' + code.replace("\\", "\\\\").replace('"', '\\"') + '"'


def extract_string(payload):
    """Return the (unescaped) first Lisp string literal in payload, or ''."""
    i = payload.find('"')
    if i < 0:
        return ""
    out = []
    i += 1
    while i < len(payload):
        c = payload[i]
        if c == "\\" and i + 1 < len(payload):
            out.append(payload[i + 1])
            i += 2
            continue
        if c == '"':
            break
        out.append(c)
        i += 1
    return "".join(out)


def run_rex(s, form, rid):
    """Send a rex and collect messages until its :return. Returns
    (return_payload, repl_result_text, other_output_text)."""
    send_rex(s, form, rid)
    repl_result = ""
    other = ""
    while True:
        msg = recv_msg(s)
        if msg.startswith("(:return"):
            return msg, repl_result, other
        if msg.startswith("(:write-string"):
            text = extract_string(msg)
            if ":repl-result" in msg:
                repl_result += text
            else:
                other += text


def connect(retries=30):
    for _ in range(retries):
        try:
            return socket.create_connection((HOST, PORT), timeout=10)
        except OSError:
            time.sleep(0.5)
    raise SystemExit(f"could not connect to {HOST}:{PORT}")


def main():
    s = connect()
    s.settimeout(15)
    passed = failed = 0

    def check(name, cond, detail):
        nonlocal passed, failed
        print(f"[{'ok  ' if cond else 'FAIL'}] {name:48s} {detail}")
        if cond:
            passed += 1
        else:
            failed += 1

    # --- handshake ---
    ret, _, _ = run_rex(s, "(swank:connection-info)", 1)
    check("connection-info", ":ok" in ret and "klisp" in ret, repr(ret[:60]))

    ret, _, _ = run_rex(s, "(swank-repl:create-repl nil)", 2)
    check("create-repl", ":ok" in ret and "klisp-user" in ret, repr(ret[:60]))

    # autodoc fires on every keystroke; SLIME destructures it as (doc cache-p),
    # so an :ok nil here breaks typing. It must be a non-empty list.
    ret, _, _ = run_rex(s, '(swank:autodoc (quote ("+")) :print-right-margin 80)', 3)
    check("autodoc shape", ":ok" in ret and "nil)" not in ret and
          ("not-available" in ret or '"' in ret), repr(ret))

    ret, _, _ = run_rex(s, '(swank:simple-completions "ca" "klisp-user")', 4)
    check("completions shape", ":ok (" in ret, repr(ret))

    # --- eval cases ---
    rid = 10
    for code, expect, kind in CASES:
        form = f"(swank-repl:listener-eval {lisp_string(code + chr(10))})"
        ret, result, other = run_rex(s, form, rid)
        rid += 1
        if kind == "ok":
            ok = ":ok" in ret and expect in result
            shown = repr(result.strip())
        elif kind == "print":
            ok = ":ok" in ret and expect in other
            shown = f"out={other.strip()!r} res={result.strip()!r}"
        else:  # abort
            ok = ":abort" in ret and expect in ret
            shown = repr(ret)
        check(code, ok, f"-> {shown} (want {expect!r})")

    s.close()
    print(f"\n{passed} passed, {failed} failed, {passed + failed} total")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

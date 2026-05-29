#!/usr/bin/env bash
# End-to-end test: build the module (Docker), boot it in QEMU, drive the REPL
# over TCP with scripts/repl_test.py, and fail if any case fails or the kernel
# faults. Run from anywhere: scripts/test.sh
#
# Prereqs: docker, qemu-system-x86_64, python3 (and /dev/kvm for speed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${TEST_PORT:-4099}"
SERIAL="${ROOT}/.devkernel/serial-test.log"

# Bring the module + initramfs + kernel image up to date via make's dependency
# tracking (rebuilds only what changed; never boots a stale artifact).
echo ">> building (make: module + initramfs as needed)"
make -C "${ROOT}" "${ROOT}/.devkernel/initramfs.gz" "${ROOT}/.devkernel/vmlinuz"

ACCEL=()
[ -w /dev/kvm ] && ACCEL=(-enable-kvm -cpu host)

rm -f "${SERIAL}"
echo ">> boot QEMU (host port ${PORT}) and run tests"
qemu-system-x86_64 "${ACCEL[@]}" \
	-kernel "${ROOT}/.devkernel/vmlinuz" \
	-initrd "${ROOT}/.devkernel/initramfs.gz" \
	-append "console=ttyS0 rdinit=/init panic=-1" \
	-m 512 -nographic -no-reboot \
	-netdev "user,id=n0,hostfwd=tcp::${PORT}-:4005" \
	-device e1000,netdev=n0 \
	</dev/null >"${SERIAL}" 2>&1 &
QPID=$!
cleanup() { kill -9 "${QPID}" 2>/dev/null || true; }
trap cleanup EXIT

# Wait for the REPL to come up, or bail on a boot-time fault.
for _ in $(seq 1 120); do
	grep -q "klisp: listening on" "${SERIAL}" 2>/dev/null && break
	if grep -qE "Oops|kernel BUG|Kernel panic|Call Trace" "${SERIAL}" 2>/dev/null; then
		echo "!! kernel fault during boot:"; sed -n '1,$p' "${SERIAL}" | tail -40
		exit 1
	fi
	sleep 0.5
done

rc=0
echo "-- raw REPL primitives --"
python3 "${ROOT}/scripts/repl_test.py" localhost "${PORT}" || rc=$?
echo "-- SWANK protocol integration --"
python3 "${ROOT}/scripts/swank_test.py" localhost "${PORT}" || rc=$?

if command -v emacs >/dev/null 2>&1 && \
   ls -d "${HOME}"/.emacs.d/elpa/slime-* >/dev/null 2>&1; then
	echo "-- real Emacs + SLIME client --"
	KLISP_PORT="${PORT}" emacs --batch -l "${ROOT}/scripts/emacs_slime_test.el" \
		2>/dev/null || rc=$?
else
	echo "-- real Emacs + SLIME client: SKIPPED (emacs or slime not found) --"
fi

# A passing test suite is meaningless if the kernel oopsed underneath it.
if grep -qE "Oops|kernel BUG|Kernel panic|Call Trace" "${SERIAL}"; then
	echo "!! kernel fault detected during tests:"
	grep -nE "Oops|BUG|panic|Call Trace|RIP:" "${SERIAL}"
	rc=1
fi

[ "${rc}" -eq 0 ] && echo ">> PASS" || echo ">> FAIL (rc=${rc})"
exit "${rc}"

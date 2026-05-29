#!/usr/bin/env bash
# Build a minimal busybox initramfs that loads klisp.ko and brings up the NIC.
# Output: .devkernel/initramfs.gz
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/.devkernel"
PORT="${PORT:-4005}"

[ -f "$ROOT/klisp.ko" ]   || { echo "build klisp.ko first: scripts/build.sh"; exit 1; }
[ -x "$OUT/busybox" ]     || { echo "fetch deps first: scripts/fetch-image.sh"; exit 1; }
[ -f "$OUT/e1000.ko" ]    || { echo "fetch deps first: scripts/fetch-image.sh"; exit 1; }

IR="$OUT/initramfs"
rm -rf "$IR"; mkdir -p "$IR"/{bin,proc,sys,dev}
cp "$OUT/busybox"   "$IR/bin/busybox"
cp "$ROOT/klisp.ko" "$IR/klisp.ko"
cp "$OUT/e1000.ko"  "$IR/e1000.ko"

cat > "$IR/init" <<EOF
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev 2>/dev/null
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null

insmod /e1000.ko
ifconfig lo 127.0.0.1 up
# QEMU SLIRP: guest is 10.0.2.15, gateway/host is 10.0.2.2
ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up
route add default gw 10.0.2.2 2>/dev/null

# bind_addr=0.0.0.0 because hostfwd lands on the guest NIC IP, not loopback.
# eval_timeout_s kept short so the runaway-eval watchdog is quick to observe.
insmod /klisp.ko bind_addr=0.0.0.0 port=${PORT} eval_timeout_s=3

# Optional teardown self-test: boot with 'klisp_selftest' on the kernel cmdline
# to exercise the rmmod/reload path (the most panic-prone code) deterministically.
if grep -qw klisp_selftest /proc/cmdline; then
	echo MARK-selftest-begin
	rmmod klisp                                   ; echo MARK-rmmod1-rc=\$?
	insmod /klisp.ko bind_addr=0.0.0.0 port=${PORT}; echo MARK-reload1-rc=\$?
	rmmod klisp                                   ; echo MARK-rmmod2-rc=\$?
	insmod /klisp.ko bind_addr=0.0.0.0 port=${PORT}; echo MARK-reload2-rc=\$?
	echo MARK-selftest-end
fi

# Optional supervisor self-test: boot with 'klisp_m6test' to kill the worker
# and confirm the supervisor respawns it and the REPL still serves.
if grep -qw klisp_m6test /proc/cmdline; then
	echo MARK-m6-begin
	S1=\$(sed -n 's/^worker_starts: //p' /sys/kernel/debug/klisp/status)
	echo MARK-starts-before=\$S1
	echo 1 > /sys/kernel/debug/klisp/kill-worker
	sleep 3
	S2=\$(sed -n 's/^worker_starts: //p' /sys/kernel/debug/klisp/status)
	echo MARK-starts-after=\$S2
	echo '(+ 40 2)' | nc -w 3 127.0.0.1 ${PORT} | grep -q 42 \\
		&& echo MARK-repl-ok || echo MARK-repl-FAIL
	echo MARK-m6-end
fi

echo
echo "=== klisp on tcp :${PORT}  (host: nc localhost ${PORT}) ==="
echo "=== rmmod klisp / insmod to reload   |   Ctrl-A X to quit qemu ==="
echo

/bin/busybox sh
# Keep the VM (and the loaded module) alive if the shell gets EOF (headless run).
echo "[init] shell exited; VM idle. Ctrl-A X to quit qemu."
exec /bin/busybox sh -c 'while :; do sleep 3600; done'
EOF
chmod +x "$IR/init"

( cd "$IR" && find . | cpio -o -H newc 2>/dev/null | gzip > "$OUT/initramfs.gz" )
echo ">> initramfs: $OUT/initramfs.gz"

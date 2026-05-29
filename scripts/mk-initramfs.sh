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

insmod /e1000.ko
ifconfig lo 127.0.0.1 up
# QEMU SLIRP: guest is 10.0.2.15, gateway/host is 10.0.2.2
ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up
route add default gw 10.0.2.2 2>/dev/null

# bind_addr=0.0.0.0 because hostfwd lands on the guest NIC IP, not loopback
insmod /klisp.ko bind_addr=0.0.0.0 port=${PORT}

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

echo
echo "=== klisp M1: TCP echo on :${PORT}  (host: nc localhost ${PORT}) ==="
echo "=== rmmod klisp / insmod to test reload   |   Ctrl-A X to quit qemu ==="
echo

/bin/busybox sh
# Keep the VM (and the loaded module) alive if the shell gets EOF (headless run).
echo "[init] shell exited; VM idle. Ctrl-A X to quit qemu."
exec /bin/busybox sh -c 'while :; do sleep 3600; done'
EOF
chmod +x "$IR/init"

( cd "$IR" && find . | cpio -o -H newc 2>/dev/null | gzip > "$OUT/initramfs.gz" )
echo ">> initramfs: $OUT/initramfs.gz"

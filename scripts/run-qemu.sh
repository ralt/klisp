#!/usr/bin/env bash
# Boot the dev kernel + klisp initramfs in QEMU, forwarding the SWANK/echo port.
# Interactive serial console; quit with Ctrl-A then X.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/.devkernel"
# Host port to connect to; guest module listens on 4005 (forwarded below).
HOSTPORT="${PORT:-4005}"

# Ensure the module + initramfs + kernel image are up to date (make handles the
# dependency order; never boots a stale artifact).
make -C "$ROOT" "$OUT/initramfs.gz" "$OUT/vmlinuz"

ACCEL=()
if [ -w /dev/kvm ]; then
	ACCEL=(-enable-kvm -cpu host)
fi

exec qemu-system-x86_64 \
	"${ACCEL[@]}" \
	-kernel "$OUT/vmlinuz" \
	-initrd "$OUT/initramfs.gz" \
	-append "console=ttyS0 rdinit=/init panic=-1" \
	-m 512 -nographic -no-reboot \
	-netdev "user,id=n0,hostfwd=tcp::${HOSTPORT}-:4005" \
	-device "e1000,netdev=n0"

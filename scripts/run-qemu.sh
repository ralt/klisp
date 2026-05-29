#!/usr/bin/env bash
# Boot the dev kernel + klisp initramfs in QEMU, forwarding the SWANK/echo port.
# Interactive serial console; quit with Ctrl-A then X.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/.devkernel"
PORT="${PORT:-4005}"

[ -f "$OUT/vmlinuz" ]       || { echo "run scripts/fetch-image.sh first"; exit 1; }
[ -f "$OUT/initramfs.gz" ]  || { echo "run scripts/mk-initramfs.sh first"; exit 1; }

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
	-netdev "user,id=n0,hostfwd=tcp::${PORT}-:${PORT}" \
	-device "e1000,netdev=n0"

#!/usr/bin/env bash
# Boot klisp in QEMU for interactive use and print how to connect.
# Builds anything missing, then runs QEMU in the foreground (Ctrl-a x to quit).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# PORT is the HOST port you connect to; the guest module always listens on 4005
# and QEMU forwards host:PORT -> guest:4005. (Keep these decoupled — exporting
# PORT into mk-initramfs would change the guest port and break the forward.)
HOSTPORT="${PORT:-4005}"

[ -f "${ROOT}/klisp.ko" ]          || bash "${ROOT}/scripts/build.sh"
[ -f "${ROOT}/.devkernel/vmlinuz" ] || bash "${ROOT}/scripts/fetch-image.sh"
PORT=4005 bash "${ROOT}/scripts/mk-initramfs.sh" >/dev/null  # guest on 4005

ACCEL=()
[ -w /dev/kvm ] && ACCEL=(-enable-kvm -cpu host)

cat <<EOF

==================================================================
  klisp is booting in QEMU.

  Connect from THIS host:
    Emacs/SLIME :  M-x slime-connect RET localhost RET ${HOSTPORT} RET
    raw REPL    :  nc localhost ${HOSTPORT}        (then type:  (+ 1 2) )

  Address     :  localhost:${HOSTPORT}
  Quit QEMU   :  press  Ctrl-a  then  x
==================================================================

EOF

exec qemu-system-x86_64 "${ACCEL[@]}" \
	-kernel "${ROOT}/.devkernel/vmlinuz" \
	-initrd "${ROOT}/.devkernel/initramfs.gz" \
	-append "console=ttyS0 rdinit=/init panic=-1" \
	-m 512 -nographic -no-reboot \
	-netdev "user,id=n0,hostfwd=tcp::${HOSTPORT}-:4005" \
	-device e1000,netdev=n0

#!/usr/bin/env bash
# Fetch the bits needed to boot a matching kernel in QEMU (host side, no root):
#   .devkernel/vmlinuz    - kernel image matching the module's vermagic
#   .devkernel/e1000.ko   - NIC driver (decompressed; busybox insmod can't unxz)
#   .devkernel/busybox    - static busybox for the initramfs
set -euo pipefail

KVER="${KVER:-$(uname -r)}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/.devkernel"
mkdir -p "$OUT"

# 1. kernel image -> vmlinuz
if [ ! -f "$OUT/vmlinuz" ]; then
	echo ">> downloading linux-image-${KVER} (~108MB)"
	( cd "$OUT" && rm -f linux-image-*.deb && apt-get download "linux-image-${KVER}" )
	rm -rf "$OUT/img" && mkdir "$OUT/img"
	dpkg-deb -x "$(ls "$OUT"/linux-image-*.deb)" "$OUT/img"
	cp "$(find "$OUT/img" -name 'vmlinuz-*' | head -1)" "$OUT/vmlinuz"
fi

# 2. e1000 NIC driver, decompressed
E="$(find "$OUT/img" -name 'e1000.ko*' | head -1)"
case "$E" in
	*.xz)  xz   -dc "$E" > "$OUT/e1000.ko" ;;
	*.zst) zstd -dc "$E" > "$OUT/e1000.ko" ;;
	*)     cp "$E" "$OUT/e1000.ko" ;;
esac

# 3. static busybox
if [ ! -x "$OUT/busybox" ]; then
	echo ">> downloading busybox-static"
	( cd "$OUT" && rm -f busybox-static*.deb && apt-get download busybox-static )
	rm -rf "$OUT/bb" && mkdir "$OUT/bb"
	dpkg-deb -x "$(ls "$OUT"/busybox-static*.deb)" "$OUT/bb"
	# Match the binary, not initramfs-tools' conf-hooks.d/busybox text file.
	cp "$(find "$OUT/bb" -path '*bin/busybox' -type f | head -1)" "$OUT/busybox"
	chmod +x "$OUT/busybox"
fi

echo ">> ready: $OUT/{vmlinuz,e1000.ko,busybox}"

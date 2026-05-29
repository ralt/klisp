#!/usr/bin/env bash
# Build klisp.ko inside a Debian container matching the (host) kernel.
# Output: ./klisp.ko at the repo root.
set -euo pipefail

KVER="${KVER:-$(uname -r)}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Docker tags disallow '+', which Debian kernel versions contain.
IMG="klisp-builder:${KVER//+/-}"

echo ">> building image ${IMG} (headers for ${KVER})"
docker build --build-arg "KVER=${KVER}" \
	-t "${IMG}" -f "${ROOT}/docker/Dockerfile" "${ROOT}/docker"

echo ">> compiling module"
# Run as the host user so build artifacts (klisp.ko, *.o) aren't root-owned.
# Invoke kbuild directly on ./Kbuild (incremental; do NOT use the recursive
# `M=$PWD clean`, which would delete fetched *.ko under .devkernel/).
docker run --rm -u "$(id -u):$(id -g)" -v "${ROOT}:/work" -w /work "${IMG}" \
	make -C "/lib/modules/${KVER}/build" M=/work modules

echo ">> built: ${ROOT}/klisp.ko"
modinfo "${ROOT}/klisp.ko" 2>/dev/null | sed -n '1,12p' || true

# Build environment for the klisp kernel module.
#
# A container shares the host kernel, so `uname -r` inside == the host kernel,
# and installing linux-headers-$KVER makes /lib/modules/$KVER/build resolve to
# real, prepared headers (no out-of-install symlink fixups needed).
FROM debian:trixie

ARG KVER

# Pin the classic suites incl. security, since the exact point-release header
# package (e.g. 6.12.74+deb13+1) lives in trixie-security.
RUN printf '%s\n' \
      'deb http://deb.debian.org/debian trixie main' \
      'deb http://deb.debian.org/debian trixie-updates main' \
      'deb http://security.debian.org/debian-security trixie-security main' \
      > /etc/apt/sources.list \
 && rm -f /etc/apt/sources.list.d/debian.sources

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential bc kmod cpio flex bison libelf-dev libssl-dev \
      "linux-headers-${KVER}" \
 && rm -rf /var/lib/apt/lists/* \
 && test -e "/lib/modules/${KVER}/build" \
      || { echo "headers for ${KVER} not prepared"; exit 1; }

WORKDIR /work

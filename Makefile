# klisp — developer orchestration with real dependency tracking.
#
# The point: `make run` / `make play` / `make test` rebuild a stale module or
# initramfs (in the right order) and never boot a stale artifact. Each target
# only re-runs when its prerequisites change. The kernel module is described in
# ./Kbuild and compiled in a container by scripts/build.sh.
#
#   make            # build klisp.ko (if sources changed)
#   make run        # build as needed, then boot in QEMU
#   make play       # same, with connect instructions printed
#   make test       # same, then run the test suite
#   make clean

DK := .devkernel

# Anything that affects the compiled module.
KO_SRCS := src/klisp_main.c \
           vendor/fe/fe.c vendor/fe/fe.h vendor/fe/fe_port.h \
           Kbuild docker/Dockerfile scripts/build.sh

.PHONY: all module image initramfs run play test clean install uninstall

all: module
module: klisp.ko

klisp.ko: $(KO_SRCS)
	scripts/build.sh

# Fetches vmlinuz + e1000.ko + busybox together; vmlinuz stands in for all three.
$(DK)/vmlinuz: scripts/fetch-image.sh
	scripts/fetch-image.sh
image: $(DK)/vmlinuz

$(DK)/initramfs.gz: klisp.ko scripts/mk-initramfs.sh $(DK)/vmlinuz
	scripts/mk-initramfs.sh
initramfs: $(DK)/initramfs.gz

run: $(DK)/initramfs.gz $(DK)/vmlinuz
	scripts/run-qemu.sh

play: $(DK)/initramfs.gz $(DK)/vmlinuz
	scripts/play.sh

test: $(DK)/initramfs.gz $(DK)/vmlinuz
	scripts/test.sh

# Targeted clean — NOT kbuild's `M=$(PWD) clean`, which runs
# `find . -name '*.ko*' -delete` and would wipe fetched artifacts under .devkernel/.
clean:
	rm -f klisp.ko klisp.o klisp.mod klisp.mod.c klisp.mod.o \
	      .module-common.o Module.symvers modules.order \
	      src/klisp_main.o vendor/fe/fe.o \
	      .*.cmd src/.*.cmd vendor/fe/.*.cmd
	rm -rf .tmp_versions

# --- host install (the REAL kernel) ----------------------------------------
# Build first as your user (`make`), then install as root (`sudo make install`).
# Installs klisp.ko for the running kernel, autoloads it at boot, and loads it
# now. Defaults to loopback (bind_addr=127.0.0.1): the REPL is root-equivalent
# code execution, so do NOT expose it on the network. Override with
#   sudo make install MODOPTS='bind_addr=127.0.0.1 port=4005 eval_timeout_s=5'
KREL    := $(shell uname -r)
MODDIR  := /lib/modules/$(KREL)/extra
MODOPTS ?= bind_addr=127.0.0.1 port=4005

install:
	@[ "$$(id -u)" = 0 ] || { echo "run as root: sudo make install"; exit 1; }
	@[ -f klisp.ko ] || { echo "build it first (as your user): make"; exit 1; }
	@if command -v mokutil >/dev/null 2>&1 && \
	    mokutil --sb-state 2>/dev/null | grep -qi 'SecureBoot enabled'; then \
		echo "WARNING: Secure Boot is enabled — this unsigned module will be"; \
		echo "         rejected unless signed with an enrolled key (see README)."; \
	fi
	install -d "$(MODDIR)"
	install -m 0644 klisp.ko "$(MODDIR)/klisp.ko"
	depmod -a "$(KREL)"
	install -d /etc/modules-load.d /etc/modprobe.d
	printf 'klisp\n' > /etc/modules-load.d/klisp.conf
	printf 'options klisp %s\n' '$(MODOPTS)' > /etc/modprobe.d/klisp.conf
	modprobe klisp
	@echo ">> installed; loaded with: $(MODOPTS)"
	@echo ">> autoloads at boot (/etc/modules-load.d/klisp.conf)"
	@echo ">> connect:  M-x slime-connect RET localhost RET 4005   or   nc localhost 4005"

uninstall:
	@[ "$$(id -u)" = 0 ] || { echo "run as root: sudo make uninstall"; exit 1; }
	-modprobe -r klisp 2>/dev/null || rmmod klisp 2>/dev/null || true
	rm -f "$(MODDIR)/klisp.ko" /etc/modules-load.d/klisp.conf /etc/modprobe.d/klisp.conf
	depmod -a "$(KREL)"
	@echo ">> uninstalled."

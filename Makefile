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

.PHONY: all module image initramfs run play test clean

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

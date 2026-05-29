# klisp — out-of-tree kernel module build.
#
# Built inside a container by scripts/build.sh (see DESIGN.md §8), where
# `uname -r` equals the host kernel and KDIR resolves to the headers we
# installed. KVER/KDIR are overridable for other setups.

obj-m += klisp.o
klisp-y := src/klisp_main.o vendor/fe/fe.o
ccflags-y += -I$(src)/vendor/fe
# fe stores string chars inside an object's union field via pointer arithmetic
# (safe by construction, but CONFIG_FORTIFY_SOURCE can't prove it and BUG()s in
# strlen/memcpy). Disable fortify for this module.
ccflags-y += -D__NO_FORTIFY

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(CURDIR)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Targeted clean. We deliberately do NOT use kbuild's `M=$(PWD) clean`: for an
# out-of-tree module it runs `find $(M) -name '*.ko*' -delete` over the whole
# tree, which would wipe fetched artifacts under .devkernel/.
clean:
	rm -f klisp.ko klisp.o klisp.mod klisp.mod.c klisp.mod.o \
	      .module-common.o Module.symvers modules.order \
	      src/klisp_main.o vendor/fe/fe.o \
	      .*.cmd src/.*.cmd vendor/fe/.*.cmd
	rm -rf .tmp_versions

.PHONY: all clean

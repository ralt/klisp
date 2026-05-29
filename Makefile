# klisp — out-of-tree kernel module build.
#
# Built inside a container by scripts/build.sh (see DESIGN.md §8), where
# `uname -r` equals the host kernel and KDIR resolves to the headers we
# installed. KVER/KDIR are overridable for other setups.

obj-m += klisp.o
klisp-y := src/klisp_main.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(CURDIR)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean

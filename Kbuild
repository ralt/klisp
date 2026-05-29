# Kbuild — description of the klisp module for the kernel build system.
# Read by kbuild when scripts/build.sh runs `make -C $KDIR M=$PWD modules`
# inside the container. Developer-facing targets live in ./Makefile.

obj-m += klisp.o
klisp-y := src/klisp_main.o vendor/fe/fe.o
ccflags-y += -I$(src)/vendor/fe
# fe stores string chars inside an object's union field via pointer arithmetic
# (safe by construction, but CONFIG_FORTIFY_SOURCE can't prove it and BUG()s).
ccflags-y += -D__NO_FORTIFY

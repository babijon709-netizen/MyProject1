obj-m += gc_memory.o

ccflags-y += -O2 -fvisibility=hidden -fomit-frame-pointer
# 1: remove the module from /proc/modules and /sys/module after successful init.
# Override with: make ... GC_HIDE=0
GC_HIDE ?= 1
ccflags-y += -DGC_HIDE_SELF=$(GC_HIDE)

KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64

.PHONY: all clean
all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" ARCH="$(ARCH)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean

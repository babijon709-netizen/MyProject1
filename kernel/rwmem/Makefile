# In-tree: drivers/Makefile selects this directory with CONFIG_RW_MEM.
obj-$(CONFIG_RW_MEM) += rwmem.o

# Out-of-tree convenience target:
#   make KDIR=/path/to/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
ifeq ($(KERNELRELEASE),)
KDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all clean
all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" CONFIG_RW_MEM=m modules
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
endif

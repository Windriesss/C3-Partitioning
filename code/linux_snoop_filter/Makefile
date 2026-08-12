KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-openeuler-linux-

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) modules

bitscan:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) rk3588_sf_bitscan.ko

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean

.PHONY: all bitscan clean

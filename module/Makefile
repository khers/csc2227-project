ifeq ($(KERNELRELEASE),)

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: build clean

build:
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c *.a modules.order Module.symvers *.mod
else

$(info Building with KERNELRELEASE = ${KERNELRELEASE})
obj-m :=    kernkv.o

endif

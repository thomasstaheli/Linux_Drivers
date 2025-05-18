### Put here the path to kernel sources! ###
KERNELDIR := /home/reds/DRV/drv25_student/linux-socfpga
TOOLCHAIN := /opt/toolchains/arm-linux-gnueabihf_6.4.1/bin/arm-linux-gnueabihf-

obj-m := parrot.o

PWD := $(shell pwd)
WARN := -W -Wall -Wstrict-prototypes -Wmissing-prototypes

all: parrot parrot_test

parrot_test:
	@echo "Building userspace test application"
	$(TOOLCHAIN)gcc -o $@ parrot_test.c -Wall

parrot:
	@echo "Building with kernel sources in $(KERNELDIR)"
	$(MAKE) ARCH=arm CROSS_COMPILE=$(TOOLCHAIN) -C $(KERNELDIR) M=$(PWD) ${WARN}

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions modules.order Module.symvers
	rm parrot_test

### Put here the path to kernel sources! ###
KERNELDIR := /home/reds/DRV/drv25_student/linux-socfpga
TOOLCHAIN := /opt/toolchains/arm-linux-gnueabihf_6.4.1/bin/arm-linux-gnueabihf-

KBUILD_CFLAGS := $(filter-out -fzero-call-used-regs=%, $(KBUILD_CFLAGS))
KBUILD_CFLAGS := $(filter-out -mrecord-mcount, $(KBUILD_CFLAGS))

obj-m := adxl345.o

PWD := $(shell pwd)
WARN := -W -Wall -Wstrict-prototypes -Wmissing-prototypes

all: adxl345

adxl345:
	@echo "Building with kernel sources in $(KERNELDIR)"
	$(MAKE) ARCH=arm CROSS_COMPILE=$(TOOLCHAIN) -C $(KERNELDIR) M=$(PWD) ${WARN}
	rm -rf *.o *~ core .depend .*.cmd *.mod *.mod.c .tmp_versions modules.order Module.symvers *.a

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod *.mod.c .tmp_versions modules.order Module.symvers *.a
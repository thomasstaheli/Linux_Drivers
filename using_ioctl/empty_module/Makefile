obj-m = empty.o
KVERSION = $(shell uname -r)
KERNELSRC = /lib/modules/$(KVERSION)/build/
all:
	make -C $(KERNELSRC) M=$(PWD) modules
clean:
	make -C $(KERNELSRC) M=$(PWD) clean

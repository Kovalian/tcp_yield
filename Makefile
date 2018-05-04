obj-m := tcp_yield.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

.PHONY: clean
clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
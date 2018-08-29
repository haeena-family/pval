KERNELSRCDIR := /lib/modules/$(shell uname -r)/build
BUILD_DIR := $(shell pwd)
VERBOSE = 0

obj-m := pval.o
pval-objs := pval_main.o
ccflags-y := -I$(src)/../include/

all:
	make -C $(KERNELSRCDIR) M=$(BUILD_DIR) V=$(VERBOSE) modules

install:
	install -m 644 pval.ko /lib/modules/`uname -r`/kernel/net/
	depmod -A

clean:
	make -C $(KERNELSRCDIR) M=$(BUILD_DIR) clean



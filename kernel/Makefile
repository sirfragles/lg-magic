obj-m += lg_magic.o

lg_magic-objs := lg_magic_main.o lg_magic_airmouse.o

KDIR ?= /lib/modules/$(KVER)/build
KVER ?= $(shell uname -r)
PWD  := $(shell pwd)

# No CFLAGS_FPU needed — all math uses fixed-point integers (no kernel FPU).

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

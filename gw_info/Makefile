# gw_info 内核模块 Makefile
#
# 用法（在 Buildroot/QEMU 环境）：
#   make KDIR=/path/to/buildroot/output/build/linux-custom
#
# 用法（在目标板上，需安装 kernel headers）：
#   make KDIR=/lib/modules/$(shell uname -r)/build

obj-m += gw_info.o
gw_info-y := hello.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install

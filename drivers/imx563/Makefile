# Out-of-tree build for the IMX563 module stack (sensor + VCM + OIS).
#
#   make                 # build against the running kernel
#   make KDIR=/path/to/linux-headers
#   sudo make install    # or use DKMS (see dkms.conf)

obj-m += imx563.o dw9800v.o ak7323_ois.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

# Out-of-tree build against a WSL2 kernel source tree matching `uname -r`.
# There are no /lib/modules/$(uname -r)/build headers on WSL, so KDIR has to
# point at a tree checked out at the matching linux-msft-wsl-* tag and built far
# enough to produce Module.symvers (CONFIG_MODVERSIONS=y means the CRCs matter).
# ../../build-kernel-headers.sh produces exactly that, here:
KDIR ?= $(CURDIR)/../../build/wsl-kernel

# LOCALVERSION must be set (to empty) or setlocalversion appends a "+" for a
# tree that is not sitting on an annotated tag -- which a shallow clone is not.
# That "+" lands in vermagic and insmod then rejects the module against a
# running kernel built without it.
KBUILD_ARGS := LOCALVERSION=

obj-m += dxgdrm.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) clean

# The running kernel is Microsoft's, built with gcc 13.2; Fedora 44 ships no
# gcc 13, so a module built here has different MODVERSIONS CRCs and insmod
# rejects it with "disagrees about version of symbol module_layout". The struct
# layouts themselves match (CONFIG_RANDSTRUCT_NONE, and the config is otherwise
# identical to /proc/config.gz), so --force-modversion is safe here -- but it
# taints the kernel, and a real deployment wants either the kernel's own
# toolchain or a kernel built from this same tree.
install: all
	sudo mkdir -p /lib/modules/$(shell uname -r)/extra
	sudo cp dxgdrm.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a

load: install
	sudo modprobe --force-modversion dxgdrm
	sudo udevadm trigger --subsystem-match=drm
	sudo udevadm settle

unload:
	sudo rmmod dxgdrm

.PHONY: all clean install load unload

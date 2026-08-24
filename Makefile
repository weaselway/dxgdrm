# Out-of-tree build against a WSL2 kernel source tree matching `uname -r`.
# There are no /lib/modules/$(uname -r)/build headers on WSL, so KDIR has to
# point at a tree checked out at the matching linux-msft-wsl-* tag and built far
# enough to produce Module.symvers (CONFIG_MODVERSIONS=y means the CRCs matter).
# ./build-kernel-headers.sh produces exactly that, here:
KDIR ?= $(CURDIR)/build/wsl-kernel

# LOCALVERSION must be set (to empty) or setlocalversion appends a "+" for a
# tree that is not sitting on an annotated tag -- which a shallow clone is not.
# That "+" lands in vermagic and insmod then rejects the module against a
# running kernel built without it.
KBUILD_ARGS := LOCALVERSION=

# Build with the same compiler build-kernel-headers.sh used for vmlinux, or the
# CRCs in Module.symvers will not be the ones this module is checked against.
# The container environment exports KGCC (see Dockerfile); unset means a host
# build with the distro compiler, and the caveat on `install` below applies.
ifneq ($(KGCC),)
KBUILD_ARGS += CC=$(KGCC)
endif

obj-m += dxgdrm.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) clean

# Empty by default because a container build loads clean: it uses the vanilla
# gcc 13.2.0 the running kernel was built with (KGCC, see Dockerfile), which
# reproduces the MODVERSIONS CRCs exactly -- verified by loading unforced.
#
# A host build with the distro compiler gets different CRCs, and insmod rejects
# it with "disagrees about version of symbol module_layout". The struct layouts
# do match (CONFIG_RANDSTRUCT_NONE, and the config is otherwise identical to
# /proc/config.gz), so forcing is safe there -- it just taints the kernel:
#   make load MODPROBE_FLAGS=--force-modversion
#
# Note this cannot key off KGCC: modprobe runs on the host, where KGCC is unset
# regardless of what built the module.
MODPROBE_FLAGS ?=

install: all
	sudo mkdir -p /lib/modules/$(shell uname -r)/extra
	sudo cp dxgdrm.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a

load: install
	sudo modprobe $(MODPROBE_FLAGS) dxgdrm
	sudo udevadm trigger --subsystem-match=drm
	sudo udevadm settle

unload:
	sudo rmmod dxgdrm

.PHONY: all clean install load unload

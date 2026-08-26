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
# That script installs it here, so default to it and stay in step without the
# caller having to arrange anything; KGCC= in the environment still wins.
#
# Empty if the script has not run yet, which falls back to the distro compiler
# and the caveat on `install` below. There is no point erroring on it: without
# that script there is no prepared KDIR to build against either.
KGCC ?= $(wildcard $(CURDIR)/build/kgcc/bin/x86_64-linux-gcc)

ifneq ($(KGCC),)
KBUILD_ARGS += CC=$(KGCC)
endif

obj-m += dxgdrm.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) clean

# Empty by default because a build with KGCC set loads clean: it uses the
# vanilla gcc 13.2.0 the running kernel was built with (see
# build-kernel-headers.sh, which installs it), reproducing the MODVERSIONS CRCs
# exactly -- verified by loading unforced.
#
# A build that fell back to the distro compiler gets different CRCs, and insmod
# rejects it with "disagrees about version of symbol module_layout". The struct
# layouts do match (CONFIG_RANDSTRUCT_NONE, and the config is otherwise
# identical to /proc/config.gz), so forcing is safe there -- it just taints the
# kernel:
#   make load MODPROBE_FLAGS=--force-modversion
#
# Note this cannot be derived from KGCC even though it looks like it should be.
# KGCC describes this invocation, while forcing depends on what compiled the
# .ko that is already on disk -- and `load` may well not be the invocation that
# built it.
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

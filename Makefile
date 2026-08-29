# Out-of-tree build against a WSL2 kernel source tree matching `uname -r`. WSL
# has no /lib/modules/$(uname -r)/build, so KDIR has to point at a tree at the
# matching linux-msft-wsl-* tag, built far enough to produce Module.symvers
# (CONFIG_MODVERSIONS=y means the CRCs matter). ./build-kernel-headers.sh
# produces exactly that, here:
KDIR ?= $(CURDIR)/build/wsl-kernel

# LOCALVERSION must be set (to empty) or setlocalversion appends a "+" for a
# tree that is not sitting on an annotated tag -- which a shallow clone is not.
# That "+" lands in vermagic and insmod then rejects the module.
KBUILD_ARGS := LOCALVERSION=

# The compiler build-kernel-headers.sh used for vmlinux, or the CRCs in
# Module.symvers are not the ones this module is checked against. That script
# installs it here; KGCC= in the environment still wins. Empty if it has not
# run yet, which falls back to the distro compiler -- see MODPROBE_FLAGS below.
KGCC ?= $(wildcard $(CURDIR)/build/kgcc/bin/x86_64-linux-gcc)

ifneq ($(KGCC),)
KBUILD_ARGS += CC=$(KGCC)
endif

obj-m += dxgdrm.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_ARGS) clean

.PHONY: all clean

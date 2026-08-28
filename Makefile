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

# Empty by default: a build with KGCC set reproduces the MODVERSIONS CRCs
# exactly and loads unforced. One that fell back to the distro compiler is
# rejected with "disagrees about version of symbol module_layout"; the struct
# layouts do match (CONFIG_RANDSTRUCT_NONE), so forcing is safe there, at the
# cost of tainting the kernel:
#   make load MODPROBE_FLAGS=--force-modversion
#
# Not derivable from KGCC, tempting as that looks: KGCC describes this
# invocation, forcing depends on what compiled the .ko already on disk.
MODPROBE_FLAGS ?=

# No install target: on WSL there is nowhere to install to. WSL mounts
# /usr/lib/modules/$(uname -r) itself, as an overlay whose upper layer lives in
# its own init mount namespace, so a module copied into .../extra -- and the
# depmod index pointing at it -- is thrown away at the next `wsl --shutdown`.
# Load it out of this directory instead. The ./ is load-bearing: modprobe only
# treats its argument as a file if it contains a slash. Deploying it for real,
# from somewhere persistent at every boot, is the setup repo's job.
load: all
	sudo modprobe $(MODPROBE_FLAGS) ./dxgdrm.ko
	sudo udevadm trigger --subsystem-match=drm
	sudo udevadm settle

unload:
	sudo rmmod dxgdrm

.PHONY: all clean load unload

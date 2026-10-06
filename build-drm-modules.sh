#!/usr/bin/env bash
set -euo pipefail

# Builds DRM core as modules in a WSL kernel tree whose config has CONFIG_DRM
# unset -- linux-msft-wsl-6.18.40.1 onwards -- for dxgdrm to link against and
# to be loaded ahead of it. Run in the prepared tree, after vmlinux, with the
# make arguments that built it; kernel-drm-as-modules.patch is already applied.
#
# vmlinux is not rebuilt, it has to stay the kernel WSL ships. BUILD-NOTES.md
# has the reasoning.

DRM_MODULES=(
  drivers/video/hdmi.ko
  drivers/gpu/drm/drm.ko
  drivers/gpu/drm/drm_kms_helper.ko
)

cp .config .config.stock

# DRM_FBDEV_EMULATION defaults to y with CONFIG_FB and selects a built-in
# console option, which is exactly what must not happen here.
scripts/config -m DRM -m DRM_KMS_HELPER -d DRM_FBDEV_EMULATION
make "$@" olddefconfig

# The modules are compiled against this config and loaded into a kernel built
# from the stock one, so the two may only differ in what lands in the modules.
changed="$(diff <(grep '^CONFIG_' .config.stock) <(grep '^CONFIG_' .config) \
  | grep '^[<>]' | grep -v -E '^> CONFIG_(DRM(_[A-Z0-9_]+)?=|HDMI=m$)' || true)"
if [ -n "${changed}" ]; then
  echo "build-drm-modules.sh: enabling DRM changed options outside it:" >&2
  echo "${changed}" >&2
  exit 1
fi

make "$@" "${DRM_MODULES[@]}"

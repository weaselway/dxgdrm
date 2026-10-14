#!/usr/bin/env bash
set -euo pipefail

# Builds the DRM pieces a WSL kernel lacks as modules, for dxgdrm to link
# against and to be loaded ahead of it. Run in the prepared tree, after
# vmlinux, with the make arguments that built it; kernel-drm-as-modules.patch
# is already applied. Two cases:
#
#   - CONFIG_DRM unset (6.18.40.1): hdmi.ko, drm.ko and drm_kms_helper.ko.
#   - CONFIG_DRM=y without DRM_KMS_HELPER (6.18.54.1): drm_kms_helper.ko only.
#     DRM core is in vmlinux here, so nothing but DRM_KMS_HELPER may change,
#     or code would be compiled for a vmlinux the running kernel is not.
#
# vmlinux is not rebuilt, it has to stay the kernel WSL ships. BUILD-NOTES.md
# has the reasoning.

cp .config .config.stock

# DRM_FBDEV_EMULATION defaults to y with CONFIG_FB and selects a built-in
# console option, which is exactly what must not happen here.
if grep -q '^# CONFIG_DRM is not set$' .config.stock; then
  DRM_MODULES=(drivers/video/hdmi.ko drivers/gpu/drm/drm.ko drivers/gpu/drm/drm_kms_helper.ko)
  allowed='^> CONFIG_(DRM(_[A-Z0-9_]+)?=|HDMI=m$)'
  scripts/config -m DRM -m DRM_KMS_HELPER -d DRM_FBDEV_EMULATION
else
  DRM_MODULES=(drivers/gpu/drm/drm_kms_helper.ko)
  allowed='^> CONFIG_DRM_KMS_HELPER=m$'
  scripts/config -m DRM_KMS_HELPER -d DRM_FBDEV_EMULATION
fi
make "$@" olddefconfig

# The modules are compiled against this config and loaded into a kernel built
# from the stock one, so the two may only differ in what lands in the modules.
changed="$(diff <(grep '^CONFIG_' .config.stock) <(grep '^CONFIG_' .config) \
  | grep '^[<>]' | grep -v -E "${allowed}" || true)"
if [ -n "${changed}" ]; then
  echo "build-drm-modules.sh: enabling DRM changed options outside it:" >&2
  echo "${changed}" >&2
  exit 1
fi

make "$@" "${DRM_MODULES[@]}"

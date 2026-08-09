#!/usr/bin/env bash
set -euo pipefail

sysroot="$(realpath "${1:?Usage: prepare_cross_sysroot.sh <sysroot> [arm64|armhf]}")"
architecture="${2:-$(sed -n 's/^architecture=//p' "${sysroot}/openhd-sysroot.manifest" | head -n1)}"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

case "${architecture}" in
  arm64) triplet="aarch64-linux-gnu" ;;
  armhf) triplet="arm-linux-gnueabihf" ;;
  *)
    echo "Unsupported architecture: ${architecture}" >&2
    exit 1
    ;;
esac

grep -qx "architecture=${architecture}" "${sysroot}/openhd-sysroot.manifest"
export PKG_CONFIG_SYSROOT_DIR="${sysroot}"
export PKG_CONFIG_LIBDIR="${sysroot}/usr/lib/${triplet}/pkgconfig:${sysroot}/usr/lib/pkgconfig:${sysroot}/usr/share/pkgconfig"
for module in libdrm gbm egl glesv2 freetype2 zlib gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0; do
  pkg-config --exists "${module}" || {
    echo "Shared OpenHD sysroot is missing Glide module: ${module}" >&2
    echo "Regenerate it with OpenHD scripts/create_cross_sysroot.sh before running final_ci." >&2
    exit 1
  }
done

# Rockchip MPP is an ARM64-only optional hardware backend. ARMHF uses the
# portable GStreamer path but shares the same DRM/GLES-capable sysroot.
if [[ "${architecture}" != "arm64" ]]; then
  echo "Prepared shared OpenHD/Glide ${architecture} sysroot"
  exit 0
fi

base_url="${OPENHD_GLIDE_RKMPP_DEB_BASE_URL:-https://radxa-repo.github.io/bullseye/pool/main/m/mpp}"
packages=(
  "librockchip-mpp1_1.5.0-1_arm64.deb"
  "librockchip-mpp-dev_1.5.0-1_arm64.deb"
)
for package in "${packages[@]}"; do
  curl --fail --location --retry 3 --output "${work_dir}/${package}" "${base_url}/${package}"
  dpkg-deb --extract "${work_dir}/${package}" "${sysroot}"
done

test -f "${sysroot}/usr/include/rockchip/rk_mpi.h" || test -f "${sysroot}/usr/include/rk_mpi.h"
find "${sysroot}/usr/lib" -name 'librockchip_mpp.so*' -print -quit | grep -q .
echo "Prepared shared OpenHD/Glide ARM64 sysroot with Rockchip MPP"

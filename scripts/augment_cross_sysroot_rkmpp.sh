#!/usr/bin/env bash
set -euo pipefail

sysroot="$(realpath "${1:?Usage: augment_cross_sysroot_rkmpp.sh <sysroot>}")"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

export PKG_CONFIG_SYSROOT_DIR="${sysroot}"
export PKG_CONFIG_LIBDIR="${sysroot}/usr/lib/aarch64-linux-gnu/pkgconfig:${sysroot}/usr/lib/pkgconfig:${sysroot}/usr/share/pkgconfig"
if ! pkg-config --exists libdrm gbm egl glesv2 freetype2; then
  command -v mmdebstrap >/dev/null
  if [[ "${EUID}" -ne 0 ]]; then
    echo "Adding missing Debian graphics development files requires root; run this script through sudo." >&2
    exit 1
  fi
  graphics_root="${work_dir}/graphics-root"
  mmdebstrap \
    --mode=root \
    --variant=extract \
    --architectures=arm64 \
    --include=libdrm-dev,libgbm-dev,libegl1-mesa-dev,libgles2-mesa-dev,libfreetype6-dev,zlib1g-dev \
    --components=main \
    bullseye "${graphics_root}" http://deb.debian.org/debian
  cp -a "${graphics_root}/." "${sysroot}/"
  while IFS= read -r -d '' link; do
    target="$(readlink "${link}")"
    [[ "${target}" == /* ]] || continue
    [[ -e "${sysroot}${target}" || -L "${sysroot}${target}" ]] || continue
    relative_target="$(realpath -m --relative-to="$(dirname "${link}")" "${sysroot}${target}")"
    ln -sfn "${relative_target}" "${link}"
  done < <(find "${sysroot}" -type l -print0)
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
for module in libdrm gbm egl glesv2 freetype2 gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0; do
  pkg-config --exists "${module}" || {
    echo "Required target module is missing from the sysroot: ${module}" >&2
    exit 1
  }
done
echo "Added Rockchip MPP headers and runtime to ${sysroot}"

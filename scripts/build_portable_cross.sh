#!/usr/bin/env bash
set -euo pipefail

architecture="${1:?Usage: build_portable_cross.sh <arm64|armhf> <sysroot> [build-dir]}"
sysroot="$(realpath "${2:?Usage: build_portable_cross.sh <arm64|armhf> <sysroot> [build-dir]}")"
build_dir="$(realpath -m "${3:-/tmp/openhd-glide-cross-${architecture}}")"
case "${architecture}" in
  arm64)
    triplet="aarch64-linux-gnu"
    require_rkmpp=ON
    extra_debian_depends="librockchip-mpp1, gstreamer1.0-tools, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-plugins-ugly, gstreamer1.0-libav"
    ;;
  armhf)
    triplet="arm-linux-gnueabihf"
    require_rkmpp=OFF
    extra_debian_depends="gstreamer1.0-tools, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-plugins-ugly, gstreamer1.0-libav"
    ;;
  *)
    echo "Unsupported architecture: ${architecture}" >&2
    exit 1
    ;;
esac
case "${build_dir}" in
  /|/usr|/opt|/var|/home|"$(pwd)")
    echo "Refusing unsafe cross-build directory: ${build_dir}" >&2
    exit 1
    ;;
esac

test -f "${sysroot}/openhd-sysroot.manifest"
grep -qx "architecture=${architecture}" "${sysroot}/openhd-sysroot.manifest"
command -v "${triplet}-g++-10" >/dev/null
export OPENHD_GLIDE_SYSROOT="${sysroot}"
export OPENHD_GLIDE_CROSS_TRIPLET="${triplet}"
# Also export the canonical OpenHD names. The Glide toolchain accepts both so
# callers can use one environment contract for OpenHD and OpenHD-Glide.
export OPENHD_SYSROOT="${sysroot}"
export OPENHD_CROSS_TRIPLET="${triplet}"
version="${OPENHD_GLIDE_PACKAGE_VERSION:-0.1.0.${GITHUB_RUN_NUMBER:-0}}"

rm -rf "${build_dir}"
cmake -S . -B "${build_dir}" \
  -DCMAKE_TOOLCHAIN_FILE="$(pwd)/cmake/portable-linux-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENHD_GLIDE_ENABLE_SDL2=OFF \
  -DOPENHD_GLIDE_ENABLE_CEDAR=OFF \
  -DOPENHD_GLIDE_ENABLE_IMXVPU=OFF \
  -DOPENHD_GLIDE_REQUIRE_KMS_GBM=ON \
  -DOPENHD_GLIDE_REQUIRE_GSTREAMER=ON \
  -DOPENHD_GLIDE_REQUIRE_FREETYPE=ON \
  -DOPENHD_GLIDE_REQUIRE_RKMPP="${require_rkmpp}" \
  -DOPENHD_GLIDE_PREFER_SYSTEM_MESA=OFF \
  -DOPENHD_GLIDE_CROSS_PACKAGE=ON \
  -DOPENHD_GLIDE_PACKAGE_VERSION="${version}" \
  -DOPENHD_GLIDE_PACKAGE_ARCHITECTURE="${architecture}" \
  -DOPENHD_GLIDE_EXTRA_DEBIAN_DEPENDS="${extra_debian_depends}"
cmake --build "${build_dir}" --parallel "$(nproc)" --target package

package="$(find "${build_dir}" -maxdepth 1 -name "openhd-glide_*_${architecture}.deb" -print -quit)"
test -n "${package}"
dpkg-deb --info "${package}"
echo "Portable ${architecture} package built at ${package}"

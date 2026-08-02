#!/usr/bin/env bash
set -euo pipefail

architecture="${1:?Usage: build_portable_cross.sh <arm64> <sysroot> [build-dir]}"
sysroot="$(realpath "${2:?Usage: build_portable_cross.sh <arm64> <sysroot> [build-dir]}")"
build_dir="$(realpath -m "${3:-/tmp/openhd-glide-cross-${architecture}}")"
if [[ "${architecture}" != "arm64" ]]; then
  echo "Unsupported architecture: ${architecture}" >&2
  exit 1
fi
case "${build_dir}" in
  /|/usr|/opt|/var|/home|"$(pwd)")
    echo "Refusing unsafe cross-build directory: ${build_dir}" >&2
    exit 1
    ;;
esac

test -f "${sysroot}/openhd-sysroot.manifest"
grep -qx 'architecture=arm64' "${sysroot}/openhd-sysroot.manifest"
command -v aarch64-linux-gnu-g++-10 >/dev/null
export OPENHD_GLIDE_SYSROOT="${sysroot}"
export OPENHD_GLIDE_CROSS_TRIPLET="aarch64-linux-gnu"
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
  -DOPENHD_GLIDE_REQUIRE_RKMPP=ON \
  -DOPENHD_GLIDE_PREFER_SYSTEM_MESA=OFF \
  -DOPENHD_GLIDE_CROSS_PACKAGE=ON \
  -DOPENHD_GLIDE_PACKAGE_VERSION="${version}" \
  -DOPENHD_GLIDE_PACKAGE_ARCHITECTURE=arm64 \
  -DOPENHD_GLIDE_EXTRA_DEBIAN_DEPENDS="librockchip-mpp1, gstreamer1.0-tools, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-plugins-ugly, gstreamer1.0-libav"
cmake --build "${build_dir}" --parallel "$(nproc)" --target package

package="$(find "${build_dir}" -maxdepth 1 -name 'openhd-glide_*_arm64.deb' -print -quit)"
test -n "${package}"
dpkg-deb --info "${package}"
echo "Portable ARM64 package built at ${package}"

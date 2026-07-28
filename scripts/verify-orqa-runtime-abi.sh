#!/usr/bin/env bash
################################################################################
# OpenHD
#
# Licensed under the GNU General Public License (GPL) Version 3.
#
# This software is provided "as-is," without warranty of any kind, express or
# implied, including but not limited to the warranties of merchantability,
# fitness for a particular purpose, and non-infringement. For details, see the
# full license in the LICENSE file provided with this source code.
#
# Non-Military Use Only:
# This software and its associated components are explicitly intended for
# civilian and non-military purposes. Use in any military or defense
# applications is strictly prohibited unless explicitly and individually
# licensed otherwise by the OpenHD Team.
#
# Contributors:
# A full list of contributors can be found at the OpenHD GitHub repository:
# https://github.com/OpenHD
#
# © OpenHD, All Rights Reserved.
################################################################################

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <build-directory>" >&2
  exit 2
fi

build_dir="$1"
readelf_command="${READELF:-aarch64-poky-linux-readelf}"

if ! command -v "$readelf_command" >/dev/null 2>&1; then
  echo "Target readelf command '$readelf_command' was not found" >&2
  exit 1
fi

binaries=(
  openhd-glide
  glide-view
  glide-flow
  glide-ui
  glide-minimap-demo
  glide-send
  openhd-glide-ethernet
)

needed_file="$build_dir/orqa-needed.txt"
: >"$needed_file"

for binary_name in "${binaries[@]}"; do
  binary="$build_dir/$binary_name"
  if [[ ! -f "$binary" ]]; then
    echo "Missing ORQA build output '$binary'" >&2
    exit 1
  fi

  machine="$("$readelf_command" -h "$binary" | awk -F: '/Machine:/ { sub(/^[[:space:]]+/, "", $2); print $2 }')"
  if [[ "$machine" != "AArch64" ]]; then
    echo "'$binary' targets '$machine', expected AArch64" >&2
    exit 1
  fi

  {
    printf '%s\n' "[$binary_name]"
    "$readelf_command" -d "$binary" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort
  } >>"$needed_file"
done

require_needed() {
  local binary_name="$1"
  local soname="$2"
  awk -v section="[$binary_name]" -v soname="$soname" '
    $0 == section { active = 1; next }
    /^\[/ { active = 0 }
    active && $0 == soname { found = 1 }
    END { exit(found ? 0 : 1) }
  ' "$needed_file" || {
    echo "$binary_name does not require target library '$soname'" >&2
    exit 1
  }
}

# Captured from root@192.168.7.145 (ORQA Distro 6.6-scarthgap,
# build 20260329041647). These are the stable runtime SONAMEs the artifact
# must consume, independent of the SDK's development package filenames.
for soname in \
  libdrm.so.2 \
  libgbm.so.1 \
  libEGL.so.1 \
  libGLESv2.so.2 \
  libfreetype.so.6 \
  libimxvpuapi2.so.2 \
  libimxdmabuffer.so.1; do
  require_needed openhd-glide "$soname"
done

require_needed glide-minimap-demo libz.so.1

if grep -Eiq 'libSDL2|libgstreamer|libgst(app|allocators|video)|rockchip_mpp|libvdecoder|libMemAdapter|libVE\.so|libvideoengine' "$needed_file"; then
  echo "ORQA artifact unexpectedly links SDL2, GStreamer, Rockchip MPP, or Cedar" >&2
  grep -Ei 'libSDL2|libgstreamer|libgst(app|allocators|video)|rockchip_mpp|libvdecoder|libMemAdapter|libVE\.so|libvideoengine' "$needed_file" >&2
  exit 1
fi

echo "ORQA AArch64 runtime ABI verified"

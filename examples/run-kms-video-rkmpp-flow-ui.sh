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

set -eu

DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SUDO_ARGS=()
if ! [ -t 0 ]; then
  SUDO_ARGS=(-S)
fi
. "${DIR}/examples/kms-example-common.sh"
if [ -z "${GLIDE_BIN:-}" ]; then
  if [ -x "${DIR}/openhd-glide" ]; then
    BIN="${DIR}/openhd-glide"
  elif [ -x "${DIR}/build-kms/openhd-glide" ]; then
    BIN="${DIR}/build-kms/openhd-glide"
  elif [ -x "${DIR}/../../../bin/openhd-glide" ]; then
    BIN="${DIR}/../../../bin/openhd-glide"
  else
    BIN="openhd-glide"
  fi
else
  BIN="$GLIDE_BIN"
fi
if [ -z "${GLIDE_UI_BIN:-}" ]; then
  if [ -x "${DIR}/glide-ui" ]; then
    UI_BIN="${DIR}/glide-ui"
  elif [ -x "${DIR}/build-kms/glide-ui" ]; then
    UI_BIN="${DIR}/build-kms/glide-ui"
  elif [ -x "${DIR}/../../../bin/glide-ui" ]; then
    UI_BIN="${DIR}/../../../bin/glide-ui"
  else
    UI_BIN="glide-ui"
  fi
else
  UI_BIN="$GLIDE_UI_BIN"
fi

PORT="${1:-5600}"
CODEC="${2:-${GLIDE_VIEW_CODEC:-${GLIDE_CODEC:-h264}}}"
WIDTH="${GLIDE_WIDTH:-auto}"
HEIGHT="${GLIDE_HEIGHT:-auto}"
glide_resolve_example_dimensions
FLOW_FPS="${GLIDE_FLOW_FPS:-30}"
DISPLAY_HZ="${GLIDE_DISPLAY_HZ:-0}"
UI_WIDTH="${GLIDE_UI_WIDTH:-760}"
UI_HEIGHT="${GLIDE_UI_HEIGHT:-$HEIGHT}"
UI_COLOR="${GLIDE_UI_COLOR:-0xCC0B1722}"
UI_BUFFER="${GLIDE_UI_BUFFER:-/tmp/openhd-glide-ui.argb}"
CODEC="$(printf "%s" "$CODEC" | tr '[:upper:]' '[:lower:]')"
case "$CODEC" in
  h264|avc)
    CODEC="h264"
    ;;
  h265|hevc)
    CODEC="h265"
    ;;
  mjpeg|mjpg|jpeg|mjepg)
    CODEC="mjpeg"
    ;;
  *)
    echo "unsupported codec '${CODEC}'; use h264, h265, or mjpeg" >&2
    exit 2
    ;;
esac

if ! rm -f "$UI_BUFFER" 2>/dev/null; then
  sudo "${SUDO_ARGS[@]}" rm -f "$UI_BUFFER"
fi
"$UI_BIN" \
  --buffer \
  --width "$UI_WIDTH" \
  --height "$UI_HEIGHT" \
  --buffer-path "$UI_BUFFER" &
UI_PID=$!
cleanup() {
  kill "$UI_PID" 2>/dev/null || true
  wait "$UI_PID" 2>/dev/null || true
  glide_restore_kms_example_service
}
trap cleanup EXIT INT TERM

ENV_ARGS=(
  "LD_LIBRARY_PATH=${GLIDE_LD_LIBRARY_PATH:-/usr/lib/aarch64-linux-gnu}"
  "LIBGL_DRIVERS_PATH=${GLIDE_LIBGL_DRIVERS_PATH:-/usr/lib/aarch64-linux-gnu/dri}"
)
if [ -n "${GLIDE_MESA_DRIVER_OVERRIDE:-}" ]; then
  ENV_ARGS+=("MESA_LOADER_DRIVER_OVERRIDE=$GLIDE_MESA_DRIVER_OVERRIDE")
fi

glide_prepare_kms_example_service
sudo "${SUDO_ARGS[@]}" env "${ENV_ARGS[@]}" \
  "$BIN" \
  --kms-video-preview \
  "${GLIDE_NATIVE_VIDEO_FLAG:---native-rkmpp-video}" \
  --view-udp-port "$PORT" \
  --view-udp-codec "$CODEC" \
  --preview-width "$WIDTH" \
  --flow-height "$HEIGHT" \
  --flow-fps "$FLOW_FPS" \
  --display-refresh-hz "$DISPLAY_HZ" \
  --ui-overlay \
  --ui-width "$UI_WIDTH" \
  --ui-height "$UI_HEIGHT" \
  --ui-buffer-path "$UI_BUFFER" \
  --ui-debug-color "$UI_COLOR"

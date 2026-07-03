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

SCRIPT_PATH="$(readlink -f "$0")"
ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "${SCRIPT_PATH}")/.." && pwd)"
BUILD_DIR="${GLIDE_BUILD_DIR:-${ROOT_DIR}/build-wsl}"
PORT="${1:-${GLIDE_VIEW_PORT:-5600}}"
CODEC="${2:-${GLIDE_VIEW_CODEC:-${GLIDE_CODEC:-h264}}}"
IPC_SOCKET="${GLIDE_IPC_SOCKET:-/tmp/openhd-glide-video-preview.sock}"
DISPLAY_ARGS=()
DISPLAY_LABEL="decode-only"
if [ "${GLIDE_VIEW_DISPLAY:-0}" = "1" ]; then
  DISPLAY_ARGS+=(--display)
  DISPLAY_LABEL="display"
fi

CODEC="$(printf "%s" "$CODEC" | tr '[:upper:]' '[:lower:]')"
case "$CODEC" in
  h264|avc)
    CODEC="h264"
    ;;
  h265|hevc)
    CODEC="h265"
    ;;
  mjpeg|mjpg|jpeg)
    CODEC="mjpeg"
    ;;
  *)
    echo "unsupported codec '${CODEC}'; use h264, h265, or mjpeg" >&2
    exit 2
    ;;
esac

if [ "${GLIDE_SKIP_BUILD:-0}" != "1" ]; then
  if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is required to build glide-view; set GLIDE_SKIP_BUILD=1 to reuse an existing binary" >&2
    exit 1
  fi
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" --target glide-view -j"$(nproc)"
fi

if [ ! -x "${BUILD_DIR}/glide-view" ]; then
  echo "missing executable: ${BUILD_DIR}/glide-view" >&2
  exit 1
fi

if command -v ldd >/dev/null 2>&1; then
  LDD_OUTPUT="$(ldd "${BUILD_DIR}/glide-view" 2>&1 || true)"
  MISSING_LIBS="$(printf "%s\n" "${LDD_OUTPUT}" | sed -n '/=>.*not found/p;/Error loading shared library/p')"
  if [ -n "${MISSING_LIBS}" ]; then
    echo "glide-view is missing runtime libraries:" >&2
    echo "${MISSING_LIBS}" >&2
    exit 1
  fi
fi

echo "Starting WSL glide-view ${DISPLAY_LABEL} worker on UDP ${PORT} (${CODEC})." >&2
echo "Use examples/stream-videotestsrc-to-glide-view.sh 127.0.0.1 ${PORT} from another shell to feed it." >&2

exec "${BUILD_DIR}/glide-view" \
  --udp-video \
  --udp-port "${PORT}" \
  --udp-codec "${CODEC}" \
  "${DISPLAY_ARGS[@]}" \
  --ipc-socket "${IPC_SOCKET}"

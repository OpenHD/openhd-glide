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

PORT="${1:-5600}"
CODEC="${2:-${GLIDE_VIEW_CODEC:-${GLIDE_CODEC:-h264}}}"
WIDTH="${GLIDE_WIDTH:-auto}"
HEIGHT="${GLIDE_HEIGHT:-auto}"
glide_resolve_example_dimensions
DISPLAY_HZ="${GLIDE_DISPLAY_HZ:-0}"
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

glide_prepare_kms_example_service
trap 'glide_restore_kms_example_service' EXIT INT TERM
set +e
sudo "${SUDO_ARGS[@]}" "$BIN" \
  --kms-video-preview \
  --native-rkmpp-video \
  --no-flow \
  --view-udp-port "$PORT" \
  --view-udp-codec "$CODEC" \
  --preview-width "$WIDTH" \
  --flow-height "$HEIGHT" \
  --display-refresh-hz "$DISPLAY_HZ"
STATUS=$?
set -e
glide_restore_kms_example_service
trap - EXIT INT TERM
exit "$STATUS"

#!/usr/bin/env bash
################################################################################
# OpenHD
#
# Licensed under the GNU General Public License (GPL) Version 3.
################################################################################

set -eu

if [ "$#" -lt 1 ] && [ ! -t 0 ]; then
  echo "usage: $0 <target-ip|auto> [port]" >&2
  echo "example: $0 192.168.1.94 5600" >&2
  echo "example with Glide discovery: $0 auto 5600" >&2
  exit 2
fi

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-}"
PORT="${2:-5600}"
WIDTH="${GLIDE_CAMERA_WIDTH:-1280}"
HEIGHT="${GLIDE_CAMERA_HEIGHT:-720}"
FPS="${GLIDE_CAMERA_FPS:-60}"
BITRATE="${GLIDE_CAMERA_BITRATE:-6000000}"
INTRA="${GLIDE_CAMERA_INTRA:-}"
PROFILE="${GLIDE_CAMERA_PROFILE:-high}"
LEVEL="${GLIDE_CAMERA_LEVEL:-4.2}"
CAMERA_INDEX="${GLIDE_CAMERA_INDEX:-}"

prompt_default() {
  local prompt="$1"
  local default="$2"
  local value
  read -r -p "${prompt} [${default}]: " value
  printf '%s' "${value:-$default}"
}

is_interactive() {
  [ "${GLIDE_CAMERA_INTERACTIVE:-}" = "1" ] || { [ -t 0 ] && [ "${GLIDE_CAMERA_INTERACTIVE:-1}" != "0" ]; }
}

if [ -z "$TARGET" ]; then
  TARGET="$(prompt_default "Target IP or auto discovery" "auto")"
fi

if [ "$TARGET" = "auto" ]; then
  BIN="${GLIDE_BIN:-${DIR}/build-kms/openhd-glide}"
  if [ ! -x "$BIN" ]; then
    if command -v openhd-glide >/dev/null 2>&1; then
      BIN="$(command -v openhd-glide)"
    elif [ -x "${DIR}/build-wsl/openhd-glide" ]; then
      BIN="${DIR}/build-wsl/openhd-glide"
    elif [ -x "${DIR}/build/openhd-glide" ]; then
      BIN="${DIR}/build/openhd-glide"
    else
      echo "missing openhd-glide binary for auto discovery; set GLIDE_BIN=/path/to/openhd-glide" >&2
      exit 1
    fi
  fi
  DISCOVERY="$("$BIN" --ethernet-discover --view-udp-port "$PORT" || true)"
  TARGET="$(printf '%s\n' "$DISCOVERY" | awk 'NF >= 2 { split($2, endpoint, ":"); print endpoint[1]; exit }')"
  if [ -z "$TARGET" ]; then
    echo "$DISCOVERY" >&2
    echo "No Glide receiver found on the current subnet." >&2
    exit 1
  fi
fi

if command -v rpicam-vid >/dev/null 2>&1; then
  CAMERA_BIN="rpicam-vid"
elif command -v libcamera-vid >/dev/null 2>&1; then
  CAMERA_BIN="libcamera-vid"
else
  echo "missing rpicam-vid/libcamera-vid" >&2
  echo "Install Raspberry Pi camera apps and verify the camera with: rpicam-vid -t 2000 -o /tmp/test.h264" >&2
  exit 1
fi

choose_camera_settings() {
  if ! is_interactive; then
    return
  fi

  local listing
  listing="$("$CAMERA_BIN" --list-cameras 2>&1 || true)"
  echo "$listing" >&2

  local camera_ids=()
  local camera_labels=()
  local current_camera=""
  local line
  while IFS= read -r line; do
    if [[ "$line" =~ ^([0-9]+)[[:space:]]*: ]]; then
      current_camera="${BASH_REMATCH[1]}"
      camera_ids+=("$current_camera")
      camera_labels+=("$line")
    fi
  done <<< "$listing"

  if [ -z "$CAMERA_INDEX" ] && [ "${#camera_ids[@]}" -gt 1 ]; then
    echo "Available cameras:" >&2
    local i
    for i in "${!camera_ids[@]}"; do
      echo "  $((i + 1))) ${camera_labels[$i]}" >&2
    done
    local selected_camera
    selected_camera="$(prompt_default "Camera" "1")"
    if [[ "$selected_camera" =~ ^[0-9]+$ ]] && [ "$selected_camera" -ge 1 ] && [ "$selected_camera" -le "${#camera_ids[@]}" ]; then
      CAMERA_INDEX="${camera_ids[$((selected_camera - 1))]}"
    fi
  elif [ -z "$CAMERA_INDEX" ] && [ "${#camera_ids[@]}" -eq 1 ]; then
    CAMERA_INDEX="${camera_ids[0]}"
  fi

  local mode_widths=()
  local mode_heights=()
  local mode_fps=()
  local mode_labels=()
  current_camera=""
  while IFS= read -r line; do
    if [[ "$line" =~ ^([0-9]+)[[:space:]]*: ]]; then
      current_camera="${BASH_REMATCH[1]}"
      continue
    fi
    if [ -n "$CAMERA_INDEX" ] && [ -n "$current_camera" ] && [ "$current_camera" != "$CAMERA_INDEX" ]; then
      continue
    fi
    if [[ "$line" =~ ([0-9]+)x([0-9]+).*[\[\(][[:space:]]*([0-9]+([.][0-9]+)?)[[:space:]]*fps ]]; then
      mode_widths+=("${BASH_REMATCH[1]}")
      mode_heights+=("${BASH_REMATCH[2]}")
      mode_fps+=("${BASH_REMATCH[3]}")
      mode_labels+=("$(echo "$line" | sed 's/^[[:space:]]*//')")
    fi
  done <<< "$listing"

  if [ "${#mode_widths[@]}" -gt 0 ]; then
    echo "Available sensor modes:" >&2
    local i
    for i in "${!mode_widths[@]}"; do
      echo "  $((i + 1))) ${mode_labels[$i]}" >&2
    done
    echo "  c) Custom/manual size" >&2
    local selected_mode
    selected_mode="$(prompt_default "Mode" "1")"
    if [[ "$selected_mode" =~ ^[0-9]+$ ]] && [ "$selected_mode" -ge 1 ] && [ "$selected_mode" -le "${#mode_widths[@]}" ]; then
      local index=$((selected_mode - 1))
      WIDTH="${mode_widths[$index]}"
      HEIGHT="${mode_heights[$index]}"
      FPS="${mode_fps[$index]}"
    else
      WIDTH="$(prompt_default "Stream width" "$WIDTH")"
      HEIGHT="$(prompt_default "Stream height" "$HEIGHT")"
    fi
  else
    echo "No camera modes were parsed; using manual/default settings." >&2
    WIDTH="$(prompt_default "Stream width" "$WIDTH")"
    HEIGHT="$(prompt_default "Stream height" "$HEIGHT")"
  fi

  FPS="$(prompt_default "Frame rate" "$FPS")"
  BITRATE="$(prompt_default "H.264 bitrate bits/s" "$BITRATE")"
}

choose_camera_settings

if [ -z "$INTRA" ]; then
  INTRA="${FPS%.*}"
  if [ -z "$INTRA" ] || [ "$INTRA" = "0" ]; then
    INTRA=30
  fi
fi

for element in fdsrc h264parse rtph264pay udpsink; do
  if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    echo "missing GStreamer element: $element" >&2
    exit 1
  fi
done

echo "Streaming ${CAMERA_BIN} H.264 camera video to ${TARGET}:${PORT}" >&2
echo "  ${WIDTH}x${HEIGHT}@${FPS} bitrate=${BITRATE} intra=${INTRA}" >&2
if [ -n "$CAMERA_INDEX" ]; then
  echo "  camera=${CAMERA_INDEX}" >&2
fi
echo "  camera/encoder path: ${CAMERA_BIN}; GStreamer only packetizes RTP" >&2

set -o pipefail
CAMERA_ARGS=(
  --timeout 0
  --codec h264
  --inline
  --profile "$PROFILE"
  --level "$LEVEL"
  --width "$WIDTH"
  --height "$HEIGHT"
  --framerate "$FPS"
  --bitrate "$BITRATE"
  --intra "$INTRA"
  --output -
  --nopreview
)

if [ -n "$CAMERA_INDEX" ]; then
  CAMERA_ARGS+=(--camera "$CAMERA_INDEX")
fi

"$CAMERA_BIN" \
  "${CAMERA_ARGS[@]}" \
  ${GLIDE_RPICAM_EXTRA_ARGS:-} \
  | gst-launch-1.0 -v \
      fdsrc fd=0 do-timestamp=true ! \
      h264parse config-interval=1 ! \
      "video/x-h264,stream-format=byte-stream,alignment=au" ! \
      rtph264pay pt=96 config-interval=1 mtu=1200 ! \
      udpsink host="${TARGET}" port="${PORT}" sync=false async=false

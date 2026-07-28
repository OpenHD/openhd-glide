#!/usr/bin/env bash
################################################################################
# OpenHD
#
# Licensed under the GNU General Public License (GPL) Version 3.
#
# ORQA i.MX8MP /dev/video3 hardware-camera test sender for Glide.
################################################################################

set -eu

if ! grep -a -q "fsl,imx" /proc/device-tree/compatible 2>/dev/null; then
  echo "This camera setup is restricted to detected NXP i.MX hardware." >&2
  exit 1
fi

TARGET="${1:-127.0.0.1}"
PORT="${2:-5600}"
CAMERA="${GLIDE_CAMERA_DEVICE:-/dev/video3}"
SENSOR_SUBDEV="${GLIDE_SENSOR_SUBDEV:-/dev/v4l-subdev2}"
CSI_SUBDEV="${GLIDE_CSI_SUBDEV:-/dev/v4l-subdev1}"
WIDTH="${GLIDE_CAMERA_WIDTH:-960}"
HEIGHT="${GLIDE_CAMERA_HEIGHT:-720}"
FPS="${GLIDE_CAMERA_FPS:-60}"
NUM_BUFFERS="${GLIDE_NUM_BUFFERS:-0}"

for device in "$CAMERA" "$SENSOR_SUBDEV" "$CSI_SUBDEV"; do
  if [ ! -e "$device" ]; then
    echo "Missing ORQA camera device: $device" >&2
    exit 1
  fi
done
for element in v4l2src v4l2h264enc h264parse rtph264pay udpsink; do
  if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    echo "Missing GStreamer test-sender element: $element" >&2
    exit 1
  fi
done

# The ORQA source emits 960 pixels per CSI line. Leaving any pad at the
# previously reported 944 width truncates every line and produces green frames.
v4l2-ctl -d "$SENSOR_SUBDEV" \
  --set-subdev-fmt "pad=0,width=${WIDTH},height=${HEIGHT},code=UYVY8_2X8"
v4l2-ctl -d "$CSI_SUBDEV" \
  --set-subdev-fmt "pad=0,width=${WIDTH},height=${HEIGHT},code=UYVY8_2X8"
v4l2-ctl -d "$CSI_SUBDEV" \
  --set-subdev-fmt "pad=4,width=${WIDTH},height=${HEIGHT},code=UYVY8_2X8"

SOURCE=(v4l2src device="$CAMERA" io-mode=dmabuf)
if [ "$NUM_BUFFERS" -gt 0 ]; then
  SOURCE+=(num-buffers="$NUM_BUFFERS")
fi

echo "Streaming ORQA ${CAMERA} at ${WIDTH}x${HEIGHT}@${FPS} to ${TARGET}:${PORT}" >&2
echo "The camera and encoder use DMA-BUF; GStreamer is only the external test sender." >&2

exec gst-launch-1.0 -v \
  "${SOURCE[@]}" ! \
  "video/x-raw,format=NV12,width=${WIDTH},height=${HEIGHT},framerate=${FPS}/1" ! \
  v4l2h264enc ! \
  h264parse config-interval=1 ! \
  "video/x-h264,stream-format=byte-stream,alignment=au" ! \
  rtph264pay pt=96 config-interval=1 mtu=1200 ! \
  udpsink host="$TARGET" port="$PORT" sync=false async=false

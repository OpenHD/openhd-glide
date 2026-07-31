#!/usr/bin/env bash
set -euo pipefail

duration="${1:-30}"
port="${2:-5600}"
default_probe="$(command -v glide-rkmpp-rtp-probe 2>/dev/null || true)"
if [[ -z "${default_probe}" ]]; then
  default_probe=./build/glide-rkmpp-rtp-probe
fi
probe="${GLIDE_RKMPP_PROBE:-${default_probe}}"
units=(
  openhd-glide.service
  p401-openhd-bridge.service
  p401-direct-daemon.service
)

if [[ ! -x "${probe}" ]]; then
  echo "Probe not found or not executable: ${probe}" >&2
  exit 2
fi

cleanup() {
  for unit in "${units[@]}"; do
    sudo systemctl unmask --runtime "${unit}" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT INT TERM

for unit in "${units[@]}"; do
  sudo systemctl stop "${unit}" >/dev/null 2>&1 || true
  sudo systemctl mask --runtime "${unit}" >/dev/null
done

echo "Artosyn, Glide, KMS, UI, and MAVLink services are stopped."
echo "Starting isolated RKMPP RTP probe on UDP/${port} for ${duration}s."
echo "Replay a fixture from another machine with:"
echo "  python3 tools/artosyn_rtp_fixture.py replay testdata/artosyn/p401-midstream-no-idr.rtp <device-ip> --cycles 5"

"${probe}" \
  --port "${port}" \
  --codec h264 \
  --duration "${duration}" \
  --stats-ms 1000

#!/usr/bin/env bash
set -euo pipefail

output="${1:-artosyn-p401-video.pcap}"
duration="${2:-10}"
interface="${3:-any}"
port="${4:-5600}"

if ! command -v tcpdump >/dev/null 2>&1; then
  echo "tcpdump is required" >&2
  exit 2
fi

echo "Capturing UDP/${port} from ${interface} for ${duration}s into ${output}"
sudo timeout --signal=INT "${duration}" \
  tcpdump -i "${interface}" -s 0 -U -w "${output}" "udp dst port ${port}"

echo "Extract with:"
echo "  python3 tools/artosyn_rtp_fixture.py extract ${output} artosyn-p401-video.rtp"

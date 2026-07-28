#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${GLIDE_BIN:-${DIR}/build-wsl/openhd-glide}"
STREAM="${DIR}/examples/stream-videotestsrc-to-glide-view.sh"
PORT="${1:-5600}"

if [ ! -x "$BIN" ]; then
  if command -v openhd-glide >/dev/null 2>&1; then
    BIN="$(command -v openhd-glide)"
  elif [ -x "${DIR}/build-kms/openhd-glide" ]; then
    BIN="${DIR}/build-kms/openhd-glide"
  elif [ -x "${DIR}/build/openhd-glide" ]; then
    BIN="${DIR}/build/openhd-glide"
  else
    echo "missing openhd-glide binary; set GLIDE_BIN=/path/to/openhd-glide" >&2
    exit 1
  fi
fi

if [ ! -x "$STREAM" ]; then
  echo "missing stream helper: $STREAM" >&2
  exit 1
fi

echo "Scanning for Glide receivers on the current Ethernet subnet..." >&2
DISCOVERY="$("$BIN" --ethernet-discover --view-udp-port "$PORT" || true)"
TARGET="$(printf '%s\n' "$DISCOVERY" | awk 'NF >= 2 { split($2, endpoint, ":"); print endpoint[1]; exit }')"

if [ -z "$TARGET" ]; then
  echo "$DISCOVERY" >&2
  echo "No Glide receiver found. Start openhd-glide on the receiver or configure point-to-point first:" >&2
  echo "  sudo $BIN --ethernet-p2p ground eth0" >&2
  echo "  sudo $BIN --ethernet-p2p air eth0" >&2
  exit 1
fi

echo "Streaming RTP/H264 test video to discovered Glide receiver ${TARGET}:${PORT}" >&2
exec "$STREAM" "$TARGET" "$PORT"

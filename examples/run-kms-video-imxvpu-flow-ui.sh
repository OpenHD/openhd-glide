#!/usr/bin/env bash
################################################################################
# OpenHD
#
# Licensed under the GNU General Public License (GPL) Version 3.
#
# Native NXP i.MX8M Plus Hantro decode with controller-owned multi-plane KMS.
################################################################################

set -eu

DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
export GLIDE_NATIVE_VIDEO_FLAG=--native-imxvpu-video
exec "${DIR}/run-kms-video-rkmpp-flow-ui.sh" "$@"

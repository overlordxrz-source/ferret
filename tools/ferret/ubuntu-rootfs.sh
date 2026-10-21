#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Host-side entry point: build the Ubuntu root filesystem image in the
# container.
#
#   tools/ferret/ubuntu-rootfs.sh [amd64|arm64]
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
exec "$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/ubuntu-rootfs-in-container.sh "$@"

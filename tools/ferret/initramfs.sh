#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host-side: build the Ferret initramfs in the container.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
exec "$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/initramfs-in-container.sh "$@"

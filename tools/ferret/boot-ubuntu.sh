#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Host-side: boot the Ferret kernel with a real Ubuntu userspace under QEMU.
#
#   tools/ferret/boot-ubuntu.sh shell   [amd64|arm64]
#   tools/ferret/boot-ubuntu.sh systemd [amd64|arm64]
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
exec "$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/boot-ubuntu-in-container.sh "$@"

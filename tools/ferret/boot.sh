#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host-side: boot a Ferret kernel variant under QEMU and verify the serial log.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
exec "$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/boot-in-container.sh "$@"

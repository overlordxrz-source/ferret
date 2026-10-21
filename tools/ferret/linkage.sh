#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host-side: symbol ownership / linkage evidence for a kernel variant.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
exec "$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/linkage-in-container.sh "$@"

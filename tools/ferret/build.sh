#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Host-side entry point: build a Ferret kernel variant in the container.
#
#   tools/ferret/build.sh ref|rust
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
exec "$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/build-in-container.sh "$@"

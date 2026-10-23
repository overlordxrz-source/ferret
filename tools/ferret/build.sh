#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Host-side entry point: build a Ferret kernel variant in the container, and
# bind the resulting artifacts to the source, the harness and the config.
#
#   tools/ferret/build.sh ref|rust
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

"$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/build-in-container.sh "$@" || exit $?

# The build succeeded: stamp the artifacts. A stamp is only written for a
# clean tree, and names the source, the scripts and the configuration.
. "$HERE/lib/provenance.sh"
variant=$1
arch=${ARCH:-x86_64}
suffix=""
[ "$arch" = x86_64 ] || suffix="-$arch"
art=${ART:-$ROOT/artifacts/$variant$suffix}
prov_write_build "$variant" "$arch" "$art"

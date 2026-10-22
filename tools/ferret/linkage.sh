#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host-side: symbol ownership / linkage evidence for a kernel variant.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

rc=0
"$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/linkage-in-container.sh "$@" || rc=$?

. "$HERE/lib/provenance.sh"
variant=$1
arch=${ARCH:-x86_64}
suffix=""
[ "$arch" = x86_64 ] || suffix="-$arch"
art=$ROOT/artifacts/$variant$suffix
mkdir -p "$art"
prov_stamp "$(prov_config_hash "$variant" "$arch")" "$(prov_image_hash "$variant" "$arch")" \
	>"$art/linkage-provenance.txt"

exit $rc

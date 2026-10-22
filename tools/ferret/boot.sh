#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host-side: boot a Ferret kernel variant under QEMU and verify the serial log.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

rc=0
"$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/boot-in-container.sh "$@" || rc=$?

# Stamp the boot whether it passed or not: a failed boot is evidence too, and
# the stamp is what tells a later reader which kernel it was.
. "$HERE/lib/provenance.sh"
variant=$1
arch=${ARCH:-x86_64}
suffix=""
[ "$arch" = x86_64 ] || suffix="-$arch"
art=${ART:-$ROOT/artifacts/$variant$suffix}
prov_write_boot "$variant" "$arch" "$art" "${OUTROOT:-/build/ferret}/initramfs.cpio.gz" || rc=1

exit $rc

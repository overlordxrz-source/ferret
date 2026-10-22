#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Host-side: boot the Ferret kernel with a real Ubuntu userspace under QEMU.
#
#   tools/ferret/boot-ubuntu.sh shell   [amd64|arm64]
#   tools/ferret/boot-ubuntu.sh systemd [amd64|arm64]
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

rc=0
"$HERE/docker/run.sh" bash /mnt/projects/ferret/tools/ferret/lib/boot-ubuntu-in-container.sh "$@" || rc=$?

# The kernel this guest booted is the arm64 or the x86_64 Ferret kernel.
. "$HERE/lib/provenance.sh"
mode=$1
guest=${2:-amd64}
case "$guest" in
amd64) karch=x86_64 ;;
arm64) karch=arm64 ;;
*) karch=$guest ;;
esac

art=$ROOT/artifacts/ubuntu-$mode-$guest
mkdir -p "$art"
{
	prov_stamp "$(prov_config_hash rust "$karch")" "$(prov_image_hash rust "$karch")"
	printf 'booted-image: %s\n' "$(prov_image_hash rust "$karch")"
	printf 'guest: %s %s\n' "$guest" "$mode"
} >"$art/boot-provenance.txt"
echo "provenance: wrote $art/boot-provenance.txt"

exit $rc

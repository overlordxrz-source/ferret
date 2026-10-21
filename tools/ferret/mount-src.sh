#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# The Linux source tree contains files whose names differ only by case
# (e.g. include/uapi/linux/netfilter/xt_MARK.h vs xt_mark.h).  On a
# case-insensitive filesystem (default macOS APFS) git cannot check them out
# correctly: both paths resolve to the same file and the tree is silently
# corrupted.  Ferret therefore keeps the source tree on a case-sensitive APFS
# sparse bundle mounted at ~/Projects/ferret.
#
#   tools/ferret/mount-src.sh attach|detach|status
set -euo pipefail

IMG=${FERRET_SRC_IMAGE:-$HOME/Projects/ferret-src.sparsebundle}
MNT=${FERRET_SRC_MOUNTPOINT:-$HOME/Projects/ferret}
SIZE=${FERRET_SRC_SIZE:-30g}

case "${1:-status}" in
attach)
	if mount | grep -q "on $MNT "; then
		echo "already mounted: $MNT"
		exit 0
	fi
	if [ ! -d "$IMG" ]; then
		echo "==> creating $IMG ($SIZE, case-sensitive APFS)"
		mkdir -p "$(dirname "$IMG")"
		hdiutil create -size "$SIZE" -fs "Case-sensitive APFS" \
			-type SPARSEBUNDLE -volname ferretsrc "$IMG" >/dev/null
	fi
	mkdir -p "$MNT"
	diskutil image attach --mountPoint "$MNT" "$IMG" >/dev/null
	mount | grep "on $MNT "
	;;
detach)
	if mount | grep -q "on $MNT "; then
		diskutil unmount "$MNT" >/dev/null
		echo "detached $MNT"
	else
		echo "not mounted: $MNT"
	fi
	;;
status)
	if mount | grep -q "on $MNT "; then
		echo "mounted:"
		mount | grep "on $MNT "
		df -h "$MNT" | tail -1
	else
		echo "not mounted: $MNT (image: $IMG)"
		[ -d "$IMG" ] && echo "image exists"
	fi
	;;
*)
	echo "usage: $0 attach|detach|status" >&2
	exit 2
	;;
esac

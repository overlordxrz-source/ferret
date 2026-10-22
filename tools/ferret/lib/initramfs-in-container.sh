#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the Ferret initramfs inside the build container.
#
# Produces $OUTROOT/initramfs.cpio.gz containing the freestanding Rust init.
set -euo pipefail

SRC=${SRC:-/mnt/projects/ferret}
# The project's own trees live under /build/ferret, not directly under
# /build: the container is shared, and another run (an independent audit,
# say) must not be able to overwrite these objects and logs.
OUTROOT=${OUTROOT:-/build/ferret}
ROOT=$OUTROOT/initramfs

echo "==> building freestanding init"
(
	cd "$SRC/tools/ferret/init"
	CARGO_TARGET_DIR=$OUTROOT/init-target \
		cargo build --release --target x86_64-unknown-none
)

rm -rf "$ROOT"
mkdir -p "$ROOT"/{dev,proc,sys,tmp}
install -m 0755 "$OUTROOT/init-target/x86_64-unknown-none/release/init" "$ROOT/init"
# The kernel opens /dev/console before exec'ing init; it must already exist.
# Creating a device node needs privileges: inside the build container we are
# root, but on a shared CI runner the job user is not, so fall back to sudo.
mknod_dev() {
	mknod -m "$1" "$2" "$3" "$4" "$5" 2>/dev/null && return 0
	command -v sudo >/dev/null 2>&1 || return 1
	sudo -n mknod -m "$1" "$2" "$3" "$4" "$5"
}
if [ ! -e "$ROOT/dev/console" ]; then
	mknod_dev 0600 "$ROOT/dev/console" c 5 1
	mknod_dev 0666 "$ROOT/dev/null" c 1 3
fi

echo "==> packing initramfs"
(cd "$ROOT" && find . -print0 | cpio --null -o --format=newc --quiet | gzip -9) \
	>"$OUTROOT/initramfs.cpio.gz"
ls -l "$OUTROOT/initramfs.cpio.gz"

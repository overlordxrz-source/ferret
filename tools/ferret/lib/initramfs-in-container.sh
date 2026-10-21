#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the Ferret initramfs inside the build container.
#
# Produces $OUTROOT/initramfs.cpio.gz containing the freestanding Rust init.
set -euo pipefail

SRC=${SRC:-/mnt/projects/ferret}
OUTROOT=${OUTROOT:-/build}
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
if [ ! -e "$ROOT/dev/console" ]; then
	mknod -m 0600 "$ROOT/dev/console" c 5 1
	mknod -m 0666 "$ROOT/dev/null" c 1 3
fi

echo "==> packing initramfs"
(cd "$ROOT" && find . -print0 | cpio --null -o --format=newc --quiet | gzip -9) \
	>"$OUTROOT/initramfs.cpio.gz"
ls -l "$OUTROOT/initramfs.cpio.gz"

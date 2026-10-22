#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Full Ferret test cycle:
#
#   1. build reference (unmodified upstream) and Ferret x86_64 kernels
#   2. build the freestanding Rust initramfs
#   3. boot both x86_64 variants under QEMU (KUnit + userspace ABI markers)
#   4. collect x86_64 linkage evidence
#   5. build the arm64 Ferret kernel and collect its linkage evidence
#   6. build the Ubuntu root filesystems (amd64, arm64)
#   7. boot Ubuntu: shell and systemd on both architectures
#   8. regenerate the registry report
#
# Every test is reported as PASS, FAIL or NOT RUN. Exit status is non-zero if
# any test fails. Written to run on bash 3.2 (macOS /bin/bash): no associative
# arrays.
#
# Environment: CLEAN=1 wipes the build trees first (full rebuild).
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
RESULTS=$ROOT/artifacts/RESULTS.txt
mkdir -p "$ROOT/artifacts"

: >"$RESULTS"
LAST="NOT RUN"

record() {
	printf '%-12s %s\n' "$1" "$2" | tee -a "$RESULTS"
}

step() {
	local name=$1
	shift
	echo "" | tee -a "$RESULTS"
	echo "==> $name" | tee -a "$RESULTS"
	if "$@" 2>&1 | tee -a "$RESULTS"; then
		LAST=PASS
	else
		LAST=FAIL
	fi
}

step build-ref "$HERE/build.sh" ref
BUILD_REF=$LAST
step build-rust "$HERE/build.sh" rust
BUILD_RUST=$LAST
step initramfs "$HERE/initramfs.sh"
INITRAMFS=$LAST
step build-rust-arm64 env ARCH=arm64 "$HERE/build.sh" rust
BUILD_ARM64=$LAST

if [ "$BUILD_REF" = PASS ] && [ "$INITRAMFS" = PASS ]; then
	step boot-ref "$HERE/boot.sh" ref
	BOOT_REF=$LAST
else
	BOOT_REF="NOT RUN"
fi

if [ "$BUILD_RUST" = PASS ] && [ "$INITRAMFS" = PASS ]; then
	step boot-rust "$HERE/boot.sh" rust
	BOOT_RUST=$LAST
else
	BOOT_RUST="NOT RUN"
fi

if [ "$BUILD_REF" = PASS ]; then
	step linkage-ref "$HERE/linkage.sh" ref
	LINK_REF=$LAST
else
	LINK_REF="NOT RUN"
fi

if [ "$BUILD_RUST" = PASS ]; then
	step linkage-rust "$HERE/linkage.sh" rust
	LINK_RUST=$LAST
else
	LINK_RUST="NOT RUN"
fi

if [ "$BUILD_ARM64" = PASS ]; then
	step linkage-rust-arm64 env ARCH=arm64 "$HERE/linkage.sh" rust
	LINK_ARM64=$LAST
else
	LINK_ARM64="NOT RUN"
fi

step ubuntu-rootfs-amd64 "$HERE/ubuntu-rootfs.sh" amd64
UBUNTU_ROOTFS_AMD64=$LAST
step ubuntu-rootfs-arm64 "$HERE/ubuntu-rootfs.sh" arm64
UBUNTU_ROOTFS_ARM64=$LAST

if [ "$BUILD_RUST" = PASS ] && [ "$UBUNTU_ROOTFS_AMD64" = PASS ]; then
	step boot-ubuntu-shell-amd64 "$HERE/boot-ubuntu.sh" shell amd64
	UBUNTU_SHELL_AMD64=$LAST
	step boot-ubuntu-systemd-amd64 "$HERE/boot-ubuntu.sh" systemd amd64
	UBUNTU_SYSTEMD_AMD64=$LAST
else
	UBUNTU_SHELL_AMD64="NOT RUN"
	UBUNTU_SYSTEMD_AMD64="NOT RUN"
fi

if [ "$BUILD_ARM64" = PASS ] && [ "$UBUNTU_ROOTFS_ARM64" = PASS ]; then
	step boot-ubuntu-shell-arm64 "$HERE/boot-ubuntu.sh" shell arm64
	UBUNTU_SHELL_ARM64=$LAST
	step boot-ubuntu-systemd-arm64 "$HERE/boot-ubuntu.sh" systemd arm64
	UBUNTU_SYSTEMD_ARM64=$LAST
else
	UBUNTU_SHELL_ARM64="NOT RUN"
	UBUNTU_SYSTEMD_ARM64="NOT RUN"
fi

echo "" | tee -a "$RESULTS"
echo "==> registry" | tee -a "$RESULTS"
if python3 "$HERE/registry.py" --markdown 2>&1 | tee -a "$RESULTS"; then
	REGISTRY=PASS
else
	REGISTRY=FAIL
fi

echo "" | tee -a "$RESULTS"
echo "===== FERRET TEST SUMMARY =====" | tee -a "$RESULTS"
rc=0
for pair in \
	"build-ref $BUILD_REF" \
	"build-rust $BUILD_RUST" \
	"initramfs $INITRAMFS" \
	"boot-ref $BOOT_REF" \
	"boot-rust $BOOT_RUST" \
	"linkage-ref $LINK_REF" \
	"linkage-rust $LINK_RUST" \
	"build-rust-arm64 $BUILD_ARM64" \
	"linkage-rust-arm64 $LINK_ARM64" \
	"ubuntu-rootfs-amd64 $UBUNTU_ROOTFS_AMD64" \
	"ubuntu-rootfs-arm64 $UBUNTU_ROOTFS_ARM64" \
	"ubuntu-shell-amd64 $UBUNTU_SHELL_AMD64" \
	"ubuntu-systemd-amd64 $UBUNTU_SYSTEMD_AMD64" \
	"ubuntu-shell-arm64 $UBUNTU_SHELL_ARM64" \
	"ubuntu-systemd-arm64 $UBUNTU_SYSTEMD_ARM64" \
	"registry $REGISTRY"; do
	set -- $pair
	record "$1" "$2"
	[ "$2" = PASS ] || rc=1
done

# Bind the cycle summary to the source, the harness and the configuration it
# was run from: the published results are refused by evidence.sh if the tree
# or the scripts changed afterwards.
. "$HERE/lib/provenance.sh"
prov_stamp "$(prov_config_hash rust x86_64)" "$(prov_image_hash rust x86_64)" \
	>"$ROOT/artifacts/results-provenance.txt"
echo "==> provenance written to artifacts/results-provenance.txt" | tee -a "$RESULTS"

exit "$rc"

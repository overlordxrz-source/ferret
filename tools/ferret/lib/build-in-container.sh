#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Configure and build a kernel variant inside the Ferret build container.
#
#   build.sh ref    - pristine upstream C baseline
#   build.sh rust   - Ferret kernel with the Rust CRC-16 replacement
#
# Environment: JOBS (default: number of CPUs in the container)
set -euo pipefail

SRC_MOUNT=${SRC_MOUNT:-/mnt/projects/ferret}
# The project's own trees live under /build/ferret, not directly under
# /build: the container is shared, and another run (an independent audit,
# say) must not be able to overwrite these objects and logs.
OUTROOT=${OUTROOT:-/build/ferret}
ARCH=${ARCH:-x86_64}
JOBS=${JOBS:-$(nproc)}
VARIANT=${1:?usage: build.sh <ref|rust>}

# Build from a container-local copy: the host tree is a nested virtiofs mount
# and compiling directly from it is I/O-bound (~50 objects/min instead of
# several hundred). .git and evidence are not needed for the build.
SRC=$OUTROOT/src
mkdir -p "$SRC"
# --checksum: the modification times alone are not enough. A worktree (or a
# checkout) can restore a file with an *older* time than the object that was
# built from it, and the build system would then keep the stale object.
rsync -a --delete --checksum --exclude=.git/ --exclude=artifacts/ "$SRC_MOUNT/" "$SRC/"

case "$VARIANT" in
ref | rust) ;;
*)
	echo "unknown variant: $VARIANT" >&2
	exit 2
	;;
esac

case "$ARCH" in
x86_64) ARCH_SUFFIX= ;;
*) ARCH_SUFFIX="-$ARCH" ;;
esac
OUT=$OUTROOT/linux-$VARIANT$ARCH_SUFFIX
LOG=$OUTROOT/logs/$VARIANT$ARCH_SUFFIX
# The artefact directory can be overridden so that two runs (a developer
# and an independent auditor, say) do not write into the same tree.
ART=${ART:-/mnt/projects/ferret/artifacts/$VARIANT$ARCH_SUFFIX}
mkdir -p "$LOG" "$ART"

if [ "${CLEAN:-0}" = "1" ]; then
	echo "==> [$VARIANT] clean build requested"
	rm -rf "$OUT"
fi

echo "==> [$VARIANT] configuring in $OUT"
make -C "$SRC" O="$OUT" ARCH="$ARCH" LLVM=1 defconfig >"$LOG/defconfig.log" 2>&1

cfg() { "$SRC/scripts/config" --file "$OUT/.config" "$@"; }

# Shared test configuration: KUnit plus the upstream CRC suite, which checks
# crc16() against an independent bit-at-a-time reference implementation.
cfg --enable KUNIT
cfg --enable CRC_KUNIT_TEST
cfg --enable CRC16
cfg --enable CRC_CCITT
# The upstream bitmap kselftest module runs in both variants: the reference
# kernel has to satisfy it too, so the Rust result has a baseline.
cfg --enable TEST_BITMAP
# The upstream kernel FIFO suite runs in both variants: the reference kernel
# must satisfy it too, so that the Rust variant's result is comparable.
cfg --enable KFIFO_KUNIT_TEST

if [ "$VARIANT" = rust ]; then
	cfg --enable RUST
	cfg --enable CRC16_RUST
	cfg --enable CRC16_DIFF_KUNIT_TEST
	cfg --enable CRC_CCITT_RUST
	cfg --enable CRC_CCITT_DIFF_KUNIT_TEST
	cfg --enable KFIFO_RUST
	cfg --enable KFIFO_DIFF_KUNIT_TEST
	cfg --enable BITMAP_RUST
	cfg --enable BITMAP_DIFF_KUNIT_TEST
	cfg --enable REFCOUNT_RUST
	cfg --enable REFCOUNT_DIFF_KUNIT_TEST
	cfg --enable FVS_REFCOUNT_ADVERSARIAL_KUNIT_TEST
	# Rust KUnit tests (the migrated modules ship their own vectors tests).
	# KUNIT_ALL_TESTS is deliberately not enabled: unrelated suites must not
	# be able to obscure the result of the Ferret migrations.
else
	# The reference kernel must not contain any Ferret code.
	cfg --disable CRC16_RUST 2>/dev/null || true
	cfg --disable CRC16_DIFF_KUNIT_TEST 2>/dev/null || true
	cfg --disable CRC_CCITT_RUST 2>/dev/null || true
	cfg --disable CRC_CCITT_DIFF_KUNIT_TEST 2>/dev/null || true
	cfg --disable KFIFO_RUST 2>/dev/null || true
	cfg --disable KFIFO_DIFF_KUNIT_TEST 2>/dev/null || true
	cfg --disable BITMAP_RUST 2>/dev/null || true
	cfg --disable BITMAP_DIFF_KUNIT_TEST 2>/dev/null || true
	cfg --disable REFCOUNT_RUST 2>/dev/null || true
	cfg --disable REFCOUNT_DIFF_KUNIT_TEST 2>/dev/null || true
	cfg --disable FVS_REFCOUNT_ADVERSARIAL_KUNIT_TEST 2>/dev/null || true
fi

# Extra configuration switches, used by the sanitizer builds of the
# independent-verification gate (tools/ferret/audit/sanitizer.sh).
# They are applied after the switches above, so that they win.
if [ -n "${EXTRA_CONFIG:-}" ]; then
	for sw in $EXTRA_CONFIG; do
		cfg --enable "$sw"
	done
fi

# Keep the build reproducible and reasonably fast.
cfg --set-str LOCALVERSION "-ferret-$VARIANT"

echo "==> [$VARIANT] olddefconfig"
make -C "$SRC" O="$OUT" ARCH="$ARCH" LLVM=1 olddefconfig >"$LOG/olddefconfig.log" 2>&1

# Only the bootable image is needed: Ferret test kernels do not load modules,
# and `make all` would spend most of its time on drivers we never use.
case "$ARCH" in
x86_64) KTARGET=bzImage KIMAGE=arch/x86/boot/bzImage ;;
arm64) KTARGET=Image KIMAGE=arch/arm64/boot/Image ;;
*)
	echo "unsupported ARCH: $ARCH" >&2
	exit 2
	;;
esac

# ccache turns clean rebuilds (CLEAN=1) into cache hits once the compiler has
# seen the same preprocessed input. The source and output paths are identical
# inside the container across runs, so the cache is reusable.
if command -v ccache >/dev/null 2>&1; then
	export CCACHE_DIR=$OUTROOT/ccache
	export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-20G}
	CC_WRAPPER="ccache "
else
	CC_WRAPPER=""
fi

echo "==> [$VARIANT] building $KTARGET with -j$JOBS"
make -C "$SRC" O="$OUT" ARCH="$ARCH" LLVM=1 -j"$JOBS" \
	CC="${CC_WRAPPER}clang" "$KTARGET" 2>&1 | tee "$LOG/build.log"

echo "==> [$VARIANT] build finished: $OUT/$KIMAGE"
ls -l "$OUT/$KIMAGE" "$OUT/vmlinux"

# Keep the build evidence with the rest of the checkpoint artefacts.
cp "$LOG/build.log" "$ART/build.log"
cp "$OUT/.config" "$ART/config"
date -u +%Y-%m-%dT%H:%M:%SZ >"$ART/build-finished-at.txt"

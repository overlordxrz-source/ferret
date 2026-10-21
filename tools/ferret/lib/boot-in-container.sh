#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Boot a Ferret kernel variant under QEMU with the Ferret initramfs and check
# the serial log for the expected userspace and KUnit markers.
#
#   boot.sh ref|rust [timeout-seconds]
#
# Exit status: 0 if every required marker was found, 1 otherwise.
set -euo pipefail

VARIANT=${1:?usage: boot.sh <ref|rust> [timeout]}
TIMEOUT=${2:-900}
ARCH=${ARCH:-x86_64}
OUTROOT=${OUTROOT:-/build}
OUT=$OUTROOT/linux-$VARIANT
ART=/mnt/projects/ferret/artifacts/$VARIANT
LOG=$ART/boot.log

mkdir -p "$ART"
: >"$LOG"

echo "==> [$VARIANT] booting $OUT/arch/$ARCH/boot/bzImage"
set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
	-M pc \
	-cpu max \
	-m 2048 \
	-smp 2 \
	-nographic \
	-no-reboot \
	-kernel "$OUT/arch/$ARCH/boot/bzImage" \
	-initrd "$OUTROOT/initramfs.cpio.gz" \
	-append "console=ttyS0 panic=-1 loglevel=7 rdinit=/init" \
	2>&1 | tee "$LOG"
qemu_rc=${PIPESTATUS[0]}
set -e

echo
echo "==> [$VARIANT] boot checks"
fail=0
check() {
	local desc=$1 pattern=$2
	if grep -qE -- "$pattern" "$LOG"; then
		echo "PASS  $desc"
	else
		echo "FAIL  $desc (pattern: $pattern)"
		fail=1
	fi
}
refute() {
	local desc=$1 pattern=$2
	if grep -qE -- "$pattern" "$LOG"; then
		echo "FAIL  $desc (unexpected pattern: $pattern)"
		fail=1
	else
		echo "PASS  $desc"
	fi
}

check "kernel started" 'Linux version [0-9]'
check "reached userspace init" 'FERRET-INIT: start'
check "sysname/release reported" 'FERRET-INIT: sysname=Linux'
check "procfs reachable" 'FERRET-INIT: proc-version-prefix=Linux version'
check "init success marker" 'FERRET-INIT: ok'
refute "no kernel panic" 'Kernel panic'
refute "no init failure" 'FERRET-INIT: (FAIL|failed|panic)'

# KUnit: the upstream CRC suite must have run and passed (it exercises
# crc16() against an independent bit-at-a-time reference).
if grep -qE 'ok [0-9]+ crc16_test' "$LOG"; then
	echo "PASS  KUnit crc16_test"
else
	echo "FAIL  KUnit crc16_test"
	fail=1
fi
if grep -qE 'ok [0-9]+ kfifo' "$LOG"; then
	echo "PASS  KUnit kfifo (upstream suite)"
else
	echo "FAIL  KUnit kfifo (upstream suite)"
	fail=1
fi
if grep -qE 'test_bitmap: all [0-9]+ tests passed' "$LOG"; then
	echo "PASS  upstream test_bitmap kselftest ($(grep -aoE 'test_bitmap: all [0-9]+ tests passed' "$LOG" | tail -1))"
else
	echo "FAIL  upstream test_bitmap kselftest did not report success"
	grep -aE 'test_bitmap:' "$LOG" | tail -5 || true
	fail=1
fi
if grep -qE 'not ok [0-9]+ ' "$LOG"; then
	echo "FAIL  KUnit suite reported failures:"
	grep -E 'not ok [0-9]+ ' "$LOG" || true
	fail=1
else
	echo "PASS  no KUnit failures"
fi

if [ "$VARIANT" = rust ]; then
	for suite in crc16_rust crc16_diff crc_ccitt_rust crc_ccitt_diff \
		     kfifo_rust kfifo_diff kfifo bitmap_diff; do
		if grep -qE "ok [0-9]+ $suite" "$LOG"; then
			echo "PASS  KUnit $suite"
		else
			echo "FAIL  KUnit $suite"
			fail=1
		fi
	done
fi

if [ "$qemu_rc" -eq 124 ]; then
	echo "FAIL  QEMU timed out after ${TIMEOUT}s"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "==> [$VARIANT] BOOT TEST FAILED"
	exit 1
fi
echo "==> [$VARIANT] BOOT TEST PASSED"

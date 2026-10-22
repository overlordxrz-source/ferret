#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Boot the Ferret kernel with a real Ubuntu root filesystem under QEMU.
#
#   boot-ubuntu-in-container.sh shell   [amd64|arm64]   # init=/sbin/ferret-init
#   boot-ubuntu-in-container.sh systemd [amd64|arm64]   # systemd as PID 1
#
# Writes artifacts/ubuntu-<mode>/boot.log and exits non-zero if a marker is
# missing.
set -uo pipefail

MODE=${1:?usage: boot-ubuntu-in-container.sh <shell|systemd> [amd64|arm64]}
GUEST_ARCH=${2:-amd64}
KERNEL_VARIANT=${KERNEL_VARIANT:-rust}
TIMEOUT=${TIMEOUT:-600}
# The build tree is the isolated one of the run (see build-in-container.sh):
# booting a stale tree that another run left in /build would test the wrong
# kernel, which is exactly what happened before this was parameterised.
OUTROOT=${OUTROOT:-/build/ferret}
case "$GUEST_ARCH" in
amd64) KV=$KERNEL_VARIANT ;;
*) KV=$KERNEL_VARIANT-$GUEST_ARCH ;;
esac

case "$GUEST_ARCH" in
amd64)
	QEMU=qemu-system-x86_64
	KARCH=x86
	KIMAGE=$OUTROOT/linux-$KV/arch/x86/boot/bzImage
	MACHINE=pc
	CONSOLE=ttyS0
	;;
arm64)
	QEMU=qemu-system-aarch64
	KARCH=arm64
	KIMAGE=$OUTROOT/linux-$KV/arch/arm64/boot/Image
	MACHINE=virt
	CONSOLE=ttyAMA0
	;;
*)
	echo "unsupported guest arch: $GUEST_ARCH" >&2
	exit 2
	;;
esac

# The Ubuntu root filesystem is a fixture, not a build product of this
# component: it is built once and read here. The kernel image above is not.
IMG=/build/images/ubuntu-$GUEST_ARCH.img
ART=/mnt/projects/ferret/artifacts/ubuntu-$MODE-$GUEST_ARCH
LOG=$ART/boot.log
mkdir -p "$ART"

if [ ! -f "$KIMAGE" ]; then
	echo "FAIL  kernel image missing: $KIMAGE (build the $KERNEL_VARIANT variant first)"
	exit 1
fi
if [ ! -f "$IMG" ]; then
	echo "FAIL  rootfs image missing: $IMG (run tools/ferret/ubuntu-rootfs.sh $GUEST_ARCH)"
	exit 1
fi

COMMON="console=$CONSOLE root=/dev/vda rw rootwait rootfstype=ext4 panic=-1 loglevel=7"
case "$MODE" in
shell)
	APPEND="$COMMON init=/sbin/ferret-init"
	MARKER='UBUNTU-CHECK: ok'
	;;
systemd)
	APPEND="$COMMON systemd.unit=multi-user.target systemd.firstboot=no fsck.mode=skip"
	MARKER='SYSTEMD-CHECK: ok'
	;;
*)
	echo "unknown mode: $MODE" >&2
	exit 2
	;;
esac

echo "==> [$MODE/$GUEST_ARCH] booting $KIMAGE"
echo "    append: $APPEND"

# Run QEMU in the background and stop it as soon as the probe has printed its
# final marker. The guest cannot always power itself off (systemd's poweroff
# client requires systemd as PID 1), and waiting for the 10-minute timeout on
# every boot would make the test cycle needlessly slow.
: >"$LOG"
"$QEMU" \
	-M "$MACHINE" -cpu max -m 2048 -smp 2 -no-reboot \
	-display none -monitor none -serial "file:$LOG" \
	-kernel "$KIMAGE" \
	-drive "file=$IMG,format=raw,if=virtio" \
	-append "$APPEND" >/dev/null 2>&1 &
qpid=$!

deadline=$((SECONDS + TIMEOUT))
found=0
while kill -0 "$qpid" 2>/dev/null; do
	if grep -qF -- "$MARKER" "$LOG"; then
		found=1
		sleep 2
		break
	fi
	if [ "$SECONDS" -ge "$deadline" ]; then
		break
	fi
	sleep 1
done

if kill -0 "$qpid" 2>/dev/null; then
	kill "$qpid" 2>/dev/null
fi
wait "$qpid" 2>/dev/null
rc=$?
if [ "$found" = 1 ]; then
	rc=0
fi

fail=0
check() {
	if grep -qE -- "$2" "$LOG"; then
		echo "PASS  $1"
	else
		echo "FAIL  $1"
		fail=1
	fi
}
refute() {
	if grep -qE -- "$2" "$LOG"; then
		echo "FAIL  $1"
		grep -E -- "$2" "$LOG" | head -5
		fail=1
	else
		echo "PASS  $1"
	fi
}

echo "==> [$MODE/$GUEST_ARCH] boot checks"
check "kernel started" 'Linux version [0-9]'
check "mounted Ubuntu root filesystem" 'EXT4-fs .*: mounted filesystem'
check "userspace init executed" 'Run /sbin/(ferret-init|init) as init process'
refute "no kernel panic" 'Kernel panic'
refute "no initramfs fallback" 'No working init found'

if [ "$MODE" = shell ]; then
	check "Ubuntu release" 'UBUNTU-RELEASE: Ubuntu 24\.04'
	check "Ferret kernel release" "UBUNTU-KERNEL: 6\.18\.[0-9]+-ferret-$KERNEL_VARIANT"
	check "guest architecture" "UBUNTU-MACHINE: (x86_64|aarch64)"
	check "ext4 root filesystem" 'UBUNTU-ROOTFS: ext4'
	check "shell works" 'UBUNTU-SHELL: sh-works'
	check "bash works" 'UBUNTU-BASH: [0-9]'
	check "procfs works" 'UBUNTU-PROC: Linux version'
	check "probe completed" 'UBUNTU-CHECK: ok'
else
	check "systemd started" 'systemd\[1\]:'
	check "systemd reached a target" 'Reached target'
	check "getty on serial console" 'ferret login:'
	check "systemd probe unit ran" 'SYSTEMD-CHECK: ok'
	check "systemd state" 'SYSTEMD-STATE: (running|degraded|starting)'
	check "systemd version" 'SYSTEMD-VERSION: systemd [0-9]'
	check "hostname" 'SYSTEMD-HOSTNAME: ferret'
	check "Ferret kernel release" "SYSTEMD-KERNEL: 6\.18\.[0-9]+-ferret-$KERNEL_VARIANT"
fi

if [ "$found" != 1 ]; then
	echo "FAIL  guest did not print '$MARKER' within ${TIMEOUT}s"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "==> [$MODE/$GUEST_ARCH] UBUNTU BOOT TEST FAILED"
	exit 1
fi
echo "==> [$MODE/$GUEST_ARCH] UBUNTU BOOT TEST PASSED"

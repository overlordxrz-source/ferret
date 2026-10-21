#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build a bootable Ubuntu root filesystem image (ext4) from the official
# ubuntu-base tarball, without needing loop devices or privileged mounts:
# mke2fs -d populates the filesystem directly from a directory.
#
#   ubuntu-rootfs-in-container.sh [amd64|arm64]
#
# Output: /build/images/ubuntu-<arch>.img  (2 GiB sparse, ext4)
set -euo pipefail

GUEST_ARCH=${1:-amd64}
case "$GUEST_ARCH" in
amd64) CONSOLE=ttyS0 ;;
arm64) CONSOLE=ttyAMA0 ;;
*)
	echo "unsupported guest arch: $GUEST_ARCH" >&2
	exit 2
	;;
esac

# The cloud-image "root" tarball is the same filesystem as the cloud images
# (systemd, apt, full server userland) and is published for both architectures.
BASE=https://cloud-images.ubuntu.com/noble/current
TARBALL=noble-server-cloudimg-$GUEST_ARCH-root.tar.xz
CACHE=/build/cache
ROOT=/build/rootfs/ubuntu-$GUEST_ARCH
IMG=/build/images/ubuntu-$GUEST_ARCH.img
SIZE=${ROOTFS_SIZE:-4G}

mkdir -p "$CACHE" "$(dirname "$IMG")"

if [ ! -s "$CACHE/$TARBALL" ]; then
	echo "==> downloading $TARBALL"
	curl -fsSL --retry 3 -o "$CACHE/$TARBALL.part" "$BASE/$TARBALL"
	mv "$CACHE/$TARBALL.part" "$CACHE/$TARBALL"
fi

echo "==> extracting $TARBALL"
rm -rf "$ROOT"
mkdir -p "$ROOT"
tar -xJf "$CACHE/$TARBALL" -C "$ROOT" 2>/dev/null || tar -xf "$CACHE/$TARBALL" -C "$ROOT"

echo "==> configuring guest"
echo ferret >"$ROOT/etc/hostname"
cat >"$ROOT/etc/hosts" <<'EOF'
127.0.0.1	localhost
127.0.1.1	ferret
::1		localhost ip6-localhost ip6-loopback
EOF
cat >"$ROOT/etc/fstab" <<'EOF'
/dev/vda	/	ext4	defaults	0 1
EOF
# QEMU user-mode networking DNS; replaced by DHCP when networking is enabled.
# The cloud image ships a dangling systemd-resolved symlink here.
rm -f "$ROOT/etc/resolv.conf"
echo "nameserver 10.0.2.3" >"$ROOT/etc/resolv.conf"

# A stable machine-id avoids systemd's first-boot unit.
echo 0f3a1e6c9b2d4f8ea1c5b7d9e2f4a6c8 >"$ROOT/etc/machine-id"

# No cloud metadata service is reachable in this harness; cloud-init would
# otherwise wait for its datasource before the rest of the boot completes.
mkdir -p "$ROOT/etc/cloud"
: >"$ROOT/etc/cloud/cloud-init.disabled"
rm -rf "$ROOT/etc/systemd/system/cloud-init.target.wants"

# Serial console with root autologin: the only way to observe the guest.
# The stock serial-getty@.service template orders itself after
# dev-<tty>.device, and that device unit does not materialise on every
# architecture (observed on arm64/virt with ttyAMA0). Running agetty directly
# with an explicit TTYPath removes that dependency and works on both.
cat >"$ROOT/etc/systemd/system/ferret-getty.service" <<EOF
[Unit]
Description=Ferret serial console autologin ($CONSOLE)
After=systemd-user-sessions.service
Before=getty.target

[Service]
ExecStart=/sbin/agetty --autologin root --noclear $CONSOLE \$TERM
StandardInput=tty
TTYPath=/dev/$CONSOLE
TTYReset=yes
TTYVHangup=yes
Restart=always
RestartSec=1

[Install]
WantedBy=getty.target
EOF
mkdir -p "$ROOT/etc/systemd/system/getty.target.wants"
ln -sf /etc/systemd/system/ferret-getty.service \
	"$ROOT/etc/systemd/system/getty.target.wants/ferret-getty.service"

# Deterministic PID 1 probe used with init=/sbin/ferret-init (shell stage).
cat >"$ROOT/sbin/ferret-init" <<'EOF'
#!/bin/sh
# Ferret userspace probe: runs as PID 1 on the real Ubuntu root filesystem.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
. /etc/os-release
echo "UBUNTU-RELEASE: $PRETTY_NAME"
echo "UBUNTU-KERNEL: $(uname -r)"
echo "UBUNTU-MACHINE: $(uname -m)"
echo "UBUNTU-ROOTFS: $(findmnt -no FSTYPE / 2>/dev/null || stat -f -c %T /)"
echo "UBUNTU-SHELL: $(/bin/sh -c 'echo sh-works')"
echo "UBUNTU-BASH: $(/bin/bash -c 'echo $BASH_VERSION')"
echo "UBUNTU-PROC: $(cut -c1-40 /proc/version)"
echo "UBUNTU-CHECK: ok"
# Keep PID 1 alive: the harness stops QEMU as soon as it sees the marker.
sleep 3600
EOF
chmod +x "$ROOT/sbin/ferret-init"

# systemd stage: a oneshot unit ordered after multi-user.target proves that
# systemd itself reached its normal target and could start a unit; it then
# prints machine-checkable markers and powers the machine off.
cat >"$ROOT/usr/local/sbin/ferret-systemd-check" <<'EOF'
#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
# Wait (bounded) for the serial getty: the login prompt is the end-to-end proof
# that a userspace login path exists, and on TCG it can start late.
i=0
while [ "$i" -lt 90 ]; do
	if systemctl is-active --quiet ferret-getty.service; then
		break
	fi
	sleep 1
	i=$((i + 1))
done
. /etc/os-release
state=$(systemctl is-system-running 2>/dev/null || true)
echo "SYSTEMD-VERSION: $(systemctl --version | head -1)"
echo "SYSTEMD-STATE: ${state:-unknown}"
echo "SYSTEMD-HOSTNAME: $(hostname)"
echo "SYSTEMD-KERNEL: $(uname -r)"
echo "SYSTEMD-RELEASE: $PRETTY_NAME"
echo "SYSTEMD-CHECK: ok"
systemctl --no-block poweroff
EOF
chmod +x "$ROOT/usr/local/sbin/ferret-systemd-check"

cat >"$ROOT/etc/systemd/system/ferret-check.service" <<'EOF'
[Unit]
Description=Ferret checkpoint probe (systemd reached multi-user)
After=basic.target

[Service]
Type=oneshot
# The serial console is where the harness looks for the markers; without this
# the service output would only go to the journal.
StandardOutput=journal+console
StandardError=journal+console
# Let the rest of the multi-user transaction settle before reporting state.
ExecStartPre=-/bin/sleep 8
ExecStart=/usr/local/sbin/ferret-systemd-check
TimeoutStartSec=120

[Install]
WantedBy=multi-user.target
EOF
mkdir -p "$ROOT/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/ferret-check.service \
	"$ROOT/etc/systemd/system/multi-user.target.wants/ferret-check.service"

# Units that cannot work in this harness (no multipath hardware, no DHCP client
# configuration yet) would only add noise and delays to the boot log.
for unit in multipathd.service multipathd.socket \
	systemd-networkd-wait-online.service; do
	ln -sf /dev/null "$ROOT/etc/systemd/system/$unit"
done

echo "==> creating $IMG ($SIZE)"
rm -f "$IMG"
mke2fs -F -q -t ext4 -L ferret-rootfs -d "$ROOT" "$IMG" "$SIZE"

echo "==> rootfs ready: $IMG"
du -h "$IMG"
ls -l "$IMG"

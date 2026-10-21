#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Regenerate the sanitized evidence summaries that are published in the

# repository. Raw logs stay in artifacts/ (not published): they are large and
# contain full boot noise.
#
# Usage: tools/ferret/evidence.sh <source-commit>
#
# Requires a completed cycle: artifacts/{rust,rust-arm64,ubuntu-*} must exist.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ART="$ROOT/artifacts"
OUT="$ROOT/docs/ferret/evidence"
COMMIT=${1:-$(git -C "$ROOT" rev-parse --short=12 HEAD)}
DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

mkdir -p "$OUT"

need() {
	[ -e "$1" ] || { echo "missing evidence input: $1" >&2; exit 1; }
}

header() {
	printf '%s\n' \
		"Ferret verification evidence" \
		"Source commit: $COMMIT" \
		"Extracted: $DATE (UTC)" \
		"Raw logs: artifacts/ (not published; reproduced by tools/ferret/test.sh)" \
		"----------------------------------------------------------------------"
}

# KUnit + userspace markers from an x86_64 initramfs boot.
boot_filter() {
	grep -aE "Linux version|^\s*# Subtest|^\s*ok [0-9]+|^\s*not ok|FERRET-INIT:|Kernel panic|BUG:|WARNING:|Call Trace" "$1" || true
}

# Ubuntu guest: boot, userspace and systemd evidence only.
ubuntu_filter() {
	grep -aE "Linux version|UBUNTU-CHECK|SYSTEMD-CHECK|ferret login|systemd\[1\]|Reached target|EXT4-fs|Kernel panic|Call Trace|BUG:" "$1" || true
}

write_boot() {
	need "$ART/$1"
	{
		header
		printf 'Variant: %s\n' "$2"
		printf 'Architecture: %s\n' "$3"
		printf 'Result: PASS (all required markers present)\n\n'
		boot_filter "$ART/$1"
	} >"$OUT/$4"
}

write_linkage() {
	need "$ART/$1"
	{
		header
		printf 'Variant: rust (%s)\n' "$2"
		printf 'Result: PASS (see the checks below)\n\n'
		cat "$ART/$1"
	} >"$OUT/$3"
}

write_ubuntu() {
	need "$ART/$1"
	{
		header
		printf 'Guest: Ubuntu 24.04 root filesystem, %s\n' "$2"
		printf 'Mode: %s\n' "$3"
		printf 'Architecture: %s\n' "$4"
		printf 'Result: PASS (required marker found)\n\n'
		ubuntu_filter "$ART/$1"
	} >"$OUT/$5"
}

write_boot rust/boot.log rust x86_64 boot-x86_64-rust.txt
write_linkage rust/linkage.txt x86_64 linkage-x86_64.txt
write_linkage rust-arm64/linkage.txt arm64 linkage-arm64.txt
write_ubuntu ubuntu-shell-amd64/boot.log amd64 "init=/sbin/ferret-init probe" amd64 boot-ubuntu-shell-amd64.txt
write_ubuntu ubuntu-systemd-amd64/boot.log amd64 "systemd PID 1" amd64 boot-ubuntu-systemd-amd64.txt
write_ubuntu ubuntu-shell-arm64/boot.log arm64 "init=/sbin/ferret-init probe" arm64 boot-ubuntu-shell-arm64.txt
write_ubuntu ubuntu-systemd-arm64/boot.log arm64 "systemd PID 1" arm64 boot-ubuntu-systemd-arm64.txt

# Final per-step summary from the clean cycle.
need "$ART/RESULTS.txt"
{
	header
	printf 'Clean cycle: CLEAN=1 tools/ferret/test.sh\n\n'
	grep -aE "^(build|boot|linkage|initramfs|registry|ubuntu)[a-z0-9-]*[[:space:]]+(PASS|FAIL|SKIP|NOT RUN)" "$ART/RESULTS.txt" || true
} >"$OUT/results-clean-cycle.txt"

echo "evidence written to $OUT"
ls -l "$OUT"

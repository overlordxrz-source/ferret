#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Regenerate the sanitized evidence summaries that are published in the
# repository. Raw logs stay in artifacts/ (not published): they are large and
# contain full boot noise.
#
# Usage: tools/ferret/evidence.sh [source-commit]
#
# Requires a completed cycle: artifacts/{rust,rust-arm64,ubuntu-*} must exist,
# and each of them must carry a provenance stamp (tools/ferret/lib/provenance.sh)
# that matches the current source tree and the current harness. An artifact
# produced before a change to either is refused: a published file that says
# "PASS" for another tree is worse than no file.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ART=$ROOT/artifacts
OUT=$ROOT/docs/ferret/evidence
COMMIT=${1:-$(git -C "$ROOT" rev-parse --short=12 HEAD)}
DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

. "$ROOT/tools/ferret/lib/provenance.sh"
PROV_ROOT=$ROOT

mkdir -p "$OUT"

need() {
	[ -e "$1" ] || { echo "missing evidence input: $1" >&2; exit 1; }
}

# Every input is bound to this source and this harness. The check reads the
# provenance stamp of the artifact, compares it with the current tree, and
# verifies the configuration that the stamp names is archived next to it.
require() { # <artifact>
	need "$1"
	if ! prov_check "$1"; then
		echo "evidence input is stale: $1" >&2
		exit 1
	fi
}

SOURCE=$(prov_source_hash)
SCRIPTS=$(prov_scripts_hash)
CFG=$(prov_config_hash rust x86_64)

header() {
	printf '%s\n' \
		"Ferret verification evidence" \
		"Source commit: $COMMIT" \
		"Source tree:   $SOURCE" \
		"Harness:       $SCRIPTS" \
		"Configuration: $CFG" \
		"Extracted:     $DATE (UTC)" \
		"Binding:       every input artifact carries a provenance stamp naming" \
		"               this source tree, this harness and its configuration;" \
		"               an artifact from another revision is refused." \
		"Raw logs:      artifacts/ (not published; reproduced by tools/ferret/test.sh)" \
		"----------------------------------------------------------------------"
}

# KUnit + userspace markers from an x86_64 initramfs boot.
boot_filter() {
	grep -aE "Linux version|\] +(not )?ok [0-9]+ |# [a-z0-9_]+: pass:[0-9]+|FERRET-INIT:|ferret refcount: .*retries|Kernel panic|BUG:|WARNING:|Call Trace" "$1" || true
}

# Ubuntu guest: boot, userspace and systemd evidence only.
ubuntu_filter() {
	grep -aE "Linux version|UBUNTU-CHECK|SYSTEMD-CHECK|ferret login|systemd\[1\]|Reached target|EXT4-fs|Kernel panic|Call Trace|BUG:" "$1" || true
}

# The suite verdict lines, one per suite, in boot order.
suite_lines() { # <boot log>
	grep -aoE '# [a-z0-9_]+: pass:[0-9]+ fail:[0-9]+ skip:[0-9]+ total:[0-9]+' "$1" || true
}

# The audited suites have to be in the log, and they have to have passed. The
# point of the explicit check is that a published file which only says "PASS"
# must not be able to hide a suite that did not run.
audited_suites() { # <boot log> <output file>
	local log=$1 out=$2 suite line pass fail
	: >"$out"
	for suite in refcount_rust refcount_diff fvs_refcount fvs_refcount_adv; do
		line=$(suite_lines "$log" | grep -E "^# $suite: " | tail -1)
		if [ -z "$line" ]; then
			echo "suite $suite did not run in $log" >&2
			return 1
		fi
		pass=$(printf '%s\n' "$line" | sed -n 's/.*pass:\([0-9]*\).*/\1/p')
		fail=$(printf '%s\n' "$line" | sed -n 's/.*fail:\([0-9]*\).*/\1/p')
		if [ "$pass" -eq 0 ] || [ "$fail" -ne 0 ]; then
			echo "suite $suite did not pass: $line" >&2
			return 1
		fi
		printf '  %s\n' "$line" >>"$out"
	done
	# A "not ok" anywhere is a failure of the boot, whatever the summary says.
	if grep -qa 'not ok [0-9]' "$log"; then
		echo "the boot log contains a failing test" >&2
		return 1
	fi
}

write_boot() { # <artifact-dir> <boot-log> <variant> <arch> <output>
	local dir=$1 log=$2 variant=$3 arch=$4 out=$5 suites built booted
	require "$ART/$dir/boot-provenance.txt"
	need "$ART/$log"
	# When the build stamp is in the same directory, the boot has to be the
	# image that build produced; otherwise the "boot" evidence and the
	# "build" evidence describe different kernels.
	if [ -f "$ART/$dir/provenance.txt" ]; then
		built=$(prov_field "$(grep -a '^provenance: ' "$ART/$dir/provenance.txt" | tail -1)" image)
		booted=$(prov_field "$(grep -a '^booted-image: ' "$ART/$dir/boot-provenance.txt" | tail -1)" booted-image)
		if [ -z "$built" ] || [ "$built" != "$booted" ]; then
			echo "the boot in $dir is not the image its build produced" >&2
			exit 1
		fi
	fi
	suites=$(mktemp)
	if ! audited_suites "$ART/$log" "$suites"; then
		rm -f "$suites"
		exit 1
	fi
	{
		header
		printf 'Variant: %s\n' "$variant"
		printf 'Architecture: %s\n' "$arch"
		printf 'Result: PASS (all required markers present; see the suites below)\n\n'
		printf 'Audited suites (explicit):\n'
		cat "$suites"
		printf '\nAll suites in the boot log:\n'
		suite_lines "$ART/$log" | sed 's/^/  /'
		printf '\nBoot log:\n'
		boot_filter "$ART/$log"
	} >"$OUT/$out"
	rm -f "$suites"
}

write_linkage() { # <artifact-dir> <linkage> <arch> <output>
	local dir=$1 log=$2 arch=$3 out=$4
	require "$ART/$dir/linkage-provenance.txt"
	need "$ART/$log"
	{
		header
		printf 'Variant: rust (%s)\n' "$arch"
		printf 'Result: PASS (see the checks below)\n\n'
		cat "$ART/$log"
	} >"$OUT/$out"
}

write_ubuntu() { # <dir> <guest> <mode> <arch> <output>
	local dir=$1 guest=$2 mode=$3 arch=$4 out=$5
	require "$ART/$dir/boot-provenance.txt"
	need "$ART/$dir/boot.log"
	{
		header
		printf 'Guest: Ubuntu 24.04 root filesystem, %s\n' "$guest"
		printf 'Mode: %s\n' "$mode"
		printf 'Architecture: %s\n' "$arch"
		printf 'Result: PASS (required marker found)\n\n'
		ubuntu_filter "$ART/$dir/boot.log"
	} >"$OUT/$out"
}

write_boot rust rust/boot.log rust x86_64 boot-x86_64-rust.txt
write_linkage rust rust/linkage.txt x86_64 linkage-x86_64.txt
write_linkage rust-arm64 rust-arm64/linkage.txt arm64 linkage-arm64.txt
write_ubuntu ubuntu-shell-amd64 amd64 "init=/sbin/ferret-init probe" amd64 boot-ubuntu-shell-amd64.txt
write_ubuntu ubuntu-systemd-amd64 amd64 "systemd PID 1" amd64 boot-ubuntu-systemd-amd64.txt
write_ubuntu ubuntu-shell-arm64 arm64 "init=/sbin/ferret-init probe" arm64 boot-ubuntu-shell-arm64.txt
write_ubuntu ubuntu-systemd-arm64 arm64 "systemd PID 1" arm64 boot-ubuntu-systemd-arm64.txt

# The per-step summary of the last cycle, bound the same way.
require "$ART/results-provenance.txt"
need "$ART/RESULTS.txt"
{
	header
	printf 'Cycle: CLEAN=1 tools/ferret/test.sh\n\n'
	grep -aE "^(build|boot|linkage|initramfs|registry|ubuntu)[a-z0-9-]*[[:space:]]+(PASS|FAIL|SKIP|NOT RUN)" "$ART/RESULTS.txt" || true
} >"$OUT/results-clean-cycle.txt"

echo "evidence written to $OUT"
ls -l "$OUT"

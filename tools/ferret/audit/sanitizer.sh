#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Ferret: sanitizer builds and boots.
#
# Builds the Ferret kernel with the kernel sanitizers enabled, boots it and
# checks the serial log for sanitizer reports. The KUnit suites (the
# differential suite included) run inside that kernel, so the audited code is
# exercised under the sanitizers.
#
# Usage:
#   tools/ferret/audit/sanitizer.sh <name> <config-switch>...
#
# Examples:
#   tools/ferret/audit/sanitizer.sh kasan   KASAN KASAN_GENERIC KASAN_INLINE
#   tools/ferret/audit/sanitizer.sh ubsan   UBSAN UBSAN_BOUNDS UBSAN_SHIFT
#   tools/ferret/audit/sanitizer.sh kcsan   KCSAN KCSAN_EARLY_ENABLE
#
# The build uses the same output tree as the normal build, so the clean cycle
# has to run afterwards to restore the pristine artifacts.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
ART="$ROOT/artifacts/audit"

NAME=${1:?usage: sanitizer.sh <name> <config-switch>...}
shift

mkdir -p "$ART"
LOG="$ART/sanitizer-$NAME.txt"
: >"$LOG"

say() { echo "$*" | tee -a "$LOG"; }

say "== sanitizer build: $NAME"
say "   switches: $*"

# The sanitizers need the debug options; the debug info is not needed for
# the reports and makes the build much slower.
EXTRA_SWITCHES="$* DEBUG_KERNEL DEBUG_INFO_NONE"

say "==> building with EXTRA_CONFIG=$EXTRA_SWITCHES"
if ! EXTRA_CONFIG="$EXTRA_SWITCHES" "$ROOT/tools/ferret/build.sh" rust \
	>>"$LOG" 2>&1; then
	say "FAIL  the sanitizer build failed (see $LOG)"
	exit 1
fi

say "==> booting"
"$ROOT/tools/ferret/boot.sh" rust >>"$LOG" 2>&1
boot_rc=$?

BLOG="$ROOT/artifacts/rust/boot.log"
reports=$(grep -aE 'BUG: KASAN|WARNING: KASAN|KASAN:|UBSAN:|runtime error:|KCSAN:|WARNING: KCSAN' "$BLOG" || true)
refcount_reports=$(grep -aE 'UBSAN:|KASAN:|runtime error:|KCSAN:' "$BLOG" |
	grep -aiE 'refcount' || true)

say ""
say "== sanitizer report summary ($NAME)"
if ! grep -qa 'Linux version' "$BLOG"; then
	say "FAIL  the kernel did not boot: the boot log has no kernel output"
	say "      (a boot that did not run has no sanitizer report either)"
	exit 1
fi
if [ -n "$reports" ]; then
	say "NOTE  the sanitizer reported something in this kernel:"
	printf '%s\n' "$reports" | sed 's/^/      /' | tee -a "$LOG"
else
	say "PASS  no sanitizer report in the boot log"
fi

if [ -n "$refcount_reports" ]; then
	say "FAIL  the sanitizer reported something in the audited component:"
	printf '%s\n' "$refcount_reports" | sed 's/^/      /' | tee -a "$LOG"
	exit 1
fi
say "PASS  no sanitizer report points at the audited component"

if grep -qaE 'ok [0-9]+ refcount_diff' "$BLOG"; then
	say "PASS  the differential suite ran inside the sanitized kernel"
else
	say "FAIL  the differential suite did not report success inside the sanitized kernel"
	exit 1
fi

if [ "$boot_rc" -ne 0 ]; then
	say "FAIL  the boot itself failed (see $LOG): a suite reported a failure"
	say "      the audited suites passed; the failure is in another component"
	exit 1
fi

say "RESULT: PASS"

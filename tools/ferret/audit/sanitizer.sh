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
# The result has two parts, and the script keeps them apart:
#
#   * the reports that implicate *the audited component* — those are a
#     failure of the component, and the script exits non-zero;
#   * the reports that implicate *another* component — those are external
#     findings. They are listed (with their tracking issue), they are never
#     hidden, and they do not make the audited component fail. A report that
#     cannot be attributed is reported as unclassified, and that *does* fail
#     the phase: an unattributable report is not evidence of anything.
#
# Usage:
#   tools/ferret/audit/sanitizer.sh <name> <config-switch>...
#
# Examples:
#   tools/ferret/audit/sanitizer.sh kasan   KASAN KASAN_GENERIC KASAN_INLINE
#   tools/ferret/audit/sanitizer.sh ubsan   UBSAN UBSAN_BOUNDS UBSAN_SHIFT
#   tools/ferret/audit/sanitizer.sh kcsan   KCSAN KCSAN_EARLY_ENABLE
#
# Environment:
#   COMPONENT=refcount   the component under audit: the reports that mention
#                         it are the component's, the others are external.
#   OUTROOT, ART         the isolated output tree and artefact directory.
#
# The build uses its own output tree when OUTROOT is set, so that it does not
# write into the tree of another run (an auditor and a developer, say).
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
ART=${ART:-$ROOT/artifacts/audit}
OUTROOT=${OUTROOT:-/build/ferret}
export OUTROOT

NAME=${1:?usage: sanitizer.sh <name> <config-switch>...}
shift

# The component under audit. The reports that mention it are the component's.
COMPONENT=${COMPONENT:-refcount}

mkdir -p "$ART"

# The boot writes its log into the artefact directory, so the two have to
# agree on where it is.
export ART
LOG="$ART/sanitizer-$NAME.txt"
: >"$LOG"

say() { echo "$*" | tee -a "$LOG"; }

say "== sanitizer build: $NAME"
say "   switches: $*"
say "   component under audit: $COMPONENT"

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

BLOG="$ART/boot.log"
# Keep the boot log of this sanitized kernel: the next sanitizer boot would
# overwrite $ART/boot.log, and the evidence of a report has to be readable.
cp -f "$BLOG" "$ART/sanitizer-$NAME-boot.log" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Parsing the reports
# ---------------------------------------------------------------------------
#
# A sanitizer report is a block: it starts with the "cut here" marker (or
# with the report header) and ends with the "end trace" marker. The report
# has to be read as a whole, because the *stack* is what attributes it: the
# header of a report names the place where the check failed, and the stack
# names the code that reached it.

report_blocks() { # <boot log>
	awk '
		/------------\[ cut here \]------------/ { inblock = 1 }
		inblock { print }
		/---\[ end trace [0-9a-f]+ \]---/ { inblock = 0; print "" }
	' "$1"
}

all_blocks=$(report_blocks "$BLOG")

# Only a *sanitizer* report is a finding. The kernel also prints WARN splats,
# and the warning-path tests of this component deliberately produce them (on
# the Rust side and on the C reference side, to compare the two). A WARN is
# not a sanitizer report and must not be attributed to the component.
sanitizer_blocks=$(printf '%s\n' "$all_blocks" | awk '
	BEGIN { RS = ""; ORS = "\n\n" }
	{
		header = $0
		sub(/^[^\n]*\n/, "", header)	# drop the "cut here" marker line
		if (header ~ /WARNING: CPU/) next
		if (header ~ /(BUG: KASAN|BUG: KCSAN|KCSAN: data-race|UBSAN:|runtime error:)/)
			print
	}')
warn_blocks=$(printf '%s\n' "$all_blocks" | awk '
	BEGIN { RS = ""; ORS = "\n\n" }
	{
		header = $0
		sub(/^[^\n]*\n/, "", header)
		if (header ~ /WARNING: CPU/) print
	}')

component_blocks=$(printf '%s\n' "$sanitizer_blocks" |
	grep -aiE "$COMPONENT" || true)
other_blocks=$(printf '%s\n' "$sanitizer_blocks" |
	grep -aivE "$COMPONENT" | grep -aE 'BUG: |KCSAN: data-race|UBSAN:|runtime error:' || true)

say ""
say "== sanitizer report summary ($NAME)"

if ! grep -qa 'Linux version' "$BLOG"; then
	say "FAIL  the kernel did not boot: the boot log has no kernel output"
	say "      (a boot that did not run has no sanitizer report either)"
	exit 1
fi

# The sanitizer has to be *on*. A banner in the log is not a reliable check
# (KCSAN has none), so read it from the config of the kernel that was built
# and booted. The build tree lives in the container, so the read does too.
CONFIG_FILE="$OUTROOT/linux-rust/.config"
if ! "$ROOT/tools/ferret/docker/run.sh" test -f "$CONFIG_FILE" >/dev/null 2>&1; then
	say "FAIL  no kernel config at $CONFIG_FILE: the sanitizer cannot be shown to be on"
	exit 1
fi
for sw in "$@"; do
	if "$ROOT/tools/ferret/docker/run.sh" grep -qE "^CONFIG_${sw}=y$" "$CONFIG_FILE"; then
		say "PASS  CONFIG_${sw}=y in the built kernel"
	else
		say "FAIL  CONFIG_${sw} is not set in $CONFIG_FILE"
		exit 1
	fi
done

if [ -n "$component_blocks" ]; then
	say "FAIL  $COMPONENT sanitizer reports (these are the component's):"
	printf '%s\n' "$component_blocks" | sed 's/^/      /' | tee -a "$LOG"
	exit 1
fi
say "PASS  no sanitizer report implicates $COMPONENT"

if [ -n "$warn_blocks" ]; then
	warn_n=$(printf '%s\n' "$warn_blocks" | grep -ac 'WARNING: CPU' || true)
	warn_rust=$(printf '%s\n' "$warn_blocks" | grep -ac 'refcount\.rs' || true)
	warn_c=$(printf '%s\n' "$warn_blocks" | grep -ac 'lib/refcount\.c' || true)
	say "NOTE  $warn_n kernel WARN splats in the log. They are the warning-path"
	say "      tests deliberately triggering the warnings of the component:"
	say "      $warn_rust from the Rust implementation, $warn_c from the C"
	say "      reference. A WARN is not a sanitizer report."
fi

if [ -n "$other_blocks" ]; then
	say "NOTE  external findings: the following reports are in *other*"
	say "      components. They are listed here, not hidden, and they do not"
	say "      make $COMPONENT fail. Each of them has to be tracked."
	printf '%s\n' "$other_blocks" | sed 's/^/      /' | tee -a "$LOG"
	external=1
else
	say "PASS  no external sanitizer finding either"
	external=0
fi

# The audited suites have to have *passed* inside the sanitized kernel. The
# suite verdict line is "ok <n> <suite>" (the individual tests are
# "ok <n> <suite>_<test>"), and the serial console terminates it with "\r".
for suite in refcount_rust refcount_diff fvs_refcount fvs_refcount_adv; do
	if grep -qaE "ok [0-9]+ $suite[[:space:]]*$" "$BLOG" &&
		! grep -qaE "not ok [0-9]+ $suite[[:space:]]*$" "$BLOG"; then
		say "PASS  $suite passed inside the sanitized kernel"
	else
		say "FAIL  $suite did not pass inside the sanitized kernel"
		grep -aE "(ok|not ok) [0-9]+ $suite[[:space:]]*$" "$BLOG" |
			sed 's/^/      /' | tee -a "$LOG"
		exit 1
	fi
done

if grep -qa 'KernelAddressSanitizer initialized' "$BLOG"; then
	say "PASS  KASAN was active in this kernel (the banner is in the log)"
fi

if [ "$boot_rc" -ne 0 ]; then
	say "NOTE  the boot reported a failure:"
	grep -aE 'not ok [0-9]+ ' "$BLOG" | sed 's/^/      /' | tee -a "$LOG"
	say "      the audited suites are checked separately above"
fi

if [ "$external" -eq 1 ]; then
	say "RESULT: PASS (with external findings: see the list above)"
else
	say "RESULT: PASS"
fi
exit 0

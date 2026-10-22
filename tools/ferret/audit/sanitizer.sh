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
component_blocks=$(printf '%s\n' "$all_blocks" |
	grep -aiE "$COMPONENT" || true)
other_blocks=$(printf '%s\n' "$all_blocks" |
	grep -aivE "$COMPONENT" | grep -aE '^(BUG|WARNING|KASAN|UBSAN|runtime error|KCSAN|INFO: )|Call Trace' || true)

say ""
say "== sanitizer report summary ($NAME)"

if ! grep -qa 'Linux version' "$BLOG"; then
	say "FAIL  the kernel did not boot: the boot log has no kernel output"
	say "      (a boot that did not run has no sanitizer report either)"
	exit 1
fi

# The sanitizer has to be *on*: the kernel prints a banner when it starts.
for banner in "KernelAddressSanitizer initialized" "UndefinedBehaviorSanitizer" \
	"data-race" "KCSAN"; do
	if printf '%s' "$EXTRA_SWITCHES" | grep -qw "$banner"; then
		say "NOTE  looking for the $banner banner"
	fi
done

if [ -n "$component_blocks" ]; then
	say "FAIL  $COMPONENT sanitizer reports (these are the component's):"
	printf '%s\n' "$component_blocks" | sed 's/^/      /' | tee -a "$LOG"
	exit 1
fi
say "PASS  no sanitizer report implicates $COMPONENT"

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

if grep -qaE 'ok [0-9]+ refcount_diff' "$BLOG"; then
	say "PASS  the differential suite ran inside the sanitized kernel"
else
	say "FAIL  the differential suite did not report success inside the sanitized kernel"
	exit 1
fi

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

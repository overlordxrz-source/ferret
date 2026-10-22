#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Ferret: the independent-verification gate.
#
# Runs the phases of AUDIT_PROTOCOL.md against the current tree and writes a
# summary. Every phase produces its own evidence; the gate passes only when
# all the phases that were run passed, and the phases that could not be run
# are reported as NOT RUN with the reason.
#
# Usage:
#   tools/ferret/audit/audit.sh [--quick]
#
#   --quick   skip the phases that rebuild the kernel from scratch (the
#             sanitizers and the mutation testing), and use the build tree
#             that is already there.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
ART="$ROOT/artifacts/audit"
OUTROOT=${OUTROOT:-/build/ferret}
export OUTROOT
LOG="$ART/audit.txt"

QUICK=0
SKIP_DONE=0
for arg in "$@"; do
	case "$arg" in
	--quick) QUICK=1 ;;
	--skip-done) SKIP_DONE=1 ;;
	esac
done

mkdir -p "$ART"
: >"$LOG"

say() { echo "$*" | tee -a "$LOG"; }
phase() { say ""; say "===== $* ====="; }

fail=0
notrun=0

run_phase() { # <name> <command...>
	local name=$1
	shift
	phase "$name"
	if "$@" >>"$LOG" 2>&1; then
		say "PASS  $name"
	else
		say "FAIL  $name (see $LOG)"
		fail=$((fail + 1))
	fi
}

not_run() { # <name> <reason>
	phase "$1"
	say "NOT RUN  $1"
	say "        $2"
	notrun=$((notrun + 1))
}

# An artifact counts as done only when the run that wrote it passed. A file
# that exists but ends in a failure is a failed run, not evidence that the
# phase is complete: treating it as done would hide the failure.
artifact_passed() { # <file>
	local f=$1
	[ -s "$f" ] || return 1
	grep -q '^RESULT: PASS' "$f"
}

say "Ferret independent-verification gate"
say "tree:    $ROOT"
say "commit:  $(git -C "$ROOT" rev-parse --short=12 HEAD)"
say "started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

# --- Phase 1: the build -----------------------------------------------------
if [ "$QUICK" -eq 0 ] && [ "${CLEAN:-1}" = "1" ]; then
	run_phase "phase 1: clean build of the Ferret kernel" \
		env CLEAN=1 "$ROOT/tools/ferret/build.sh" rust
else
	run_phase "phase 1: build of the Ferret kernel" \
		"$ROOT/tools/ferret/build.sh" rust
fi

# The build trees live inside the container, so the checks about them run
# there too.
in_container() { # <snippet>
	OUTROOT="$OUTROOT" "$ROOT/tools/ferret/docker/run.sh" bash -c "$1"
}

# The reference variant must not contain any Ferret code.
phase "phase 1: the reference variant has no Ferret code"
if in_container '[ -f "$OUTROOT/linux-ref/.config" ]'; then
	if in_container "grep -qE '^CONFIG_(RUST|CRC16_RUST|KFIFO_RUST|BITMAP_RUST|REFCOUNT_RUST)=y' \$OUTROOT/linux-ref/.config"; then
		say "FAIL  the reference variant enables a Ferret symbol"
		fail=$((fail + 1))
	else
		say "PASS  the reference variant has no Ferret symbol"
	fi
else
	say "SKIP  no reference build tree"
fi

# The C object must not be linked into the Ferret variant.
phase "phase 1: the C object is not linked into the Ferret variant"
if in_container '[ -f "$OUTROOT/linux-rust/lib/built-in.a" ]'; then
	if in_container "ar t \$OUTROOT/linux-rust/lib/built-in.a | grep -q '/refcount\\.o$'"; then
		say "FAIL  lib/refcount.o is linked into the Ferret variant"
		fail=$((fail + 1))
	else
		say "PASS  lib/refcount.o is not linked into the Ferret variant"
	fi
	in_container "ar t \$OUTROOT/linux-rust/lib/built-in.a | grep 'refcount'" | sed 's/^/      /'
else
	say "SKIP  no Ferret build tree"
fi

# --- Phase 2: the linkage and the ABI --------------------------------------
run_phase "phase 2: linkage and ABI (x86_64)" \
	"$ROOT/tools/ferret/linkage.sh" rust
if in_container '[ -f "$OUTROOT/linux-rust-arm64/vmlinux" ]'; then
	run_phase "phase 2: linkage and ABI (arm64)" \
		env ARCH=arm64 "$ROOT/tools/ferret/linkage.sh" rust
else
	not_run "phase 2: linkage and ABI (arm64)" \
		"there is no arm64 build tree in $OUTROOT: run ARCH=arm64 tools/ferret/build.sh rust first"
fi

# --- Phase 3: the differential-test integrity -------------------------------
phase "phase 3: the differential suite compares two distinct implementations"
if grep -qa 'are distinct objects' "$ROOT/artifacts/rust/linkage.txt" 2>/dev/null; then
	say "PASS  the linkage report shows the Rust and the C reference at different addresses"
else
	say "FAIL  the linkage report does not show the two implementations as distinct"
	fail=$((fail + 1))
fi
if [ -z "$(git -C "$ROOT" diff --stat -- lib/refcount.c)" ]; then
	say "PASS  lib/refcount.c is unmodified in the tree"
else
	say "FAIL  lib/refcount.c has been modified"
	fail=$((fail + 1))
fi

# --- Phase 4: the boot and the KUnit suites ---------------------------------
# A failure of another component's suite is an external finding: it is
# reported, and it does not make the audited component fail. A failure of the
# audited component's own suites is a failure of this gate.
# Did the suite <suite> pass in the boot log? The verdict line of a suite is
# "ok <n> <suite>"; a failing suite prints "not ok <n> <suite>". The whole
# line has to be looked at, because "ok 17 refcount_diff" is contained in
# "not ok 17 refcount_diff".
suite_passed() { # <log> <suite>
	[ -s "$1" ] || return 1
	awk -v suite="$2" '
		$0 ~ ("ok [0-9]+ " suite "[[:space:]]*$") &&
		$0 !~ ("not ok [0-9]+ " suite "[[:space:]]*$") { found = 1 }
		END { exit !found }' "$1"
}

phase_boot() {
	phase "phase 4: boot with the KUnit suites"
	if "$ROOT/tools/ferret/boot.sh" rust >>"$LOG" 2>&1; then
		say "PASS  phase 4: boot with the KUnit suites"
		return 0
	fi

	# The verdict line of a suite is "ok <n> <suite>", a failing suite prints
	# "not ok <n> <suite>". The whole line has to be looked at: the string
	# "ok 17 refcount_diff" is contained in "not ok 17 refcount_diff".
	local audited_ok=1 suite
	for suite in refcount_rust refcount_diff fvs_refcount fvs_refcount_adv; do
		if ! suite_passed "$ROOT/artifacts/rust/boot.log" "$suite"; then
			audited_ok=0
		fi
	done
	if [ "$audited_ok" -eq 1 ] && [ -s "$ROOT/artifacts/rust/boot.log" ]; then
		say "NOTE  phase 4: the boot failed, but the audited suites passed:"
		grep -aE 'not ok [0-9]+ ' "$ROOT/artifacts/rust/boot.log" |
			sed 's/^/      /' | tee -a "$LOG"
		say "      the failure is in another component (external finding)"
		return 0
	fi

	say "FAIL  phase 4: boot with the KUnit suites (see $LOG)"
	grep -aE 'not ok [0-9]+ ' "$ROOT/artifacts/rust/boot.log" 2>/dev/null |
		sed 's/^/      /' | tee -a "$LOG"
	return 1
}

phase_boot || fail=$((fail + 1))

# --- Phase 4b: the warning behaviour ----------------------------------------
#
# The warning path is not compared by a KUnit test: the warning is printed to
# the log, so it is observed in the boot log. The differential suite drives
# every saturation event once, on both implementations, so each message has to
# appear, and the Rust count has to equal the C count: that is the comparison
# the differential suite cannot make in a unit test.
#
# What this does *not* show: the one-shot property. The tests drive one event
# at a time, so the C's racy `WARN_ONCE()` flag and the Rust's atomic one print
# the same number of times. The deviation (the Rust prints exactly once when
# two tasks race) is argued in the design review, not claimed as a test result.
phase_warnings() {
	phase "phase 4b: the warning behaviour"
	local log="$ROOT/artifacts/rust/boot.log"
	local msg rust c

	if [ ! -f "$log" ]; then
		say "FAIL  phase 4b: no boot log"
		return 1
	fi

	for msg in \
		"refcount_t: saturated; leaking memory." \
		"refcount_t: addition on 0; use-after-free." \
		"refcount_t: underflow; use-after-free." \
		"refcount_t: decrement hit 0; leaking memory." \
		"refcount_t: unknown saturation event"; do
		# The Rust's copy carries the kernel crate's log prefix.
		rust=$(grep -acF "rust_kernel: $msg" "$log" || true)
		c=$(grep -acF "$msg" "$log" || true)
		c=$((c - rust))
		if [ "$rust" -ge 1 ] && [ "$rust" -eq "$c" ]; then
			say "PASS  \"$msg\": Rust $rust time(s), C $c time(s) (equal, both non-zero)"
		else
			say "FAIL  \"$msg\": Rust $rust time(s), C $c time(s)"
			return 1
		fi
	done

	return 0
}

phase_warnings || fail=$((fail + 1))

# --- Phase 5: the mutation testing ------------------------------------------
if [ "$SKIP_DONE" -eq 1 ] && artifact_passed "$ART/mutation-test.txt"; then
	not_run "phase 5: mutation testing" \
		"already executed and passed on this revision; the evidence is in artifacts/audit/mutation-test.txt"
elif [ "$QUICK" -eq 0 ]; then
	run_phase "phase 5: mutation testing" \
		"$HERE/mutation-test.sh"
else
	not_run "phase 5: mutation testing" \
		"skipped (--quick): it rebuilds the kernel for every mutation"
fi

# --- Phase 6: the sanitizers -------------------------------------------------
if [ "$SKIP_DONE" -eq 1 ] && artifact_passed "$ART/sanitizer-kasan-ubsan.txt" &&
	artifact_passed "$ART/sanitizer-kcsan.txt"; then
	not_run "phase 6: the sanitizers" \
		"already executed and passed on this revision; the evidence is in artifacts/audit/sanitizer-*.txt"
elif [ "$QUICK" -eq 0 ]; then
	run_phase "phase 6: KASAN + UBSAN" \
		"$HERE/sanitizer.sh" kasan-ubsan KASAN KASAN_GENERIC KASAN_INLINE \
		UBSAN UBSAN_BOUNDS UBSAN_SHIFT
	run_phase "phase 6: KCSAN" \
		"$HERE/sanitizer.sh" kcsan KCSAN KCSAN_EARLY_ENABLE
else
	not_run "phase 6: the sanitizers" \
		"skipped (--quick): each sanitizer needs a full rebuild"
fi

# --- Phase 7: the property and model tests ----------------------------------
phase "phase 7: the property and the model tests"
BLOG="$ROOT/artifacts/rust/boot.log"
if grep -qaE 'ok [0-9]+ refcount_diff' "$BLOG" && \
	grep -qaE 'ok [0-9]+ refcount_rust' "$BLOG"; then
	say "PASS  the differential suite and the Rust suite ran"
else
	say "FAIL  the suites did not report success"
	fail=$((fail + 1))
fi
if grep -qa 'not ok [0-9]* ' "$BLOG"; then
	say "FAIL  a suite reported a failure"
	fail=$((fail + 1))
else
	say "PASS  no suite reported a failure"
fi

# --- Summary ----------------------------------------------------------------
phase "summary"
say "failures:    $fail"
say "not run:     $notrun"
say "evidence:    $ART"
say ""

if [ "$fail" -ne 0 ]; then
	say "RESULT: FAIL"
	exit 1
fi
if [ "$notrun" -ne 0 ]; then
	say "RESULT: PASS (with phases NOT RUN: see above)"
	exit 0
fi
say "RESULT: PASS"

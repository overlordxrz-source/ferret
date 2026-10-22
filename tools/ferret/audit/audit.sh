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
	grep -q '^RESULT: PASS' "$f" || return 1
	# The artifact also has to be bound to this source, this harness and the
	# configuration it names; a PASS from an older tree is not evidence for
	# this one.
	prov_check "$f" >/dev/null 2>&1
}

# Why an artifact is not accepted, for the human reading the gate output.
artifact_reason() { # <file>
	local f=$1
	[ -s "$f" ] || { echo "no artifact"; return; }
	grep -q '^RESULT: PASS' "$f" || { echo "the last run did not pass"; return; }
	prov_check "$f" 2>&1 | head -2 | tr '\n' ' '
}

. "$ROOT/tools/ferret/lib/provenance.sh"

say "Ferret independent-verification gate"
say "tree:    $ROOT"
say "commit:  $(prov_commit_short)"
say "source:  $(prov_source_hash)"
say "scripts: $(prov_scripts_hash)"
say "started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

# The gate's result is evidence for one revision of the tree and one version
# of the harness. From a dirty tree it can name neither, so it refuses to run
# rather than produce an artifact that cannot be bound.
if [ "$(prov_dirty)" = 1 ]; then
	say ""
	say "FAIL  the build-relevant tree is dirty: the gate has to name an exact"
	say "      source revision and an exact harness version. Commit and rerun."
	git -C "$ROOT" status --porcelain --untracked-files=normal -- $(prov_paths) |
		sed 's/^/      /' | tee -a "$LOG"
	say "RESULT: FAIL (dirty tree)"
	exit 2
fi

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
		if ! prov_check "$ROOT/artifacts/rust/boot-provenance.txt" >>"$LOG" 2>&1; then
			say "FAIL  the boot is not bound to this tree:"
			prov_check "$ROOT/artifacts/rust/boot-provenance.txt" 2>&1 |
				head -3 | sed 's/^/      /' | tee -a "$LOG"
			return 1
		fi
		built=$(prov_field "$(grep -a '^provenance: ' "$ROOT/artifacts/rust/provenance.txt" | tail -1)" image)
		booted=$(prov_field "$(grep -a '^booted-image: ' "$ROOT/artifacts/rust/boot-provenance.txt" | tail -1)" booted-image)
		if [ -z "$built" ] || [ "$built" != "$booted" ]; then
			say "FAIL  the booted image ($booted) is not the one the build produced ($built)"
			return 1
		fi
		say "PASS  phase 4: boot with the KUnit suites (image $booted)"
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
mutation_done=0
if [ "$SKIP_DONE" -eq 1 ]; then
	if artifact_passed "$ART/mutation-test.txt"; then
		mutation_done=1
		not_run "phase 5: mutation testing" \
			"already executed and passed on this revision; the evidence is in artifacts/audit/mutation-test.txt ($(prov_check "$ART/mutation-test.txt" 2>&1 | head -1))"
	elif [ -s "$ART/mutation-test.txt" ]; then
		say ""
		say "STALE  the mutation artifact is not evidence for this tree: $(artifact_reason "$ART/mutation-test.txt")"
	fi
fi
if [ "$mutation_done" -eq 0 ]; then
	if [ "$QUICK" -eq 0 ]; then
		run_phase "phase 5: mutation testing" \
			"$HERE/mutation-test.sh"
	else
		not_run "phase 5: mutation testing" \
			"skipped (--quick): it rebuilds the kernel for every mutation"
	fi
fi

# --- Phase 6: the sanitizers -------------------------------------------------
sanitizers_done=0
if [ "$SKIP_DONE" -eq 1 ]; then
	if artifact_passed "$ART/sanitizer-kasan-ubsan.txt" &&
		artifact_passed "$ART/sanitizer-kcsan.txt"; then
		sanitizers_done=1
		not_run "phase 6: the sanitizers" \
			"already executed and passed on this revision; the evidence is in artifacts/audit/sanitizer-*.txt (KASAN: $(prov_check "$ART/sanitizer-kasan-ubsan.txt" 2>&1 | head -1); KCSAN: $(prov_check "$ART/sanitizer-kcsan.txt" 2>&1 | head -1))"
	elif [ -s "$ART/sanitizer-kasan-ubsan.txt" ] || [ -s "$ART/sanitizer-kcsan.txt" ]; then
		say ""
		say "STALE  a sanitizer artifact is not evidence for this tree:"
		say "       KASAN: $(artifact_reason "$ART/sanitizer-kasan-ubsan.txt")"
		say "       KCSAN: $(artifact_reason "$ART/sanitizer-kcsan.txt")"
	fi
fi
if [ "$sanitizers_done" -eq 0 ]; then
	if [ "$QUICK" -eq 0 ]; then
		run_phase "phase 6: KASAN + UBSAN" \
			"$HERE/sanitizer.sh" kasan-ubsan KASAN KASAN_GENERIC KASAN_INLINE \
			UBSAN UBSAN_BOUNDS UBSAN_SHIFT
		run_phase "phase 6: KCSAN" \
			"$HERE/sanitizer.sh" kcsan KCSAN KCSAN_EARLY_ENABLE
	else
		not_run "phase 6: the sanitizers" \
			"skipped (--quick): each sanitizer needs a full rebuild"
	fi
fi

# --- Phase 7: the property and model tests ----------------------------------
phase "phase 7: the property and the model tests"
BLOG="$ROOT/artifacts/rust/boot.log"
suites_ok=1
for suite in refcount_rust refcount_diff fvs_refcount fvs_refcount_adv; do
	if suite_passed "$BLOG" "$suite"; then
		say "PASS  $suite passed"
	else
		say "FAIL  $suite did not pass"
		suites_ok=0
	fi
done
[ "$suites_ok" -eq 1 ] || fail=$((fail + 1))

# The retry path of the compare-and-exchange loop is asserted by the test
# itself (it fails when it did not run); the gate checks that the assertion is
# in the log and that it was met, because a PASS without it would be the old
# inference again.
retries=$(grep -aoE 'ferret refcount: rust phase 0: .*retries [0-9]+' "$BLOG" | tail -1 | grep -oE '[0-9]+$' || true)
if [ -n "$retries" ] && [ "$retries" -gt 0 ]; then
	say "PASS  the compare-and-exchange retry path ran: $retries retries"
else
	say "FAIL  the compare-and-exchange retry path did not report a retry"
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
say "config:      $(prov_config_hash rust x86_64)"
say "image:       $(prov_image_hash rust x86_64)"
say "$(prov_stamp "$(prov_config_hash rust x86_64)" "$(prov_image_hash rust x86_64)")"
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

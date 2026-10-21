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
LOG="$ART/audit.txt"

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

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

say "Ferret independent-verification gate"
say "tree:    $ROOT"
say "commit:  $(git -C "$ROOT" rev-parse --short=12 HEAD)"
say "started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

# --- Phase 1: the build -----------------------------------------------------
if [ "$QUICK" -eq 0 ]; then
	run_phase "phase 1: clean build of the Ferret kernel" \
		env CLEAN=1 "$ROOT/tools/ferret/build.sh" rust
else
	run_phase "phase 1: incremental build of the Ferret kernel" \
		"$ROOT/tools/ferret/build.sh" rust
fi

# The reference variant must not contain any Ferret code.
phase "phase 1: the reference variant has no Ferret code"
if [ -f /build/linux-ref/.config ]; then
	if grep -qE '^CONFIG_(RUST|CRC16_RUST|KFIFO_RUST|BITMAP_RUST|REFCOUNT_RUST)=y' \
		/build/linux-ref/.config; then
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
if [ -f /build/linux-rust/lib/built-in.a ]; then
	if ar t /build/linux-rust/lib/built-in.a | grep -q '/refcount\.o$'; then
		say "FAIL  lib/refcount.o is linked into the Ferret variant"
		fail=$((fail + 1))
	else
		say "PASS  lib/refcount.o is not linked into the Ferret variant"
	fi
	ar t /build/linux-rust/lib/built-in.a | grep 'refcount' | sed 's/^/      /'
else
	say "SKIP  no Ferret build tree"
fi

# --- Phase 2: the linkage and the ABI --------------------------------------
run_phase "phase 2: linkage and ABI (x86_64)" \
	"$ROOT/tools/ferret/linkage.sh" rust
run_phase "phase 2: linkage and ABI (arm64)" \
	env ARCH=arm64 "$ROOT/tools/ferret/linkage.sh" rust

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
run_phase "phase 4: boot with the KUnit suites" \
	"$ROOT/tools/ferret/boot.sh" rust

# --- Phase 5: the mutation testing ------------------------------------------
if [ "$QUICK" -eq 0 ]; then
	run_phase "phase 5: mutation testing" \
		"$HERE/mutation-test.sh"
else
	not_run "phase 5: mutation testing" \
		"skipped (--quick): it rebuilds the kernel for every mutation"
fi

# --- Phase 6: the sanitizers -------------------------------------------------
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

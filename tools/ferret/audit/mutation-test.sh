#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Ferret: mutation testing for the reference counting port.
#
# A test suite that never fails proves nothing. This script injects a defect
# into the Rust implementation, in a disposable git worktree, rebuilds the
# kernel, boots it and checks that the differential suite notices.
#
# A mutation that the suite does NOT notice is a coverage hole. It is
# reported as a hole, with the reason, and it is never presented as a pass.
#
# Usage:
#   tools/ferret/audit/mutation-test.sh [mutation-name ...]
#
# Environment:
#   MUTATION_BUILD=1  keep the mutated build tree (default: rebuild the
#                      pristine tree at the end)
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
WORKTREES="$ROOT/.audit-worktrees"
ART="$ROOT/artifacts/audit"
LOG="$ART/mutation-test.txt"

mkdir -p "$ART"

say() { echo "$*" | tee -a "$LOG"; }

say "=============================================================="
say "mutation testing run: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "tree:   $ROOT"
say "commit: $(git -C "$ROOT" rev-parse --short=12 HEAD)"
say "=============================================================="


# The mutations. Each one is a Python patch applied to the worktree, and the
# expectation says whether the differential suite is expected to catch it.
#
# "detected"     the suite must fail; a pass is a coverage hole.
# "undetected"   the defect is not observable by a functional test (it is a
#                memory-ordering defect); the suite is expected to pass, and
#                the hole is reported as such.
mutations() {
	cat <<'EOF'
dec-not-one-ignores-one|detected|rust/kernel/sync/refcount.rs|report a count of one as decremented
dec-not-one-underflow|detected|rust/kernel/sync/refcount.rs|decrement the count of zero instead of warning
dec-not-one-saturation|detected|rust/kernel/sync/refcount.rs|decrement the saturated count instead of leaving it alone
saturate-wrong-value|detected|rust/kernel/sync/refcount.rs|saturate the count to zero instead of REFCOUNT_SATURATED
lock-taken-when-it-must-not-be|detected|lib/refcount_rust.rs|take the mutex even when the first step failed
lock-not-released|undetected|lib/refcount_rust.rs|do not release the spinlock on the failure path
irqsave-not-restored|undetected|lib/refcount_rust.rs|do not restore the interrupt state on the failure path
dec-if-one-relaxed|undetected|rust/kernel/sync/refcount.rs|use a relaxed compare-and-exchange instead of a release one
EOF
}

apply_mutation() { # <worktree> <file> <mutation-name>
	local wt=$1 file=$2 name=$3

	python3 - "$wt" "$file" "$name" <<'PY'
import pathlib
import sys

wt, rel, name = sys.argv[1], sys.argv[2], sys.argv[3]
path = pathlib.Path(wt) / rel
text = path.read_text()

if name == "dec-not-one-ignores-one":
	old = """            if val == 1 {
                return false;
            }
"""
	new = """            if val == 1 {
                // MUTATION: the count of one is reported as decremented.
                return true;
            }
"""
elif name == "dec-not-one-underflow":
	old = """            if new > val {
                // The count was zero: this is an underflow. The C warns once
                // and returns true without decrementing.
                warn_once(
                    &UNDERFLOW_WARNED,
                    "refcount_t: underflow; use-after-free.\\n",
                );

                return true;
            }
"""
	new = """            if new > val {
                // The count was zero: this is an underflow. The C warns once
                // and returns true without decrementing.
                warn_once(
                    &UNDERFLOW_WARNED,
                    "refcount_t: underflow; use-after-free.\\n",
                );

                // MUTATION: the underflow also decrements the count.
                let _ = self.as_atomic().cmpxchg(val as i32, new as i32, Release);

                return true;
            }
"""
elif name == "dec-not-one-saturation":
	old = """            if val == REFCOUNT_SATURATED as u32 {
                return true;
            }
"""
	new = """            if val == REFCOUNT_SATURATED as u32 {
                // MUTATION: the saturated count is decremented.
            }
"""
elif name == "lock-taken-when-it-must-not-be":
	old = """    if r.try_dec_not_one() {
        return false;
    }

    // SAFETY: the caller passes a valid, initialised mutex.
    unsafe { bindings::mutex_lock(lock) };
"""
	new = """    if r.try_dec_not_one() {
        // MUTATION: the mutex is taken even when it must not be.
        unsafe { bindings::mutex_lock(lock) };
        return false;
    }

    // SAFETY: the caller passes a valid, initialised mutex.
    unsafe { bindings::mutex_lock(lock) };
"""
elif name == "lock-not-released":
	old = """    if !r.dec_and_test() {
        // SAFETY: the spinlock is held here.
        unsafe { bindings::spin_unlock(lock) };
        return false;
    }
"""
	new = """    if !r.dec_and_test() {
        // MUTATION: the spinlock is not released on the failure path.
        return false;
    }
"""
elif name == "saturate-wrong-value":
	old = """    pub fn set_saturated(&self) {
        self.set(REFCOUNT_SATURATED);
    }
"""
	new = """    pub fn set_saturated(&self) {
        // MUTATION: the count is saturated to the wrong value.
        self.set(0);
    }
"""
elif name == "irqsave-not-restored":
	old = """    if !r.dec_and_test() {
        // SAFETY: the spinlock is held here.
        unsafe { bindings::spin_unlock_irqrestore(lock, flags) };
        return false;
    }
"""
	new = """    if !r.dec_and_test() {
        // MUTATION: the interrupt state is not restored on the failure path.
        unsafe { bindings::spin_unlock(lock) };
        return false;
    }
"""
elif name == "dec-if-one-relaxed":
	old = """    pub fn try_dec_if_one(&self) -> bool {
        self.as_atomic().cmpxchg(1, 0, Release).is_ok()
    }
"""
	new = """    pub fn try_dec_if_one(&self) -> bool {
        // MUTATION: the compare-and-exchange is relaxed, not release.
        self.as_atomic().cmpxchg(1, 0, Relaxed).is_ok()
    }
"""
else:
	raise SystemExit(f"unknown mutation: {name}")

if old not in text:
	raise SystemExit(f"mutation {name}: the patch context was not found in {rel}")

path.write_text(text.replace(old, new, 1))
print(f"mutation {name} applied to {rel}")
PY
}

run_mutation() { # <name> <expectation> <file> <description>
	local name=$1 expect=$2 file=$3 desc=$4
	local wt="$WORKTREES/$name"

	say ""
	say "== mutation: $name ($expect)"
	say "   $desc"

	rm -rf "$wt"
	git -C "$ROOT" worktree prune
	git -C "$ROOT" worktree add --detach "$wt" HEAD >/dev/null 2>&1 || {
		say "FAIL  could not create the worktree $wt"
		return 1
	}

	apply_mutation "$wt" "$file" "$name" >>"$LOG" 2>&1 || {
		say "FAIL  could not apply the mutation"
		git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1
		return 1
	}

	# The build and the boot of the mutated tree. The artifacts of the
	# mutated build overwrite the ones of the pristine build; the clean
	# cycle regenerates them.
	SRC_MOUNT="/mnt/projects/ferret/.audit-worktrees/$name" \
		"$ROOT/tools/ferret/build.sh" rust >>"$LOG" 2>&1
	local build_rc=$?

	if [ "$build_rc" -ne 0 ]; then
		say "INVALID  the mutation did not build: the mutation itself is broken"
		git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1
		return 1
	fi

	SRC_MOUNT="/mnt/projects/ferret/.audit-worktrees/$name" \
		"$ROOT/tools/ferret/boot.sh" rust >>"$LOG" 2>&1
	local boot_rc=$?
	local blog="$ROOT/artifacts/rust/boot.log"

	local failed=0
	if grep -qaE 'not ok [0-9]+ ' "$blog"; then
		failed=1
	fi
	if [ "$boot_rc" -ne 0 ]; then
		failed=1
	fi

	git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1

	if [ "$failed" -eq 1 ]; then
		if [ "$expect" = "detected" ]; then
			say "PASS  the mutation was detected by the test suite"
			return 0
		fi
		say "HOLE  the mutation was detected, but it was expected to be invisible"
		return 1
	fi

	if [ "$expect" = "undetected" ]; then
		say "KNOWN LIMIT  the mutation was NOT detected, as declared"
		say "             the functional suites cannot see it: the defect is"
		say "             either in the memory ordering or on a path that is"
		say "             only reachable under a race. The mitigation is the"
		say "             disassembly review and the code review of that path."
		return 1
	fi

	say "HOLE  the mutation was NOT detected: the suite has a coverage gap"
	return 1
}

selected=("$@")

holes=0
total=0
MUTATION_LIST=$(mktemp)
trap 'rm -f "$MUTATION_LIST"' EXIT
mutations >"$MUTATION_LIST"
while IFS='|' read -r name expect file desc; do
	[ -n "$name" ] || continue
	if [ "${#selected[@]}" -gt 0 ]; then
		want=0
		for s in "${selected[@]}"; do
			[ "$s" = "$name" ] && want=1
		done
		[ "$want" -eq 1 ] || continue
	fi
	total=$((total + 1))
	# The build and the boot inside the worker read the standard input, so
	# they get none: the list of the mutations is read from there.
	run_mutation "$name" "$expect" "$file" "$desc" </dev/null ||
		holes=$((holes + 1))
done <"$MUTATION_LIST"

say ""
say "== mutation testing summary"
say "mutations:  $total"
say "coverage holes: $holes"

if [ "${MUTATION_BUILD:-0}" != "1" ]; then
	say ""
	say "==> restoring the pristine build tree"
	# The rsync preserves the modification times, so the restored files
	# would look *older* than the objects that were built from the
	# mutated ones, and the build system would keep those objects. Touch
	# every file a mutation touched, so that they are rebuilt.
	for f in rust/kernel/sync/refcount.rs lib/refcount_rust.rs; do
		touch "$ROOT/$f"
	done
	"$ROOT/tools/ferret/build.sh" rust >>"$LOG" 2>&1
	say "==> pristine build restored"
fi

if [ "$holes" -ne 0 ]; then
	say "RESULT: FAIL ($holes coverage holes)"
	exit 1
fi
say "RESULT: PASS"

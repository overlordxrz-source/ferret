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

. "$ROOT/tools/ferret/lib/provenance.sh"

say "=============================================================="
say "mutation testing run: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "tree:   $ROOT"
say "commit: $(prov_commit_short)"
say "source: $(prov_source_hash)"
say "scripts: $(prov_scripts_hash)"
say "=============================================================="

# The campaign builds the committed head in disposable worktrees. If the
# build-relevant tree is dirty, the commit named above is not the tree the
# script was run from: the artifact could not be bound to either.
if [ "$(prov_dirty)" = 1 ]; then
	say "FAIL  the build-relevant tree is dirty: the campaign has to name an"
	say "      exact source revision and an exact harness version."
	git -C "$ROOT" status --porcelain --untracked-files=normal -- $(prov_paths) |
		sed 's/^/      /' | tee -a "$LOG"
	say "RESULT: FAIL (dirty tree: no provenance)"
	exit 1
fi


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
dec-not-one-retry-broken|detected|rust/kernel/sync/refcount.rs|do not update the expected value when the compare-and-exchange fails
saturate-wrong-value|detected|rust/kernel/sync/refcount.rs|saturate the count to zero instead of REFCOUNT_SATURATED
lock-taken-when-it-must-not-be|detected|lib/refcount_rust.rs|take the mutex even when the first step failed
lock-not-released|detected|lib/refcount_rust.rs|do not release the spinlock on the failure path
mutex-not-released|detected|lib/refcount_rust.rs|do not release the mutex on the failure path
irqsave-not-restored|detected|lib/refcount_rust.rs|do not restore the interrupt state on the failure path
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
                warn_once!(
                    &UNDERFLOW_WARNED,
                    "refcount_t: underflow; use-after-free.\\n"
                );

                return true;
            }
"""
	new = """            if new > val {
                // The count was zero: this is an underflow. The C warns once
                // and returns true without decrementing.
                warn_once!(
                    &UNDERFLOW_WARNED,
                    "refcount_t: underflow; use-after-free.\\n"
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
elif name == "dec-not-one-retry-broken":
	old = """                Err(current) => {
                    // The count changed between the read and the
                    // compare-and-exchange: update the expected value and
                    // retry, exactly as the C loop does.
                    #[cfg(CONFIG_KUNIT)]
                    retry_stats::record();
                    val = current as u32;
                }
"""
	new = """                Err(current) => {
                    // The count changed between the read and the
                    // compare-and-exchange: update the expected value and
                    // retry, exactly as the C loop does.
                    #[cfg(CONFIG_KUNIT)]
                    retry_stats::record();
                    // MUTATION: the expected value is not updated on
                    // failure, so the loop keeps retrying with a stale
                    // value.
                    let _ = current;
                }
"""
elif name == "mutex-not-released":
	old = """    if !r.dec_and_test() {
        // SAFETY: the mutex is held here.
        unsafe { bindings::mutex_unlock(lock) };
        return false;
    }
"""
	new = """    if !r.dec_and_test() {
        // MUTATION: the mutex is not released on the failure path.
        return false;
    }
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
		say "INVALID  could not create the worktree $wt"
		return 3
	}

	apply_mutation "$wt" "$file" "$name" >>"$LOG" 2>&1 || {
		say "INVALID  could not apply the mutation: the patch context does"
		say "         not match the source. Nothing was tested."
		git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1
		return 3
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
		return 3
	fi

	SRC_MOUNT="/mnt/projects/ferret/.audit-worktrees/$name" \
		"$ROOT/tools/ferret/boot.sh" rust 180 >>"$LOG" 2>&1
	local boot_rc=$?
	local blog="$ROOT/artifacts/rust/boot.log"
	# Keep the boot log of the mutated kernel: the detection has to be
	# auditable, not just a PASS line. The next boot would overwrite it.
	cp -f "$blog" "$ART/mutation-boot-$name.log" 2>/dev/null || true

	# What counts as a detection: the audited suite reports a failure; the
	# kernel panics after the suites started; or the boot does not finish
	# while the suites are running (a mutation that leaves a lock held or
	# loops forever is detected by the hang, and the log shows no verdict).
	# A boot that never got as far as the suites proves nothing about the
	# mutation and must not be recorded as one.
	local kunit_failed=0 panic=0 started=0 verdict=0 hung=0
	grep -qaE 'not ok [0-9]+ ' "$blog" && kunit_failed=1
	grep -qa 'Kernel panic' "$blog" && panic=1
	grep -qaE 'ok [0-9]+ (refcount_diff|refcount_rust|fvs_refcount)' "$blog" && started=1
	grep -qaE '(ok|not ok) [0-9]+ (refcount_diff|refcount_rust|fvs_refcount)' "$blog" && verdict=1

	local failed=0
	if [ "$kunit_failed" -eq 1 ]; then
		failed=1
	elif [ "$panic" -eq 1 ] && [ "$started" -eq 1 ]; then
		failed=1
	elif [ "$boot_rc" -ne 0 ] && [ "$started" -eq 1 ] && [ "$verdict" -eq 0 ]; then
		hung=1
		failed=1
	fi

	if [ "$failed" -eq 1 ]; then
		say "   the failing tests in the mutated kernel:"
		grep -aE 'not ok [0-9]+ ' "$blog" | sed 's/^/      /' | tee -a "$LOG"
		if [ "$kunit_failed" -eq 0 ] && [ "$hung" -eq 0 ]; then
			say "      (none: the kernel panicked after the suites started)"
		fi
		if [ "$hung" -eq 1 ]; then
			say "      (none: the boot did not finish while the suites were"
			say "      running — the mutation hangs the kernel; last lines:)"
			tail -n 5 "$blog" | sed 's/^/      /' | tee -a "$LOG"
		fi
	fi

	local invalid=0
	if [ "$failed" -eq 0 ] && [ "$boot_rc" -ne 0 ] && [ "$started" -eq 0 ]; then
		invalid=1
		say "INVALID  the mutated kernel did not boot far enough to run the"
		say "         audited suites (boot exit $boot_rc, no suite started)."
		say "         This is not a detection; see"
		say "         artifacts/audit/mutation-boot-$name.log"
	fi

	git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1

	# The build tree now holds the objects of the mutated source. The next
	# mutation only restores the files it touches, so the objects of the
	# files this mutation touched have to be rebuilt: touch them, so that
	# the build system does not keep the objects of the mutated source.
	for f in rust/kernel/sync/refcount.rs lib/refcount_rust.rs; do
		touch "$ROOT/$f"
	done

	if [ "$invalid" -eq 1 ]; then
		return 3
	fi

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
		say "             in the memory ordering, which no functional test"
		say "             can observe. The mitigation is the disassembly"
		say "             review and the code review, not the tests."
		# Not a coverage hole: the protocol declares this mutation
		# not detectable, and requires it to be listed with its reason.
		return 2
	fi

	say "HOLE  the mutation was NOT detected: the suite has a coverage gap"
	return 1
}

selected=("$@")

holes=0
limits=0
invalid=0
detected=0
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
	rc=0
	run_mutation "$name" "$expect" "$file" "$desc" </dev/null || rc=$?
	case $rc in
	0) detected=$((detected + 1)) ;;
	2) limits=$((limits + 1)) ;;
	3) invalid=$((invalid + 1)) ;;
	*) holes=$((holes + 1)) ;;
	esac
done <"$MUTATION_LIST"

say ""
say "== mutation testing summary"
say "mutations:  $total"
say "detected:   $detected"
say "declared not detectable: $limits"
say "invalid (did not boot):  $invalid"
say "coverage holes: $holes"

# The campaign's own provenance: the baseline source and harness, and (after
# the pristine restore) the configuration and image the restored tree has.
# `prov_check` reads the last stamp of the artifact.
prov_cfg=-
prov_img=-
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
	prov_cfg=$(prov_config_hash rust x86_64)
	prov_img=$(prov_image_hash rust x86_64)
fi
say "$(prov_stamp "$prov_cfg" "$prov_img")"

if [ "$holes" -ne 0 ]; then
	say "RESULT: FAIL ($holes coverage holes)"
	exit 1
fi
if [ "$invalid" -ne 0 ]; then
	say "RESULT: FAIL ($invalid invalid runs: the mutated kernel did not"
	say "run the suites, so the mutation is neither detected nor cleared)"
	exit 1
fi
if [ "$limits" -ne 0 ]; then
	say "RESULT: PASS (with $limits declared not-detectable mutation:"
	say "a known limit of the tests, listed with its reason in the"
	say "coverage report of the audit)"
	exit 0
fi
say "RESULT: PASS"

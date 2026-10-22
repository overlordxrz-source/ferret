# Audit evidence

The raw evidence of the independent-verification gate
([`AUDIT_PROTOCOL.md`](../../../AUDIT_PROTOCOL.md)) for checkpoint 005, as it
stands after the FVS-AUDIT-001 review. The analysis is in
[`docs/ferret/design/COMPONENT_005_AUDIT.md`](../../docs/ferret/design/COMPONENT_005_AUDIT.md);
this directory holds the files that report claims are made from.

Everything here was produced on the `ferret/005-refcount` branch, by the
scripts in `tools/ferret/audit/`, inside the `ferret-build` container. The
build trees are isolated per run (`OUTROOT`), so no artifact depends on a tree
that another run left behind.

## Provenance: an artifact is evidence for one tree

Every artifact ends with a stamp:

```
provenance: commit=<sha> source=<sha256> scripts=<sha256> config=<sha256|-> image=<sha256|-> dirty=<0|1> generated=<UTC>
```

* `source` hashes the build-relevant files of the tree (the kernel, the Rust
  crate, the harness), by blob hash;
* `scripts` hashes `tools/ferret/` and `AUDIT_PROTOCOL.md` — the harness and
  the gate definition;
* `config` is the kernel `.config` that was built, and a copy of that file is
  archived next to the artifact as `config-<sha256>.txt`, so the binding can
  be rechecked after the build tree has been rebuilt;
* `image` is the kernel image that was built, and a boot records the image it
  actually booted (`booted-image:` in `boot-provenance.txt`), which must be
  the one the build produced;
* `dirty=1` is never accepted: an artifact from a dirty tree cannot name a
  revision.

`tools/ferret/lib/provenance.sh` writes and checks the stamps.
`audit.sh` refuses to run from a dirty tree, and `--skip-done` skips a phase
only when the artifact ends in `RESULT: PASS` *and* its stamp matches the
current source and harness; otherwise it prints `STALE ...` and runs the
phase. `evidence.sh` refuses a stale input.


## `mutation-test.txt`

`tools/ferret/audit/mutation-test.sh` injects a defect into the Rust
implementation in a disposable git worktree created from the **committed**
head, rebuilds the kernel, boots it, and checks whether an audited suite
notices. It is the evidence for the adversarial tests: a test that cannot
distinguish a broken implementation from the correct one is not evidence.

The file **accumulates the runs**, so the history is visible, including the
runs that were wrong. Each run starts with a header naming the time and the
commit that was mutated.

The classification (`AUDIT_PROTOCOL.md` §5):

* **detected** — an audited suite reports `not ok`, the kernel panics after the
  suites started, or the boot hangs while the suites are running;
* **known limit** — declared not detectable, not detected, with the reason;
  it does not fail the run;
* **coverage hole** — declared detectable and not detected, or declared
  invisible and detected; it fails the run;
* **invalid** — the patch did not apply, the worktree could not be created,
  the kernel did not build, or the boot never reached the suites. Nothing was
  tested, so it is neither a detection nor a clearance, and it fails the run.

The boot log of every mutated kernel is archived next to this file as
`mutation-boot-<mutation>.log`, so each detection can be read, not just
believed.

### The final run

`058e0ca1ddce` — **10 mutations: 9 detected, 1 declared limit, 0 coverage
holes, 0 invalid runs.** Summary:

| Mutation | Declared | Result |
| --- | --- | --- |
| a count of one is reported as decremented | detectable | detected |
| the underflow also decrements the count | detectable | detected |
| the saturated count is decremented | detectable | detected |
| the compare-and-exchange loop does not update the expected value on failure | detectable | detected (the concurrency invariant; the retry path itself is counted and asserted, below) |
| the count is saturated to zero | detectable | detected |
| the mutex is taken when it must not be | detectable | detected |
| the spinlock is not released on the failure path | detectable | detected |
| the mutex is not released on the failure path | detectable | detected |
| the interrupt state is not restored on the failure path | detectable | detected |
| the compare-and-exchange is relaxed instead of release | not detectable | not detected, as declared |

The three lock-failure mutations are detected **only** by `fvs_refcount` (the
audit's deterministic failure-path tests); the retry-loop mutation only by
`refcount_diff` (the concurrency invariant). The per-mutation attribution is
in `COMPONENT_005_AUDIT.md` §7.1.

The retry path behind that mutation is not inferred from the final count: the
Rust implementation counts the retries of its compare-and-exchange loop in a
KUnit-only counter, the concurrency test **asserts** the count and prints it
(`ferret refcount: retry path exercised: N retries (attempt M of 5)`), and the
gate checks the line. The workers are placed round-robin on the online CPUs
while they sleep on the completion, so the release lands one per CPU and the
race really happens; measured across 22 boots (during development and on the final
revision): 1208–3640 retries, always on
the first attempt. A boot in which the workers never overlap repeats the
phase up to five times and then **fails** rather than passing silently.

### What the history contains, and why

* The first campaign ran against an older head, **before the audit suites were
  committed**, so it measured the old suite. It is superseded by the run
  above; `COMPONENT_005_AUDIT.md` §7.2 says so.
* A run reported `HOLE` for "the underflow also decrements the count" because
  the patch still expected the pre-macro `warn_once( ... )` text and did not
  apply. A patch that does not apply is now `INVALID`, and the corrected
  mutation is detected.
* The first campaign declared the lock-failure mutations undetectable. That was
  wrong: the path is deterministically reachable, and the mutations are now
  declared detectable and detected. This is finding F-03 of the review.
* The second audit (FVS-AUDIT-001-R2) asked for the compare-and-exchange retry
  path to be asserted rather than inferred. The test now counts the retries in
  a KUnit-only counter and asserts them; the campaign was re-run when the
  barrier changed, and the retry mutation itself was rewritten twice: the
  first version did not compile, the second hung the kernel before the
  suites, and both were recorded as `INVALID` — a mutation that cannot be
  measured is not a detection. Only the run quoted above is claimed; every
  earlier run is stale by its own provenance stamp, and the stamp is how a
  reader can tell.

## `sanitizer-kasan-ubsan.txt`, `sanitizer-kcsan.txt`

`tools/ferret/audit/sanitizer.sh` builds the kernel with the sanitizer
switches, boots it, and reports three things separately: whether the sanitizer
was actually enabled in the kernel that was built (`CONFIG_<switch>=y`), whether
the audited suites passed inside it, and whether any *sanitizer report*
implicates the component.

Both runs **PASS**:

| Run | Switches | Result |
| --- | --- | --- |
| KASAN + UBSAN | `KASAN`, `KASAN_GENERIC`, `KASAN_INLINE`, `UBSAN`, `UBSAN_BOUNDS`, `UBSAN_SHIFT` | no component report, no external report; the four audited suites pass |
| KCSAN | `KCSAN`, `KCSAN_EARLY_ENABLE` | no data-race report, no external report; the four audited suites pass. **Non-strict**: KCSAN reports races, it does not model every weak memory ordering, so a clean run is not evidence that the ordering is correct |

Rules that the artifacts apply, in both directions:

* a report is parsed as a **whole block** (from `cut here` to `end trace`), so
  its stack, not only its header, attributes it;
* a report that cannot be attributed is **unclassified**, and an unclassified
  report fails the run: it is not evidence of anything;
* a report in another component is an **external finding**. It is printed with
  its stack, it is never hidden, and it does not make this component fail;
* KCSAN's clean result is reported as what it is. The artifact says, in the
  run itself, that KCSAN is in its default non-strict mode; the memory
  ordering is argued in the design review and recorded as a declared limit,
  not as a pass;
* a `WARNING: CPU` splat is **not** a sanitizer report. The warning-path tests
  deliberately produce them — 14 of them in each run (7 from the Rust
  implementation, 7 from the C reference) — so they are counted and reported
  as the intentional tests they are.

The boot log of each sanitized kernel is archived as
`sanitizer-<name>-boot.log`. (Earlier runs of these scripts failed to find
their own boot log because the host `ART` path was not translated into the
container; that is defect 9 in `COMPONENT_005_AUDIT.md` §12.1, and the logs
here are from the fixed script.)

## `audit.txt`

The summary of `tools/ferret/audit/audit.sh`, the gate itself: the clean build,
the linkage and symbol-ownership check, the differential-symbol distinctness,
the boot, the warning-behaviour comparison, the mutation and sanitizer
artifacts, and the coverage report. The result of the last run is at the end of
the file.

## What is not here

* **The arm64 boot.** The arm64 kernel is built and its linkage is checked, but
  the host has no arm64 QEMU boot in this configuration; the gate reports that
  phase as `NOT RUN`, not as a pass.
* **Native performance.** The host has no KVM, so any timing would be QEMU TCG.
  The component's cost is not measured, and no number is claimed.
* **A proof of correctness.** Mutation testing shows that the suite notices an
  enumerated set of defects. It does not show that the set is complete, and the
  memory-ordering limit (no functional test can see it) is declared, not
  solved.

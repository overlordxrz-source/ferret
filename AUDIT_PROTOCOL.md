# Ferret independent-verification protocol

This protocol is the gate a Ferret component has to pass before it is
declared **VERIFIED** and before its pull request is merged. It is written so
that somebody who has never seen the implementation — a second agent, or a
human with a fresh checkout — can execute it and reach the same conclusions
without trusting the reports, the test results or the claims of the branch
that is being audited.

The protocol does **not** ask the auditor to believe the branch. Every step
produces its own evidence, and the evidence is required to be reproducible
from a fresh clone of the public repository.

Companion document: [`docs/ferret/design/COMPONENT_005_AUDIT.md`](docs/ferret/design/COMPONENT_005_AUDIT.md)
is the audit of the first component that went through this gate (the
reference counting helpers). It is an example of the report this protocol
produces, not a substitute for running the protocol.

## 0. Scope: what this gate can and cannot establish

It **can** establish:

* that the C implementation is not compiled into the audited build, and that
  the ABI symbols are defined exactly once, with the right size, the right
  export class and a ksymtab entry;
* that the differential suite compares two genuinely different
  implementations, and that it *notices* defects (mutation testing);
* that the code passes the kernel's own sanitizers, in a kernel where the
  audited suites actually run;
* that the behaviour of the port matches a specification that is written
  independently of both the C original and the Rust port (the model test);
* that the memory ordering of the port uses the same kernel primitives as the
  C original, on the architectures that are built, and that this is visible
  in the generated code;
* that the Rust API of the component does not make promises it cannot keep
  (the soundness review).

It **cannot** establish:

* that the implementation is free of defects. The gates above are evidence
  about the *coverage* of the tests, not a proof.
* that the memory ordering is correct on an architecture that is not built.
* anything about the native performance: every measurement in this project is
  a QEMU TCG measurement, and it is labelled as such.
* that the `unsafe` blocks of the Rust code are correct in the sense of the
  Rust specification. The soundness review is a structured argument, and the
  argument is written down so that it can be attacked.

A component that passes this protocol is **verified against the tests that
exist**, and the report has to say exactly which paths are *not* covered.

## 1. Prerequisites

```sh
git clone https://github.com/overlordxrz-source/ferret.git ferret-audit
cd ferret-audit
git log -1 --format='%H %s'          # record the audited commit
```

* A machine with Docker: the builds run in the `ferret-build` container
  (Ubuntu 24.04, aarch64 host, clang/lld 18).
* Disk: the kernel tree is about 1.5 GB, the build tree about 12 GB.
* No state from the developer's machine may be used: the protocol uses a
  fresh output tree (`OUTROOT`) and a fresh worktree for the mutations.

The host helpers do the container plumbing:

```sh
tools/ferret/build.sh ref      # the pristine upstream baseline
tools/ferret/build.sh rust     # the Ferret kernel with the audited component
```

## 2. Phase 1 — independent build

```sh
CLEAN=1 tools/ferret/build.sh ref
CLEAN=1 tools/ferret/build.sh rust
```

Requirements:

* both builds succeed, from scratch;
* the reference variant contains **no** Ferret code:

```sh
grep -E '^CONFIG_(RUST|CRC16_RUST|KFIFO_RUST|BITMAP_RUST|REFCOUNT_RUST)=' \
	/build/linux-ref/.config      # must print nothing
```

* the Ferret variant enables the audited component and its differential
  test, and the C object is not in the build:

```sh
grep -E '^CONFIG_REFCOUNT' /build/linux-rust/.config
ar t /build/linux-rust/lib/built-in.a | grep -c 'refcount\.o'   # must be 0
ar t /build/linux-rust/lib/built-in.a | grep 'refcount'         # the Rust objects
```

The `built-in.a` check is used on purpose: an *incremental* tree can still
hold the stale object of a build made before the migration, and the presence
of a file proves nothing. What matters is what is linked.

## 3. Phase 2 — linkage and ABI verification

```sh
tools/ferret/linkage.sh ref
tools/ferret/linkage.sh rust
```

The script checks, for every exported symbol of the component:

* exactly one definition in `vmlinux` (`nm --print-size vmlinux | grep -c`);
* the symbol is defined by the Rust object, and the export shim only
  *references* it (`nm --print-size lib/…_rust.o`, `nm lib/…_export.o`);
* the size of the linked symbol equals the size of the function in the
  object;
* the `__ksymtab_*` entry exists, and `vmlinux.symvers` records the export
  class (`EXPORT_SYMBOL` vs `EXPORT_SYMBOL_GPL`).

The auditor must not only run the script: the same facts have to be
reproducible by hand, so that a bug in the script cannot hide a defect.

```sh
nm --print-size /build/linux-rust/vmlinux | grep -E ' refcount_'
nm --print-size /build/linux-rust/lib/refcount_rust.o | grep ' T '
nm /build/linux-rust/lib/refcount_export.o | grep -E 'refcount'
grep -E 'refcount' /build/linux-rust/vmlinux.symvers
```

Requirements: one definition per symbol; the export class is unchanged; the
`__ksymtab_*` entries exist; the shim defines nothing.

## 4. Phase 3 — differential-test integrity

A differential suite is worthless if the two sides are the same code, or if
the suite does not exercise the function under test. Three independent
checks are required.

**3.1 — the C reference is the unmodified original.** The reference object
is built from the original file with the symbols renamed; the file itself
must be untouched:

```sh
git diff --stat -- lib/refcount.c          # must be empty
grep -n 'CFLAGS_refcount_ref.o' -A 8 lib/Makefile
```

**3.2 — the two implementations are distinct objects at run time.** The
suite itself asserts it (`KUNIT_ASSERT_NE` on the two addresses, so a
renaming that rewrote the Rust call site would fail the suite), and the
linkage report repeats it at link time:

```sh
grep -E 'are distinct objects' artifacts/rust/linkage.txt
```

**3.3 — the reference symbols stay test-only.** They exist only because the
differential test is enabled, and they are exported under their test-only
names (`…_c_ref`), never under the real names.

```sh
grep -E 'refcount_.*_c_ref' /build/linux-rust/vmlinux.symvers
grep -E '^CONFIG_REFCOUNT_DIFF_KUNIT_TEST' /build/linux-rust/.config
```

## 5. Phase 4 — mutation testing

This is the phase that gives the other phases their meaning: it shows that
the suite *notices* defects. Each mutation is applied in a disposable git
worktree, the kernel is rebuilt and booted, and the suite must fail.

```sh
tools/ferret/audit/mutation-test.sh
```

The script prints, per mutation, whether the defect was **detected**. A
mutation that is not detected is a *coverage hole* and is reported as a
failure of the gate, not as a pass.

The mutation set must cover, at least:

* a defect in the algorithm (a helper that decrements a count it must not);
* a defect in the underflow handling;
* a defect in the saturation handling (a saturated count that is decremented,
  a wrong saturated value);
* a defect in the locking (a lock that is taken when it must not be);
* a defect in the interrupt handling (a missing `irqrestore`);
* a defect in the **memory ordering** (a release compare-and-exchange
  replaced by a relaxed one).

Each mutation is declared *detectable* or *not detectable*, and the reason
has to be written down. A mutation that is declared detectable and is not
detected is a **coverage hole** and fails the gate. A mutation that is
declared not detectable is a *known limitation of the tests* and has to be
listed in the coverage report of phase 9 with the reason.

The mutations that are known to be not detectable, and why:

* a **memory-ordering** defect is not observable by a functional test: a
  relaxed compare-and-exchange produces the same results. The mitigation is
  the disassembly review (phase 7) and the code review, not the tests.
* a defect on a path that is only reachable **under a race** is not
  observable by a deterministic test. The path of the lock helpers where the
  lock has been taken and the decrement to zero then fails is only reachable
  when another task decrements the count between the two steps of the
  helper. A test that drives it has to win a race, which is not something a
  test suite should depend on. The mitigation is the code review of the
  failure path, and the fact that the *other* failure path (the lock is
  taken when it must not be) is covered.

The set that the reference counting component uses is in
`tools/ferret/audit/mutation-test.sh`, and its results are in
`artifacts/audit/mutation-test.txt`.

## 6. Phase 5 — sanitizers

```sh
tools/ferret/audit/sanitizer.sh kasan KASAN KASAN_GENERIC KASAN_INLINE
tools/ferret/audit/sanitizer.sh ubsan UBSAN UBSAN_BOUNDS UBSAN_SHIFT
tools/ferret/audit/sanitizer.sh kcsan KCSAN KCSAN_EARLY_ENABLE
```

Each build boots the kernel with the sanitizers and runs the KUnit suites
inside it, so the audited code is exercised by the sanitizers. The boot log
must contain no sanitizer report, and the differential suite must report
success inside the sanitized kernel.

If a sanitizer is not supported on the host (or the build does not fit in
the time budget), the report says so explicitly and lists it as **NOT RUN**.
It is never reported as a pass.

## 7. Phase 6 — property and fuzz testing

The differential suite contains a randomised property test (a fixed seed, a
pseudo-random sequence of operations applied to both implementations, with
the return value and the count compared at every step) and the Rust suite
contains a *model* test: a third implementation of the semantics, written
from the specification and compared against the port over a long
pseudo-random sequence.

```sh
grep -E 'refcount_diff_property|refcount_diff: pass' artifacts/rust/boot.log
```

An independent fuzzer (a `kunit` property test or a userspace model
checker) may be added; when it is not, the report says which properties are
covered by the in-tree tests and which are not.

## 8. Phase 7 — architecture and ordering semantics

The port must use the *same* kernel primitives as the C original, and this
has to be visible in the generated code, on every architecture that is
built.

```sh
# x86_64 and arm64
llvm-objdump -d --no-show-raw-insn /build/linux-rust/lib/refcount_rust.o
llvm-objdump -d --no-show-raw-insn /build/linux-rust/rust/helpers/helpers.o \
	| sed -n '/<rust_helper_atomic_try_cmpxchg_release>:/,/^$/p'
```

The check is that the helper the Rust code calls is the C primitive
(`atomic_try_cmpxchg_release`), and that the instructions it emits are the
instructions the C inline emits (`lock cmpxchg` on x86_64, `casl` or
`ldxr`+`stlxr` on arm64). A port that re-implements the ordering with
Rust's own atomics has to show the difference here, and the difference has
to be argued for explicitly.

The interrupt semantics are checked by the differential suite: the
`irqsave` helper is compared against the C reference with the interrupt
state observed before and after the call.

## 9. Phase 8 — Rust soundness review

The review is written down, per type, with the questions that have to be
answered:

* **aliasing** — does the type promise (through `PhantomData`, through a
  `&mut`, through a `&`) access to memory that it does not control? For an
  object owned by C, the answer is never "exclusive";
* **ownership** — who owns the object, who owns the reference, what happens
  when the last reference goes away;
* **`Drop`** — what the destructor does, in which order, and what it must
  not do (free memory that is still reachable, call a callback twice, touch
  an object after the release);
* **`into_raw` and friends** — whether a reference is transferred or leaked,
  and what the caller has to do;
* **lifetimes** — what the lifetime parameter really promises, and whether
  the type system enforces it;
* **`Send`/`Sync`** — whether the handle can be moved or shared, and the
  argument for it;
* **the `unsafe` blocks** — the invariant of every one of them.

The review must state, for each claim, whether it is *enforced by the type
system*, *relied on from the C contract*, or *a documented requirement on
the caller*.

## 10. Phase 9 — coverage report

The report has to list, for the audited component:

* the paths that are exercised by a test (with the test that exercises them);
* the paths that are **not** exercised, and why;
* the defects that the suite provably does not catch (from the mutation
  phase);
* the claims that are *not* established (from phase 0).

A component with an empty "not covered" list is a red flag, not a result.

## 11. Evidence requirements

The audit produces, in the repository (or in the pull request):

| File | Content |
| --- | --- |
| `docs/ferret/design/COMPONENT_0NN_AUDIT.md` | the review, the coverage table, the defects that were found and fixed |
| `artifacts/audit/mutation-test.txt` | the mutation results, one line per mutation |
| `artifacts/audit/sanitizer-*.txt` | the sanitizer builds and their boot logs |
| `docs/ferret/evidence/*.txt` | the published evidence summaries (regenerated by `tools/ferret/evidence.sh`) |
| `registry/migrations.json` | the component's status, its tests and their evidence paths |

The registry entry is updated **only after** the phases above have run, and
its status is `migrated` only if the clean cycle passed as well.

## 12. Exit criteria

The gate passes when:

1. phases 1 to 9 have been executed and their evidence is in the repository;
2. every mutation that is expected to be detected was detected;
3. no sanitizer reported anything, or the sanitizer is explicitly listed as
   NOT RUN with the reason;
4. the coverage report lists the uncovered paths;
5. the soundness review has no unresolved finding — a finding that cannot be
   resolved is listed as an open risk in the registry, and the component
   stays `partial`.

The pull request is merged only after the gate passes and the registry
records the evidence. A component that passes its tests but not this gate is
`partial`, and is reported as such.

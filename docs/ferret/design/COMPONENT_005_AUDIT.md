# Component 005 audit — the out-of-line reference counting helpers

Component: `lib/refcount.c` → `lib/refcount_rust.rs`
Checkpoint: 005
Branch: `ferret/005-refcount`
Protocol: [`AUDIT_PROTOCOL.md`](../../../AUDIT_PROTOCOL.md) (the gate this
component went through)

## 1. Scope

The audit covers:

* the six exported helpers of `lib/refcount.c` and their Rust counterparts;
* the extensions this checkpoint added to the in-tree kernel crate
  (`kernel::sync::Refcount` and the new `kernel::sync::KRef`);
* the C helpers this checkpoint added to the Rust-for-Linux helper set;
* the differential suite, and what it does and does not prove.

It does **not** claim that the port is free of defects. Section 9 lists the
paths that no test covers, and section 10 lists the claims that are not
established.

## 2. The `KRef` soundness review

### 2.1 The finding

The first implementation of `KRef` carried `PhantomData<&'a mut T>`:

```rust
pub struct KRef<'a, T: ?Sized> {
    kref: *mut bindings::kref,
    data: *mut T,
    release: KRefRelease,
    _lifetime: PhantomData<&'a mut T>,
}
```

That was **wrong**, and it was wrong in a way the type system did not even
notice.

`PhantomData<&'a mut T>` is a promise that the holder has *exclusive*
access to the `T` for `'a`. For a `KRef` that promise cannot be kept:

* the object is owned by C. The C code may hold other references to it and
  may access it through its own pointers at any time;
* other `KRef`s for the same object may exist at the same time, each holding
  one reference — that is the whole point of a reference count;
* the promise is not enforced either: the object is reached through a raw
  pointer, so the borrow checker never sees the accesses and cannot stop
  them. The `PhantomData` therefore did not restrict anything; it only
  *asserted* something untrue.

Two consequences followed from the false promise:

1. The derived auto-trait bounds were wrong in the conservative direction:
   `KRef<'_, T>` was `Send` only if `T: Send` and `Sync` only if
   `T: Sync`, although the handle does not move the object and does not
   create any reference to it. Moving a `KRef` between tasks moves a
   *reference*, not the object — the C `struct kref` is passed between tasks
   freely.
2. The documentation of the type read as if the handle owned the object
   exclusively, which would have led a user to create `&mut T` views of an
   object that C may be mutating at the same time.

### 2.2 The redesign

The marker was replaced by a lifetime marker that makes no promise about the
aliasing of the object:

```rust
    /// The lifetime of the C object.
    ///
    /// This is a plain lifetime marker. It deliberately says nothing about
    /// the aliasing of the object:
    ///
    /// * `PhantomData<&'a mut T>` would promise *exclusive* access, which
    ///   cannot be proven: …
    /// * `PhantomData<&'a T>` would promise that the object is not mutated
    ///   for `'a`, which the C code may do anyway.
    _lifetime: PhantomData<&'a ()>,
```

and the `Send`/`Sync` behaviour is now stated explicitly, with the argument,
instead of being derived from the object type:

```rust
// SAFETY: the handle owns one reference and nothing else. Moving it to
// another task transfers that reference, and the release callback may then
// run in that task, which is exactly what the C `kref_put()` allows. The
// operations on the count itself are atomic.
unsafe impl<T: ?Sized> Send for KRef<'_, T> {}

// SAFETY: the handle only performs atomic operations on the count and hands
// out a raw pointer to the object. …
unsafe impl<T: ?Sized> Sync for KRef<'_, T> {}
```

The explicit implementations are possible because the struct contains raw
pointers, which already deny the auto traits; the implementations therefore
*replace* the derivation rather than conflicting with it.

### 2.3 The checklist

| Question | Answer | Where it is enforced |
| --- | --- | --- |
| Does the type promise exclusive access to the object? | **No.** The marker is a plain lifetime marker. | the type definition |
| Does the type promise that the object is not mutated? | **No.** | the type definition |
| Who owns the object? | C. The handle owns *one reference*, never the object. | the documentation of `new()` |
| Who owns the reference? | The handle. It is released in `Drop`. | the `Drop` implementation |
| What does `Drop` do? | `refcount_dec_and_test()` on the embedded count; if it reaches zero, the C release callback runs — exactly the C `kref_put()`. | the `Drop` implementation |
| Can `Drop` free an object that is still reachable? | No: the callback runs only when the count reaches zero, which is the C contract for the object's lifetime. The handle's own reference is the one being dropped. | the C `kref` contract |
| Can the release callback run twice for one reference? | No: `Drop` runs once per handle, and the callback only runs when the count reaches zero. | the `Drop` implementation |
| What does `into_raw()` do? | It *transfers* the reference to the C side: the handle is not dropped, and the caller becomes the owner of one reference and must release it with `kref_put()`. | `ManuallyDrop` in `into_raw()` |
| Does `into_raw()` leak? | It transfers, so it does not leak: the C side owns the reference from that point on. The alternative (dropping it) would have been a use-after-free of the count. | the `into_raw()` documentation |
| What does the lifetime `'a` promise? | That the C object outlives the handle. It is *not* enforced by the type system: the C object's lifetime is not a Rust lifetime. It is a requirement on the caller, documented in the safety contract of `new()`. | the safety contract |
| Is the handle `Send`? | Yes: it owns one reference; moving it transfers that reference; the release callback may then run in the destination task, which the C `kref_put()` allows. The count operations are atomic. | the explicit `unsafe impl` |
| Is the handle `Sync`? | Yes: the handle only performs atomic operations on the count and hands out a raw pointer. The accesses to the object through that pointer are the caller's responsibility, exactly as for the C `struct kref`. | the explicit `unsafe impl` |
| Can the handle create a data race by itself? | No: it never dereferences the object pointer. `as_ptr()` returns a raw pointer, and using it is an `unsafe` operation. | the `as_ptr()` documentation |
| Does the handle drop the object? | No: `T` is only ever behind a raw pointer, so the handle never runs a `Drop` for it, and the C release callback is the only thing that frees the object. | the field types |

### 2.4 The remaining, unavoidable boundary

The handle cannot make the C object's lifetime safe. A C object can be
freed by the C code while a Rust handle exists, if the C code drops the
last reference behind the handle's back. That is a requirement on the
caller, and it is stated in the safety contract of `KRef::new()`:

> the object must be alive for the lifetime `'a`; the count must not be
> accessed through the C API in a way that conflicts with the reference held
> here; `release` must be the callback the C code would pass to `kref_put()`
> for this object, and it must be safe to call from any task; the caller must
> not create a data race through the pointer returned by `as_ptr()`.

## 3. The six helpers, one by one

The C original is `lib/refcount.c` (186 lines, unchanged in the tree). The
Rust port is `lib/refcount_rust.rs`; the shared semantics live in
`rust/kernel/sync/refcount.rs`.

### 3.1 `refcount_warn_saturate()`

| Aspect | C | Rust | Same? |
| --- | --- | --- | --- |
| Saturation value | `refcount_set(r, REFCOUNT_SATURATED)` | `Refcount::set_saturated()` → the same `refcount_set()` | yes |
| `REFCOUNT_SATURATED` | `INT_MIN / 2` = `0xc000_0000` | `i32::MIN / 2`, compared as `u32` | yes |
| Message per event | five cases, `WARN_ONCE(1, "refcount_t: " str ".\n")` | the same five texts, byte for byte | yes |
| Unknown event | `"refcount_t: unknown saturation event!?."` | the same | yes |
| Printed once | a plain, racy flag: two tasks can print it twice | an atomic compare-and-exchange: printed exactly once | **no — documented deviation** |
| Warning machinery | `WARN_ONCE` → the taint, the location, the stack dump | `pr_warn!()` + `warn_on!()` → the taint, the location, the stack dump | yes, except the log level of the message line (`KERN_WARNING` instead of the default) |

The taint matters: the C `WARN_ONCE()` sets `TAINT_WARN`, and the Rust
`warn_on!()` sets the same taint flag.

The call direction is the opposite of what it looks like: the C inline
operations of the primitive (`refcount_inc()`, `refcount_dec()`,
`refcount_add()`, …) call `refcount_warn_saturate()` — which is now the Rust
implementation. Every saturation warning in the kernel therefore goes
through the Rust code. There is no recursion: the Rust code never calls the
C `refcount_warn_saturate()`.

### 3.2 `refcount_dec_if_one()`

| Aspect | C | Rust | Same? |
| --- | --- | --- | --- |
| Operation | `atomic_try_cmpxchg_release(&r->refs, &val = 1, 0)` | `Atomic::cmpxchg(1, 0, Release)` | yes |
| Primitive | the C `atomic_try_cmpxchg_release()` | the *same* C primitive, through the kernel crate's `Atomic` | yes |
| Success ordering | release, with a control dependency | release, with a control dependency | yes |
| Failure ordering | relaxed | relaxed | yes |
| Return value | the result of the compare-and-exchange | `cmpxchg(..).is_ok()` | yes |
| The old value on failure | written back to the caller's `val` | returned in the `Err` variant and discarded | yes (the value is not used) |

### 3.3 `refcount_dec_not_one()`

| Aspect | C | Rust | Same? |
| --- | --- | --- | --- |
| Initial read | `atomic_read()` (relaxed) | `Atomic::load(Relaxed)` | yes |
| Saturated count | returns `true`, the count is not touched | the same | yes |
| Count of one | returns `false`, the count is not touched | the same | yes |
| Underflow (count of zero) | `WARN_ONCE`, returns `true`, **the count is not decremented** | the same, including the quirk that the return value is `true` | yes |
| The decrement | `atomic_try_cmpxchg_release()` in a loop, the expected value updated on failure | the same loop, the expected value updated from the `Err` variant | yes |
| The comparison of the count | `unsigned int` | `u32` | yes |

### 3.4 The three lock helpers

| Aspect | C | Rust | Same? |
| --- | --- | --- | --- |
| First step | `refcount_dec_not_one(r)`; if it returns `true`, return `false` **without taking the lock** | the same (`try_dec_not_one()`) | yes |
| The lock | `mutex_lock()` / `spin_lock()` / `spin_lock_irqsave()` | the same C primitives (`bindings::mutex_lock`, `bindings::spin_lock`, the new `spin_lock_irqsave` helper) | yes |
| Second step | `refcount_dec_and_test(r)` (the C inline of the primitive, with its `smp_acquire__after_ctrl_dep()`) | the same C inline, reached through the Rust-for-Linux helper `rust_helper_refcount_dec_and_test` (in `rust/helpers/refcount.c`), so the step is a **call into a C object** and not an inlined copy of the inline | yes |
| On failure | the lock is released and `false` is returned | the same | yes |
| On success | `true` is returned and the lock is **held** by the caller | the same | yes |
| Interrupts | `spin_lock_irqsave(lock, *flags)`; the failure path does `spin_unlock_irqrestore(lock, *flags)` | the same, through the C helper | yes |

The "the lock is held on success" part is the reason the Rust code does not
use the kernel crate's `Guard`: a guard would unlock the lock when it goes
out of scope, which is not what the C ABI promises.

### 3.5 The ordering, in the generated code

The port does not re-implement the ordering: it calls the same C
primitives. The disassembly of the helper the Rust code calls is the
disassembly of the C inline:

| Architecture | The helper `rust_helper_atomic_try_cmpxchg_release` | The C inline |
| --- | --- | --- |
| x86_64 | `lock cmpxchgl` | `lock cmpxchgl` |
| arm64 | `casl` (LSE) with the `ldxr`+`stlxr` fallback | `casl` with the `ldxr`+`stlxr` fallback |

The failure ordering is relaxed in both cases, as in the C original. A
port that had used Rust's own `compare_exchange` would have had to argue
about the difference between "release on success, relaxed on failure" and
Rust's "release on success, acquire on failure"; the port does not have that
argument to make, because it calls the C primitive.

The cost of that choice is one function call per operation, where the C
inlined the primitive. No throughput number is claimed: the host has no
hardware virtualisation, and every measurement would be a QEMU TCG figure.

## 4. The differential-test integrity

The differential suite (`lib/refcount_diff_kunit.c`) compiles the unmodified
`lib/refcount.c` a second time under test-only symbol names
(`lib/refcount_ref.o`) and compares the two implementations.

The checks that the two sides are *different*:

1. **In the suite**: every test starts with `KUNIT_ASSERT_NE` on the two
   addresses, so a renaming that had rewritten the Rust call site (which
   happened in the bitmap checkpoint) fails the suite instead of passing it
   for the wrong reason.
2. **At link time**: the linkage report checks that the C reference is
   defined by `lib/refcount_ref.o`, that the Rust and the C reference live
   at *different addresses* in `vmlinux`, and that the reference symbols
   exist only because the differential test is enabled.
3. **In the source**: `lib/refcount.c` is untouched in the tree, and the
   rename is a list of `-D` options in `lib/Makefile`.

The suite covers: the count of zero, the count of one, `INT_MAX`, the
saturated count, the negative ("bad") counts, the saturation warning (all
five events and the unknown one), the three lock helpers (return value,
resulting count, the state of the lock, the interrupt state), an
interleaved sequence of decrements, a randomised property test (2 000
pseudo-random operations, return value and count compared at every step),
and a concurrency phase (four tasks over a shared count, and the race for
the last reference).

The Rust suite adds a *model* test: a third implementation of the two
decrement helpers, written from the specification and compared against the
port over 20 000 pseudo-random operations. A port that misread the C would
have to misread the specification in exactly the same way to escape it.

## 5. The architectures

| | x86_64 | arm64 |
| --- | --- | --- |
| Build | PASS | PASS |
| Linkage report | PASS | PASS |
| Boot | PASS | NOT RUN (the boot harness is x86_64-only) |
| The compare-and-exchange | `lock cmpxchg` | `casl` / `ldxr`+`stlxr` |
| The interrupt state | checked by the differential suite | same code path, same check |

## 6. The findings and what was done about them

| # | Finding | Severity | Action |
| --- | --- | --- | --- |
| 1 | `KRef` promised exclusive access to a C-owned object (`PhantomData<&'a mut T>`) | high: a false promise, and the wrong auto-trait bounds | the marker was replaced and `Send`/`Sync` were stated explicitly with the argument (section 2) |
| 2 | The first version of the concurrency test was racy: `kthread_stop()` kills a task that has not been scheduled yet, so one of the four tasks silently did nothing | high in the test, not in the port: the count ended 250 too high, and *both* implementations showed it | the test now waits for the tasks to signal that they are running; the harness defect is recorded in `docs/ferret/STATUS.md` |
| 3 | The exported reference symbols are exported under their test-only names in the differential build (a consequence of the unmodified C file's `EXPORT_SYMBOL()` calls being renamed) | low: they do not collide with the real names and they do not exist outside the differential build | documented; the linkage report checks the collision and the configuration |
| 4 | The `WARN_ONCE()` semantics differ: the C flag is racy, the Rust one is atomic | low: the Rust version warns *less* often, never more | documented |

No defect was found in the six helpers themselves: the differential suite,
the model test and the mutation testing did not find one.

## 7. The mutation testing

`tools/ferret/audit/mutation-test.sh` injects a defect into the Rust
implementation, in a disposable git worktree created from the **committed**
head (so the campaign tests what the branch contains, not a developer's
uncommitted edits), rebuilds the kernel, boots it and checks that the audited
suites notice. The raw results are in `artifacts/audit/mutation-test.txt`, and
the boot log of every mutated kernel is archived next to it as
`artifacts/audit/mutation-boot-<mutation>.log`, so a claimed detection can be
read, not just believed.

The classification is the one of `AUDIT_PROTOCOL.md` §5:

* **detected** — an audited suite reports `not ok`, or the kernel panics after
  the suites started, or the boot hangs while the suites are running (a
  mutation that never releases a lock is noticed by the hang; the archived log
  shows where it stopped);
* **known limit** — a mutation declared not detectable, not detected, with the
  reason written down; it is listed in §9 and it does not fail the gate;
* **coverage hole** — a mutation declared detectable that was not detected, or
  a declared-invisible mutation that *was* detected (the declaration was
  wrong); it fails the gate;
* **invalid** — the mutated kernel did not boot far enough to run the suites;
  neither detected nor cleared, and it fails the run. A boot that failed for an
  infrastructure reason is not evidence that a suite noticed anything.

### 7.1 The mutations

The campaign of `artifacts/audit/mutation-test.txt` (the run against the
committed head `058e0ca1ddce`) executed all ten mutations: **nine detected, one
declared limit, zero coverage holes, zero invalid runs.** The last column names
the suite whose verdict failed in the mutated kernel, read from the archived
boot log.

| Mutation | Declared | Result |
| --- | --- | --- |
| a count of one is reported as decremented | detectable | **detected** — `refcount_rust`, `refcount_diff`, `fvs_refcount`, `fvs_refcount_adv` |
| the underflow also decrements the count | detectable | **detected** — `refcount_diff`, `fvs_refcount`, `fvs_refcount_adv` |
| the saturated count is decremented instead of being left alone | detectable | **detected** — `refcount_rust`, `refcount_diff`, `fvs_refcount`, `fvs_refcount_adv` |
| the loop gives up after a failed compare-and-exchange instead of retrying | detectable | **detected** — `refcount_diff`: the decrement phase starts one above the total, so no refusal is legitimate, and the test asserts `refused == 0`; giving up is caught there (and the retry counter would have stopped growing) |
| the count is saturated to zero instead of `REFCOUNT_SATURATED` | detectable | **detected** — `refcount_rust`, `refcount_diff`, `fvs_refcount`, `fvs_refcount_adv` |
| the mutex is taken even when the first step failed | detectable | **detected** — `refcount_diff`, `fvs_refcount` |
| the spinlock is not released on the failure path | detectable | **detected** — `fvs_refcount` (the deterministic failure-path test) |
| the mutex is not released on the failure path | detectable | **detected** — `fvs_refcount` |
| the interrupt state is not restored on the failure path | detectable | **detected** — `fvs_refcount` |
| the compare-and-exchange is relaxed instead of release | not detectable | **not detected, as declared** — the known limit of the tests (§9, §10) |

The three lock-failure mutations are detected **only** by the audit's
deterministic failure-path tests: the spinlock, the mutex and the `irqsave`
variants each fail `fvs_refcount` and nothing else. That is the direct
measurement of F-03: the path is reachable without winning a race, and the
tests do reach it.

The retry-loop mutation is detected only by the concurrency invariant of
`refcount_diff`: the single-threaded tests cannot see a lost update. That is
the direct measurement of F-21.

### 7.2 What the campaign establishes, and what it does not

The campaign shows that the suite *notices* a specific, enumerated set of
defects. It does not show that the implementation is correct, and it does not
show that the set is complete: a defect nobody thought of is not covered by
inventing mutations. The one declared limit is the memory ordering (the last
row), and §10 says what the mitigation is.

The first version of this campaign had two defects of its own, and they are
worth recording because both made the result look better than it was:

* the "lock is not released" and "interrupt state is not restored" mutations
  were declared *not detectable* on the argument that the failure path needs a
  race. FVS-AUDIT-001 showed that the path is deterministically reachable, so
  the mutations are now declared detectable — and detected (F-03);
* the campaign did not build the working tree, it built `HEAD`. The audit
  suites were uncommitted at the time, so the first runs measured the *old*
  suite; they are re-run on the commit that contains the suites, and only those
  results are reported here;
* the "underflow also decrements" patch was written against the pre-macro
  `warn_once( ... )` text and no longer applied after `warn_once` became a
  macro. A patch that does not apply is now `INVALID` (nothing was tested), not
  a coverage hole; the failed first run is kept in the artifact's history, and
  the run quoted above contains the corrected patch.

The artifact is append-only, so the history of the campaign — including the
intermediate runs that were wrong for the reasons above — is readable next to
the final result. Only the last run is claimed.

## 8. The sanitizers

`tools/ferret/audit/sanitizer.sh` builds the kernel with the kernel
sanitizers, boots it, and checks three things: that the audited suites ran
*inside* the sanitized kernel, that no report implicates the audited
component, and that every report which implicates another component is listed
with its owner. The results are in `artifacts/audit/sanitizer-*.txt`, and each
sanitized boot log is archived as
`artifacts/audit/sanitizer-<name>-boot.log`, so the report can be read after
the next boot overwrites `artifacts/audit/boot.log`.

The report parsing is deliberately conservative:

* a report is parsed as a **whole block**, from the `cut here` marker to the
  matching `end trace`, so the stack is attributed, not just the header
  (F-14);
* a report that cannot be attributed to a component is **unclassified**, and
  an unclassified report fails the phase: an unattributable report is not
  evidence of anything;
* a report in another component is an **external finding**. It is printed in
  full with its stack, it is never hidden, and it does not make this component
  fail. The two results are kept apart in the artifact, and the phase prints
  `RESULT: PASS (with external findings: ...)` in that case;
* a `WARNING: CPU` splat is **not** a sanitizer report. The warning-path tests
  deliberately produce them (7 on the Rust side and 7 on the C reference side
  in the KASAN boot), so they are counted and reported as the intentional
  tests they are; only a block whose header is `BUG: KASAN`, `BUG: KCSAN`,
  `KCSAN: data-race`, `UBSAN:` or `runtime error:` is a finding.

A boot that produces no kernel output is reported as "the kernel did not
boot", and never as "no sanitizer report": the script reads the boot log the
boot actually wrote (`$ART/boot.log`, with `ART` exported to the boot), and it
checks the KASAN banner, so a silent failure cannot pass for a clean run.

| Sanitizer | Result |
| --- | --- |
| KASAN + UBSAN | **PASS** — `CONFIG_KASAN{,,_GENERIC,_INLINE}` and `CONFIG_UBSAN{,_BOUNDS,_SHIFT}` are `=y` in the kernel that was built, the four audited suites pass inside it, no report implicates `refcount` and there is no external report. The 14 kernel WARN splats in the log are the warning-path tests (7 Rust, 7 C reference), not sanitizer findings. |
| KCSAN | **PASS, non-strict** — `CONFIG_KCSAN=y` and `CONFIG_KCSAN_EARLY_ENABLE=y` are set in the kernel that was built, the four audited suites pass inside it, no data-race report implicates `refcount` and there is no external report. The same 14 WARN splats are the warning-path tests (7 Rust, 7 C reference). KCSAN runs in its **default, non-strict mode**: it reports races between instrumented accesses, it does not model every weak memory ordering, and a clean run is *not* evidence that the ordering is correct (see §10). |

The earlier checkpoint recorded two UBSAN reports in `include/linux/log2.h`,
reached from the `kfifo_diff` suite (issue #25). That defect is fixed on this
branch by the FIFO commit (`3835c75c906a`); the two reports are re-checked in
this run, and if any report remains it is listed above as an external finding
with its owning component.

## 9. The coverage: what is tested and what is not

| Path | Covered by |
| --- | --- |
| `refcount_warn_saturate()`, all five events and the unknown one | `refcount_diff_warn_saturate` |
| The saturation value written by the warning | `refcount_diff_warn_saturate`, `refcount_diff_dec_if_one` (the saturated count) |
| `refcount_dec_if_one()`, all the interesting counts | `refcount_diff_dec_if_one` |
| `refcount_dec_not_one()`, all the interesting counts | `refcount_diff_dec_not_one` |
| The underflow path (the count of zero) | `refcount_diff_dec_not_one` (the count of 0) |
| The three lock helpers: the return value, the count, the lock state, the interrupt state | `refcount_diff_dec_and_mutex_lock`, `…_dec_and_lock`, `…_dec_and_lock_irqsave` |
| The interleaving of the helpers | `refcount_diff_sequence` |
| The randomised behaviour | `refcount_diff_property` (2 000 operations, half of them drawn from the boundary table); the test **asserts** that every branch it claims to cover was taken, and prints the coverage counters in the boot log |
| The specification model | `fvs_refcount_adv_dec_helpers_match_the_contract_model_on_the_boundaries` and `..._over_a_reaching_distribution` — the contract model of the independent audit, not a transliteration of the implementation |
| The boundaries of the count space | `fvs_refcount`'s six sweeps: every helper over 19 boundary counts, with the contract spelled out per state |
| The failure path of the three lock helpers | `fvs_refcount`'s three deterministic tests: the lock is made contended, so the helper blocks between its two steps while the count changes underneath it; the lock must be released and the interrupt state must be restored |
| The concurrent behaviour | `refcount_diff_concurrent` (4 tasks): released together through a completion, so the compare-and-exchange loop actually retries; the test asserts `refcount_read == initial - decrements`, `decrements + refused == total` and `refused == 0` (the count starts above the total, so a refusal means the loop gave up), and reports the distribution in the boot log |
| The compare-and-exchange **retry** path of `try_dec_not_one()` | `refcount_diff_concurrent`: the workers are released through a completion, the Rust implementation counts the retries in a KUnit-only counter, and the test **asserts** that the count is non-zero (it repeats the phase at most 5 times, prints the count and the attempt, and fails if no retry was observed). Measured across 22 boots (during development and on the final revision): 1208–3640 retries, every one on the first attempt. The `dec-not-one-retry-broken` mutation is detected by the same invariant |
| The warning **location**: one reported line per saturation event | the boot log: each event reports its own `rust/kernel/sync/refcount.rs:<line>` — six distinct sites for the six saturation events, plus a seventh for the underflow in `try_dec_not_one`, each reported once — as the C's `REFCOUNT_WARN()` call sites do; phase 4b checks the texts and the counts |
| The `KRef` lifetime, the release callback, the reference transfer | `fvs_refcount_adv_kref_new_does_not_take_a_reference` (the drop releases the adopted reference) and `..._into_raw_then_c_kref_put_releases_once` |
| The `KRef` with two handles over one reference | `fvs_refcount_adv_two_handles_over_one_reference_release_the_object_early` |
| The `KRef` on a saturated and on a dead count | `fvs_refcount_adv_get_on_a_saturated_count_takes_no_reference` and `..._dead_count_...` |
| The `KRef` released by the C side | `fvs_refcount_adv_into_raw_then_c_kref_put_releases_once` |
| The `Send`/`Sync` bounds of `KRef` | `fvs_refcount_adv_kref_is_send_and_sync` (a compile-time proof) |
| The atomic ordering | **not covered by a functional test** — by construction (see the mutation testing: the relaxed mutation is declared not detectable) |
| The IRQ state of the `irqsave` helper, on arm64 | **not covered**: arm64 is built and linked, but not booted |
| The saturation warning, as the *kernel* would hit it (through the C inline operations) | **not covered**: no test drives `refcount_inc()` into a saturation |
| The `KRef` against a real C object (a C structure with an embedded `struct kref`) | **not covered**: the test uses a Rust structure with the same layout |
| The `KRef` released by another task | **not covered**: the release callback always runs in the task that drops the handle |
| The release callback running in a different task than the one that took the reference | **not covered** |

## 10. The claims that are not established

* The port is not proven correct. The evidence is a set of tests that
  *notice* a set of injected defects, and a review that argues about the
  rest.
* The memory ordering is not verified by a test. It is verified by the code
  review (the same C primitives) and by the disassembly. On x86_64 a relaxed
  compare-and-exchange and a release one are the same instruction
  (`lock cmpxchg`), so no functional test *can* see the difference; the
  mutation testing records that as a declared, documented limit of the
  tests, not as a pass. **KCSAN does not change this**: it runs in its
  default, non-strict mode, so it reports races between instrumented
  accesses but does not model every weak memory ordering. The KCSAN result is
  "no report", not "the ordering is correct".
* The failure path of the lock helpers, after the lock has been taken, is
  covered deterministically (not by winning a race): the holder contends the
  lock, the helper blocks between its two steps, and the count is changed
  underneath it. A lock that is not released, a mutex that is not released
  and an interrupt state that is not restored are all caught by that test
  (the three mutations are detected).
* The arm64 behaviour is verified by the build and the linkage report, not
  by a boot: the interrupt state and the compare-and-exchange are checked by
  the same source code, but the arm64 kernel is never run in this checkpoint.
* The native performance is not measured. The host has no hardware
  virtualisation, and the port adds one function call per atomic operation.
* The `KRef` does not make the C object's lifetime safe; that remains a
  requirement on the caller.
* The saturation warning path of the *kernel* (the C inline operations) is
  not exercised by a test, only the exported function is.

## 11. The state of the component

The registry records the component as `migrated`: the clean cycle passed
16/16 on the branch that contains this audit, and the gate in
[`AUDIT_PROTOCOL.md`](../../../AUDIT_PROTOCOL.md) passed on the same branch.

The evidence, in the order it was produced (all of it on the branch that
contains this audit and its answers):

| What | Command | Result |
| --- | --- | --- |
| the adversarial tests are in the tree | `lib/fvs_refcount_adversarial_kunit.c`, `lib/fvs_refcount_adversarial.rs` | 10 C cases + 8 Rust cases, under `CONFIG_FVS_REFCOUNT_ADVERSARIAL_KUNIT_TEST` |
| the mutation campaign | `tools/ferret/audit/mutation-test.sh` | 10 mutations: 9 detected, 1 declared limit, 0 holes, 0 invalid |
| the retry path, focused | `artifacts/audit/retry-coverage.txt` | 5 boots on the final revision, 2119–3404 retries, always on the first attempt (22 boots in total on the branch: 1208–3640) |
| the sanitized kernels | `tools/ferret/audit/sanitizer.sh kasan-ubsan ...`, `... kcsan ...` | both PASS: the four audited suites pass inside, no report implicates the component, no external report |
| the clean cycle | `CLEAN=1 tools/ferret/test.sh` | 16/16 steps |
| the gate | `tools/ferret/audit/audit.sh --skip-done` | PASS (phases 5 and 6 were skipped: their artifacts already ended in `RESULT: PASS` from the runs above) |
| the arm64 boot | — | **NOT RUN**: the arm64 kernel is built and its linkage is checked, but the host has no arm64 boot in this configuration |
| the performance | — | **NOT TESTED**: the host has no KVM, so any timing would be QEMU TCG |
(An earlier revision of this document said `partial`, which contradicted the
registry; the independent audit FVS-AUDIT-001 called the contradiction out,
and it is fixed here.)

Every row of that table is bound to the tree: each artifact carries a
`provenance:` stamp with the hash of the build-relevant source, the hash of
the harness (`tools/ferret` + `AUDIT_PROTOCOL.md`), the hash of the kernel
configuration (with the `.config` archived next to the artifact) and the hash
of the image, and the gate refuses a dirty tree or an artifact whose stamp
does not match the current tree (`tools/ferret/lib/provenance.sh`). The
published evidence in `docs/ferret/evidence/` is regenerated by
`tools/ferret/evidence.sh` from those artifacts, and it refuses a stale
input.

The pull request stays open until the gate has been re-run on the revision
that answers the audit.

## 12. The independent audit FVS-AUDIT-001 and the disposition of its findings

The independent audit
([FVS-AUDIT-001](https://github.com/overlordxrz-source/ferret/issues/26),
run against the pinned head `32b9daabdbee`) reached the verdict
**REQUIRE FURTHER EVIDENCE**: it found no defect in the port itself, but it
found that the *evidence* of this audit was weaker than it read, and that
several of the coverage claims did not hold. Its adversarial test suites were
taken into the tree, and the findings are disposed of here.

| ID | Finding | Disposition |
| --- | --- | --- |
| F-01 | The randomised property test never reached the count 0, 2, the saturated count or `INT_MAX`; the 1→0 transition happened 0 times in 383; the lock was taken 0 times in 809 lock-helper calls; 21 % of the iterations asserted a tautology. | **FIXED** — the property test now draws its state from the boundary table half of the time, performs an operation in every iteration, and **asserts that every branch it claims to cover was taken**. The coverage claim is a measurement again. |
| F-02 | `RefcountModel` was a line-for-line transliteration of the implementation, not the specification-derived oracle its documentation claimed. | **FIXED** — the transliteration is deleted. The oracle is now the *contract model* of the audit's own suite (`lib/fvs_refcount_adversarial.rs`): it states the contract per state, derives the saturated value from the contract, and computes the boundary values instead of hard-coding them. |
| F-03 | The lock helpers' failure path was declared "not covered" and its two mutations "not detectable"; the path is in fact deterministically reachable (by making the lock contended, so that the helper blocks between its two steps while the count changes underneath it). | **FIXED** — the audit's three deterministic failure-path tests are in the tree (`lib/fvs_refcount_adversarial_kunit.c`), and the three mutations are now declared *detectable*. The justification that "a race means untestable" was wrong. |
| F-04 | `KRef::new()` does not take a reference, and its safety contract did not say so; a literal reading yields a premature free. | **FIXED** — the contract now states the adopted-reference precondition in full, the type documentation says that the handle *adopts* a reference, and the audit's two-handles test (which demonstrates the consequence) is in the tree. |
| F-05 | The lifetime of `KRef<'a, T>` is not enforced: `new()` does not take it, so a `'static` handle can be built over a stack object. | **ACCEPTED RISK** — acknowledged before the audit and unchanged by it. The type system cannot enforce the lifetime of a C-owned object; the audit's compile-time proof is in the tree as a reminder. |
| F-06 | `Refcount::from_ptr<'a>` has the same unbounded-lifetime property. | **ACCEPTED RISK** — the same, documented in the safety contract. |
| F-07 | The saturation warning differs from the C in the log prefix (`rust_kernel: `) and in the reported location (one line for all five events, so the event can no longer be read off the warning). | **PARTIALLY FIXED** — the reported location is now the call site of each event: the boot log shows seven distinct sites, each reported once (`refcount.rs:182`, `:188`, `:191`, `:194`, `:197`, `:199` for the six saturation events and `:241` for the underflow in `try_dec_not_one`), as the C's `REFCOUNT_WARN()` call sites do. The log prefix is the kernel crate's and cannot be changed without changing the crate's printing; **ACCEPTED RISK** for the prefix. |
| F-08 | The differential suite never compared the warning path: a Rust implementation that never warned would pass. | **FIXED** — the gate's phase 4b compares the warning from the boot log: the differential suite drives all five saturation events on both implementations, and the check requires each message to appear, with the **Rust count equal to the C count** and both non-zero. (The one-shot property itself is *not* claimed as a test result: the events are driven one at a time, so the C's racy flag and the Rust's atomic flag print the same number; that deviation remains a review argument.) |
| F-09 | The audit report described the decrement-to-zero step as "the same C inline"; it is a call into a C object (`rust_helper_refcount_dec_and_test`). | **FIXED** — the wording is corrected in §3.4. |
| F-12 | The gate as executed ended in `FAIL` (the recorded sanitizer run has no `RESULT: PASS`), and criterion 3 of the protocol was not met; KCSAN was never run. | **FIXED** — the gate is re-run with the revised scripts, the criterion now says what it has to say (no report implicates the audited component; the reports in other components are listed with their tracking issue), and the KCSAN phase is executed. |
| F-13 | The registry said `migrated`, this audit report said `partial`, and the first commit message said `partial`. | **FIXED** — §11 and the registry agree. |
| F-14 | The "points at the audited component" filter only inspected the report *headers*, not the stacks; and the committed evidence predated the committed script, so the script's assertions had never been machine-checked. | **FIXED** — the reports are now parsed as whole blocks (from the "cut here" marker to the "end trace" marker), and the evidence is produced by the current script. |
| F-17 | The port adds lines to the **vendored upstream** Rust-for-Linux `kernel` crate, which every future upstream synchronisation will conflict with; the registry's `remaining_c_dependencies` mentioned only the export shim. | **FIXED** — the divergence is listed in the registry as a dependency, with the same reasoning as the other checkpoints that extended the crate. |
| F-18 | `Refcount::read()` returned `i32` while the C `refcount_read()` returns `unsigned int`. | **FIXED** — it returns `u32`, so a saturated count reads back as `0xc000_0000`, exactly as in C. |
| F-19 | The saturated and dead paths of `KRef`, the two-handle case and the "the release callback runs in another task" case were untested. | **PARTIALLY FIXED** — the saturated count, the dead count, the two-handle case and the transfer back to C are covered by the audit's tests, which are in the tree. The **"release callback runs in another task" case is still not covered**: no test drops a handle from a second task, and it is listed as a limitation in §9. |
| F-20 | The reference variant was not built by the audit, so the export CRC was not compared numerically. | **ACCEPTED RISK** — the CRC is preserved *by construction* (the upstream headers are untouched), and the project's clean cycle builds the reference variant. |
| F-21 | The "concurrency phase" did not actually contend (the tasks were created one after the other, so the first one finished before the next started), so the compare-and-exchange **retry** path was never executed and a broken retry loop passed the whole suite. | **FIXED** — the workers are released together through a completion, the Rust implementation counts the retries of the loop in a KUnit-only counter, the test **asserts** the count (repeating the phase at most 5 times, printing the count and the attempt, and failing if it never sees a retry), and the gate checks the count in the boot log. Measured after placing the workers on the CPUs while they sleep on the completion: 22 boots (during development and on the final revision), 1208–3640 retries, every one on the first attempt. The `dec-not-one-retry-broken` mutation (give up after a failed compare-and-exchange) is detected by the `refused == 0` assertion. |
| F-22 | The published evidence (`docs/ferret/evidence/*`) was extracted from an older revision (`f281ebf3b2e6`), and its boot log did not show the adversarial suites at all. | **FIXED** — the evidence is regenerated from the current tree; every published file carries the source, harness and configuration hashes, and the boot evidence lists every suite verdict explicitly and refuses to publish if `fvs_refcount` or `fvs_refcount_adv` did not run or did not pass. |
| F-23 | No artifact and no `--skip-done` result was bound to the revision, configuration and harness version that produced it, so a stale `PASS` could be read as current. | **FIXED** — `tools/ferret/lib/provenance.sh` stamps every artifact with the build-relevant source-tree hash, the harness hash (`tools/ferret` + `AUDIT_PROTOCOL.md`), the configuration hash (with a copy of the `.config` archived next to the artifact) and the image hash; the gate refuses to run from a dirty tree, checks the stamp before skipping a phase and prints why an artifact is stale; `evidence.sh` refuses a stale input. |

Two findings of the audit are worth keeping in the report, because they are
about the *method* and not about the code:

* the differential reference is genuinely differential (the renaming is a
  preprocessor redefinition, so it applies to the whole translation unit and
  the C reference never calls the Rust implementation) — but three of the
  compared behaviours are shared C code on *both* sides: the
  decrement-to-zero step, the lock primitives, and the atomic primitive
  itself. The only genuinely Rust algorithm under differential test is the
  `dec_not_one` compare-and-exchange loop. That is now stated in §5;
* the `#[export]` attribute in this tree only emits `#[no_mangle]` and a
  signature check; the `__ksymtab_*` entries come from the C shim. There is
  therefore no duplicate-export hazard — a fact the audit confirmed
  empirically.

### 12.1 Defects found in the harness while answering the audit

Re-running the gate after these changes exposed ten defects in the audit
*scripts*, each of which could have produced a false result. They are listed
here because the point of the exercise is that the evidence is checkable, and
a harness that reports the wrong thing is a finding in its own right.

| Harness defect | Consequence before the fix | Fix |
| --- | --- | --- |
| `lib/boot-ubuntu-in-container.sh` booted `/build/linux-$KV`, outside the run's `OUTROOT` | the clean cycle's four Ubuntu boots ran a stale kernel left in `/build` by another run; the "Ferret kernel release" check failed on a kernel this tree never built | the kernel image path is `$OUTROOT/linux-$KV`; only the Ubuntu root filesystem, a fixture, is shared |
| `audit/sanitizer.sh` read `artifacts/rust/boot.log` while the sanitized boot wrote `artifacts/audit/boot.log` | a sanitized kernel that built, booted and passed the suites was reported as "the kernel did not boot" | the boot log is `$ART/boot.log`, and `ART` is exported to the boot script |
| `audit/audit.sh --skip-done` treated any non-empty artifact as a completed phase | a phase whose recorded run ended in `FAIL` was reported as `NOT RUN (already executed)`, which hides the failure | a phase is skipped only when its artifact's last line is `RESULT: PASS` |
| `audit/mutation-test.sh` treated any boot failure as a detection | an infrastructure failure (a boot that never reached the suites) would have been recorded as "the suite noticed the defect" | a detection is a `not ok` from an audited suite, or a panic after the suites started; a boot that never reaches them is `INVALID` and fails the run |
| `audit/mutation-test.sh` counted a declared not-detectable mutation as a coverage hole | the run could never pass while the documented memory-ordering limit existed, contradicting `AUDIT_PROTOCOL.md` §5 | declared limits are counted separately and listed in the coverage report; only unexpected outcomes are holes |
| `audit/audit.sh` phase 4b required each warning text exactly once | the underflow text has **two** sites on both sides (the saturation event and `dec_not_one`'s own underflow), so the check would have failed a correct port; it also omitted the "unknown event" message | the check is parity: each of the five messages must appear, with the Rust count equal to the C count and both non-zero |
| `audit/mutation-test.sh` counted a mutation whose patch does not apply as a coverage hole | the "underflow also decrements" patch went stale when `warn_once` became a macro; the run reported a hole although nothing had been tested | a patch that does not apply, a worktree that cannot be created and a build failure are `INVALID` runs; only a booted mutation can be a hole |
| `audit/sanitizer.sh` checked "the sanitizer is on" with a banner loop that could never match, and accepted `ok N refcount_diff` as a pass even when the verdict was `not ok` | the phase could report a sanitized kernel that was not sanitized, or a failing suite as passing | the sanitizer is read from `CONFIG_<switch>=y` of the kernel that was built, and the four audited suites must have a passing verdict line (`ok N <suite>`, not `not ok N <suite>`) |
| `audit/sanitizer.sh` treated every `WARNING: CPU` splat as a sanitizer report | the 14 WARN splats that the warning-path tests deliberately produce (7 Rust, 7 C reference) were parsed as component findings, so a clean sanitized kernel failed | only blocks whose header is a sanitizer report are findings; the WARN splats are counted and reported as the intentional tests they are |
| `docker/run.sh` passed the host `ART` path into the container unchanged | the boot wrote `$ART/boot.log` into the *container's* filesystem, and the host-side script, reading the same path on the host, found nothing: a sanitized kernel that booted and passed every suite was reported as "the kernel did not boot" (this happened twice before it was diagnosed) | a path under `$HOST_PROJECTS` is translated to `/mnt/projects/...`, an already-container path is accepted, and any other path is refused with an error instead of being silently unusable |

### 12.2 The second audit (FVS-AUDIT-001-R2) and what it changed

The second pass found no defect in the port either. It found three ways in
which the evidence could still mislead a reader, and answering them exposed
two more defects of the harness:

* **F-21 — the retry path was inferred, not asserted.** The concurrency test
  argued from the final count and from the spread of the successful
  decrements; it never showed that the compare-and-exchange loop retried.
  The first barrier was a `msleep(1)` poll, and the measurement was blunt: of
  three boots, one had 430 retries and two had none. A busy-wait barrier was
  worse (four spinning tasks on a two-CPU guest starve the test thread that
  has to release them: the release arrived after the spin timeout in every
  boot). The design that works is a completion barrier, a KUnit-only retry
  counter in the implementation, an explicit assertion, and a bounded repeat
  of the phase when the workers happen not to overlap. Placement was the
  missing piece: with the completion barrier alone, 1 boot in 5 never
  overlapped its workers even after five attempts, and the assertion failed
  loudly (which is the honest outcome, but a flaky test is not evidence
  either). Placing the sleeping workers round-robin on the online CPUs makes
  the release land one worker per CPU; measured after the change: 22 boots
  (during development and on the final revision), 1208–3640 retries, every one on the first attempt, and still a failure —
  not a silent pass — if a future boot never retries.
* **F-22 — the published evidence was older than the tree.** The files in
  `docs/ferret/evidence/` were extracted from `f281ebf3b2e6`, before the
  adversarial suites existed; their boot log did not contain a single `fvs_`
  line. `tools/ferret/evidence.sh` now requires the source, harness and
  configuration hashes in every input, lists every suite verdict explicitly,
  and refuses to publish if `fvs_refcount` or `fvs_refcount_adv` did not run
  or did not pass.
* **F-23 — nothing bound an artifact to the tree that produced it.** Every
  artifact now carries a stamp (see §11) and every consumer checks it. The
  gate refuses a dirty tree, does not skip a phase whose stamp is stale, and
  prints the reason. Two consequences of the check are visible in this
  campaign: the artifacts of the first campaign are *rejected* as stale, and
  the second campaign had to be run after the harness commit rather than
  before it.

The two harness defects found while answering R2:

| Defect | Consequence | Fix |
| --- | --- | --- |
| The per-suite checks in `boot-in-container.sh` and in the gate's phase 7 matched `ok [0-9]+ <suite>` as a substring | `not ok 17 refcount_diff` contains `ok 17 refcount_diff`, so a failing suite could be reported as passing in the per-suite line (the global `not ok` check still failed the boot, so the two could contradict each other) | the verdict is matched against the whole line, and a passing line must not be a `not ok` line: `suite_passed()` in the gate and the anchored pattern in the boot script |
| The retry coverage was written down as "the contention is real: the successful decrements are spread over the workers" | an inference presented as a measurement; it was also wrong in boots where the workers did not overlap | the count is measured, printed, asserted, and repeated up to five times; the report says "measured", never "inferred" |

### 12.3 The third round (R3): the approval and the merge

The third pass returned **APPROVE** for the head
`c49a7437aa1892503360c7b4f87df7cbe7ab9ba6`. The pull request was merged as
PR #24 in the merge commit
`33629bd547af0a3894b0fd9604ece469cc14091c`; the merged tree is identical to
the approved head (main was an ancestor, and the two trees hash the same), so
the merge added no content to what was audited. The record is
[`../evidence/fvs-audit-001-r3-approval.md`](../evidence/fvs-audit-001-r3-approval.md).

The evidence the approval rests on is the evidence in §11; the parts that are
declared rather than demonstrated are listed in §10 and repeated, with the
operational consequences, in the approval record. In summary:

* the memory ordering is not verified by a functional test, and KCSAN — which
  runs non-strict — does not verify it either;
* the arm64 test kernel is built and linked but not booted;
* native performance is not measured (no KVM);
* `KRef` cannot enforce a C-owned object's lifetime, and no test drops a
  handle from a second task;
* two harness follow-ups are tracked in issue #27: archiving the kernel
  configuration next to the `test.sh` and `mutation-test.sh` stamps, and the
  stale concurrency-test comment (the constants are 4 tasks × 1 000
  decrements, not "a thousand decrements" in total).

The checkpoint is closed. The next checkpoint is not started.

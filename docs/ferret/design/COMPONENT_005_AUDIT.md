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
| Second step | `refcount_dec_and_test(r)` (the C inline of the primitive, with its `smp_acquire__after_ctrl_dep()`) | the same C inline (`Refcount::dec_and_test()`) | yes |
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
implementation, in a disposable git worktree, rebuilds, boots and checks
that the suite notices. The raw results are in
`artifacts/audit/mutation-test.txt`.

| Mutation | Declared | Result |
| --- | --- | --- |
| a count of one is reported as decremented | detectable | **detected** |
| the underflow also decrements the count | detectable | **detected** (the first version of the mutation did not build, because the "warn once" flag became dead code and the kernel crate builds with `-D warnings`; the mutation was corrected and re-run) |
| the saturated count is decremented instead of being left alone | detectable | **detected** |
| the count is saturated to zero instead of `REFCOUNT_SATURATED` | detectable | **detected** |
| the mutex is taken even when the first step failed | detectable | **detected** |
| the spinlock is not released on the failure path | not detectable | not detected, as declared: the failure path is only reachable under a race |
| the interrupt state is not restored on the failure path | not detectable | not detected, as declared: same reason |
| the compare-and-exchange is relaxed instead of release | not detectable | not detected, as declared: a functional test cannot see the ordering |

The three "not detectable" mutations are the *known limitations* of the
tests, and they are the reason the coverage report of section 9 has to
list them. They are not swept under the carpet: the mutation testing is
what measures them.

Five mutations of the eight are detected by the suite. The suite is not
vacuous: it notices a wrong return value, a wrong underflow handling, a
saturated count that is decremented, a wrong saturated value, and a lock
that is taken when it must not be.

## 8. The sanitizers

`tools/ferret/audit/sanitizer.sh` builds the kernel with the kernel
sanitizers, boots it, and checks that the audited suites run inside it and
that the log contains no sanitizer report. The results are in
`artifacts/audit/sanitizer-*.txt`.

| Sanitizer | Result |
| --- | --- |
| KASAN + UBSAN | the kernel builds, boots, and the audited suites pass inside it: `refcount_diff` 9/9 and `refcount_rust` 7/7. **No sanitizer report points at the audited component.** |
| KCSAN | **NOT RUN** — the build and the boot of a KCSAN kernel do not fit in the checkpoint's time budget. |

Two UBSAN reports *were* produced by the same kernel, and they are **not**
in the audited component: they are `shift-out-of-bounds` in
`include/linux/log2.h`, reached from the `kfifo_diff` suite of the earlier
checkpoint 003 (`kfifo_diff_init` and `kfifo_diff_alloc`). The boot of the
sanitized kernel therefore reports a KUnit failure and the boot check
fails; the failure belongs to the FIFO port, and it is recorded here as a
finding for that component rather than as a result of this one.

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
| The randomised behaviour | `refcount_diff_property` (2 000 operations) |
| The specification model | `refcount_rust_dec_helpers_match_the_model` (20 000 operations) |
| The concurrent behaviour | `refcount_diff_concurrent` (4 tasks) |
| The `KRef` lifetime, the release callback, the reference transfer | `refcount_rust_kref_*` (3 cases) |
| The atomic ordering | **not covered by a functional test** — by construction (see the mutation testing) |
| The lock helpers' failure path *after* the lock was taken (the decrement to zero fails) | **not covered**: that path is only reachable when another task decrements the count between the two steps of the helper, so a test that drives it has to win a race |
| The underflow of the lock helpers' second step | **not covered**: same reason (it needs the race above) |
| The IRQ state of the `irqsave` helper, on arm64 | **not covered**: arm64 is built and linked, but not booted |
| The saturation warning, as the *kernel* would hit it (through the C inline operations) | **not covered**: no test drives `refcount_inc()` into a saturation |
| The `KRef` against a real C object (a C structure with an embedded `struct kref`) | **not covered**: the test uses a Rust structure with the same layout |
| The release callback running in a different task than the one that took the reference | **not covered** |

## 10. The claims that are not established

* The port is not proven correct. The evidence is a set of tests that
  *notice* a set of injected defects, and a review that argues about the
  rest.
* The memory ordering is not verified by a test. It is verified by the code
  review (the same C primitives) and by the disassembly. A relaxed
  compare-and-exchange would pass every test in the tree; the mutation
  testing shows exactly that.
* The failure path of the lock helpers, after the lock has been taken, is
  not covered by a test: it is only reachable when another task decrements
  the count concurrently. A defect there (a lock that is not released, an
  interrupt state that is not restored) passes the whole suite; the
  mutation testing measures that hole instead of hiding it.
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

The registry records the component as `partial` until the clean cycle has
run on the branch that contains this audit. The gate in
[`AUDIT_PROTOCOL.md`](../../../AUDIT_PROTOCOL.md) is what the component has
to pass before the status becomes `migrated` and the pull request is merged.

# Ferret: checkpoint 005 investigation — ownership and reference counting

Status: **investigation**. Nothing is claimed as migrated. This document
selects the next component, states the exact C interfaces, the dependencies,
the test plan and the Rust abstractions that still have to be built.

## 1. Where checkpoint 004 left us

The bitmap audit recorded the structural gaps that stand between Ferret and
the components that matter for memory management, filesystems, networking and
drivers:

| Gap | Consequence |
|---|---|
| no lifetime handle for a **C-owned** object | a Rust component cannot hold a `struct file`, `struct inode`, `struct device` or `struct sk_buff` safely |
| no per-CPU abstraction | `percpu_counter`, `percpu-refcount` and `objagg` are blocked |
| no waitqueue abstraction | `mempool`'s blocking path and `sbitmap` are blocked |
| the XArray is still C | the IDR is a thin layer over it and cannot be ported first |
| the alternatives mechanism does not reach Rust | the population count is slower (issue #21) |

The first gap is the one that blocks the most: **every** C object with a
lifetime (the VFS's inodes and files, the netdevs, the devices) is owned by a
reference count plus a destructor callback. That is exactly what `kref` and
`refcount_t` are.

## 2. The candidates, evaluated

| Candidate | LOC | Exports | Real ownership/concurrency challenge | Rust prerequisites | Verdict |
|---|---|---|---|---|---|
| `lib/idr.c` | 668 | 13 | ID lifetime, chunked tree, spinlock | **the XArray**: `lib/idr.c` is a thin layer over it (25 `xa_*` calls, `idr_rt` is a `radix_tree_root`) | **blocked** on a 2 484-line, 41-export component |
| `mm/mempool.c` | 664 | 17 | pool ownership, spinlock, GFP at run time | **a slab** (the pool is slab-backed) and a **waitqueue** (the blocking path) | **blocked** on two abstractions |
| `lib/llist.c` | 94 | 3 | lock-free list, ABA, memory ordering | `Atomic::cmpxchg` exists ✓ | feasible, but the *risk* (the ordering) is exactly what a test cannot check; the leverage is the scheduler and the workqueue, not MM/VFS/networking |
| `lib/refcount.c` | 186 | 6 | the saturation protocol, the "no increment from zero" rule, the lock-and-drop handoff | `kernel::sync::Refcount` exists ✓; the saturation-aware add does not | **selected** |
| `lib/objagg.c` | 1 039 | 12 | aggregation with per-CPU state | `kernel::percpu` | blocked |
| `lib/percpu-refcount.c` | 479 | 9 | per-CPU batching | `kernel::percpu` | blocked |
| `lib/rhashtable.c` | 1 256 | 16 | resizing under a lock | — | too large for one checkpoint |

### Why the IDR is not the answer

It looks like the natural "resource lifetime" component — 460 files use
`idr_alloc`/`ida_alloc` (PCI, netdevs, block devices, filesystems). But
`lib/idr.c` is no longer a radix tree of chunks: it is a thin layer over the
**XArray** (`#include <linux/xarray.h>`, `idr_rt` is a `radix_tree_root`, 25
`xa_*` calls). Porting the IDR means porting the XArray first — the kernel's
most intricate data structure, with RCU, spinlocks, multi-index entries, tags
and shadow entries. That is a project of its own, and the IDR would be the
second checkpoint of it, not the first.

### Why mempool is not the answer

`mm/mempool.c` is a good ownership component (a pool owns its elements, the
caller owns what it takes), but it is slab-backed, and the kernel crate has no
slab allocator; its blocking allocation path also needs a waitqueue, which the
kernel crate does not have. Two missing abstractions, each of them bigger than
the component.

### Why the lock-free list is not the answer

`lib/llist.c` is small and the kernel crate's `Atomic` has `cmpxchg`, so it
is *feasible*. But the risk in a lock-free list is the memory ordering and the
ABA window, and neither can be checked by a test: a single-threaded
differential suite would pass even if the ordering were wrong. Its consumers
(the scheduler, the workqueue, RCU) are not the memory-management,
filesystem, networking or driver paths that the roadmap targets. It is a
milestone for the *concurrency* theme, not a leverage play.

## 3. The selected component: `lib/refcount.c`

**Why.** It is the portable part of the kernel's reference-counting
machinery, it is the substrate every C lifetime is built on, and it is the
place where the missing "C-owned lifetime" abstraction can land with a real
user instead of speculatively.

**What moves.** The six exported helpers of `lib/refcount.c`:

```c
void  refcount_warn_saturate(refcount_t *r, enum refcount_saturation_type t);
bool  refcount_dec_if_one(refcount_t *r);
bool  refcount_dec_not_one(refcount_t *r);
bool  refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock);
bool  refcount_dec_and_lock(refcount_t *r, spinlock_t *lock);
bool  refcount_dec_and_lock_irqsave(refcount_t *r, spinlock_t *lock,
                                   unsigned long *flags);
```

**What stays in C, and why.** The `refcount_t` primitive itself lives in
`include/linux/refcount.h` (an inline atomic with the saturation protocol and
the debug variants). It is a header, not an object: it cannot be "migrated" as
a file, and the C code that embeds it in its own structures must keep
compiling. The port therefore keeps the C primitive and moves the *helpers* to
Rust, exactly the way the kernel's own `kref`/`refcount` split works.

**The dependencies.**

| Dependency | State |
|---|---|
| `refcount_t` (the C primitive) | stays in C; the Rust side reaches it through the kernel crate's `kernel::sync::Refcount`, which already wraps it |
| the saturation-aware increment | **missing**: `kernel::sync::Refcount` has `inc`, `dec` and `dec_and_test`, but not the "warn on saturation" add that `refcount_warn_saturate()` implements |
| `struct mutex` / `spinlock_t` in the lock-and-drop helpers | the kernel crate has `Mutex` and `Spinlock`; the C helpers take the raw C lock types, so the Rust port needs a way to name them (the kernel crate's locks wrap the C types ✓) |
| the lock-and-drop protocol | the C helper decrements the count and, if it reaches zero, takes the lock, re-checks the count and only then calls the destructor. The Rust port must reproduce the "drop the reference, then take the lock" ordering exactly, including the `lockdep` expectations |

**The Rust abstractions that still have to be built.**

1. **`kernel::sync::Refcount::add_checked()`** (or an equivalent): the
   saturation-aware increment, with the same "never increment from zero" rule
   and the same warning, so that `refcount_warn_saturate()` can be expressed
   in Rust rather than forwarded.
2. **`kernel::sync::KRef`**: a Rust handle for a **C-owned** object with a
   destructor — the abstraction the bitmap audit identified as missing. It
   wraps the C `struct kref` (a header primitive, like `refcount_t`) and gives
   Rust code a typed "get / put, and the put returns the object to the
   destructor" handle. This is the piece that unblocks the VFS, networking and
   driver work later.
3. A Rust `Drop`-safe wrapper for the lock types, so that the
   `dec_and_lock` family can be expressed without the caller having to name
   the raw C lock.

**The test plan.**

| Test | What it proves |
|---|---|
| differential KUnit suite: the unmodified `lib/refcount.c` compiled a second time under test-only symbol names, compared with the Rust implementation | the helpers behave identically, including at the saturation boundary |
| Rust unit tests: a deterministic sequence of increments and decrements around the boundary (0, 1, 2, `INT_MAX`) | the saturation protocol and the "no increment from zero" rule |
| the upstream `refcount` KUnit suite (it lives in the header, so it keeps testing the C primitive) | the primitive is unchanged |
| a differential test of the new `KRef` against the C `kref` (a header primitive, with the same "last put calls the destructor" contract) | the lifetime handle behaves like the C one |
| linkage evidence: the C object absent, one definition per symbol, the export class preserved | the ABI |
| the clean cycle | the checkpoint |

**The verification risk, stated up front.** The helpers are thin, and the
*interesting* part (the primitive) stays in C, so the checkpoint is not a
showcase. Its value is the two abstractions: without `KRef` the VFS, the
networking and the driver migrations have no way to hold a C object, and
without the saturation-aware increment the Rust helpers cannot be written
without forwarding to C — which the porting policy forbids.

**Exit criteria.** `lib/refcount.c` is not built in the Rust variant, the six
symbols are defined once by Rust with their export class preserved, the
differential suite and the Rust unit tests pass, `kernel::sync::KRef` exists
with its own differential test against the C `kref`, and the clean cycle
passes.

## 4. The order of the work

1. `kernel::sync::Refcount`: the saturation-aware increment (with the warning),
   so the Rust helpers need no C forwarding.
2. `kernel::sync::KRef`: the C-owned lifetime handle, with a differential test
   against the C `kref`.
3. `lib/refcount.c` → `lib/refcount_rust.rs`, with the differential suite and
   the Rust unit tests.
4. The linkage, the registry, the audit and the clean cycle.

Steps 1 and 2 are kernel-crate changes of the same kind as the GFP mask and
the NUMA node: they expose primitives that already exist in C.

## 5. What is not selected, and why it is still on the list

* `lib/idr.c` — after the XArray.
* `mm/mempool.c` — after a slab and a waitqueue.
* `lib/objagg.c`, `lib/percpu-refcount.c`, `lib/percpu_counter.c` — after a
  per-CPU abstraction.
* `lib/llist.c` — feasible, but the verification of the ordering is weak and
  the leverage is outside the target areas; it is a good candidate for a
  *concurrency* milestone once the ownership work is done.
* `lib/rhashtable.c` — too large for one checkpoint.
* The CRC family (CRC-ITU-T, CRC-8, CRC-7) — good starter work for new
  contributors, no architectural leverage.

## 6. Open items that are not part of a component

* The population-count regression (issue #21) — a kernel-crate mechanism, not
  a component.
* The 32-bit test lane (issue #22) — infrastructure.

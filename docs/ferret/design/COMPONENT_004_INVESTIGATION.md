# Ferret: allocation, reference counting and ownership (checkpoint 004 investigation)

Status: **investigation**. Nothing in this document is claimed as migrated.
The selected target for the next real Rust replacement is `lib/bitmap.c`, and
it needs one small prerequisite in the kernel crate (section 5).

## 1. Why this checkpoint is about abstractions

Checkpoint 002 (CRC-CCITT) and checkpoint 003 (the generic kernel FIFO) proved
the *mechanism*: a C object is replaced by a Rust object, the C object stops
being built, the exported symbols keep their visibility, and a differential
suite compares the two implementations.

What they did not prove is that the **ownership and allocation substrate** a
larger component needs is expressible in Rust. The kfifo port hit that limit
directly: `__kfifo_alloc()` takes the caller's GFP mask, and the `kernel` crate
cannot express a run-time mask, so the port needed a C helper
(`rust/helpers/slab.c`) to reach `kmalloc_array()`.

The components that come next — memory management, VFS, networking, drivers —
all need the same three things:

1. **allocation with the caller's context** (GFP mask, sleeping or not),
2. **a lifetime for an object owned by C** (refcount, RCU, list membership),
3. **a way to hand a reference to Rust code without transferring ownership**.

This document surveys what the tree already provides, what is missing, which
real C components can be migrated, and which one to do next.

## 2. What the Rust side already has

| Need | Available | Notes |
|---|---|---|
| Allocation | `kernel::alloc::{KBox, KVBox, KVec, VVec}`, `Flags` | `Flags` only exposes the *fixed* flag sets (`GFP_KERNEL`, `GFP_ATOMIC`, …) |
| Run-time allocation flags | **missing** | `Flags::as_raw()` is `pub(crate)`; there is no `from_raw()`. A caller-supplied GFP mask cannot be expressed. |
| Reference counting | `kernel::sync::Refcount` | wraps the C `refcount_t`; `inc`, `dec`, `dec_and_test` |
| Rust-owned shared ownership | `kernel::sync::{Arc, UniqueArc, ArcBorrow}` | Rust-allocated objects only |
| Lifetime handle for a **C-owned** object | **missing** | `ARef<T>` requires `AlwaysRefCounted`, i.e. Rust ownership |
| Atomics | `kernel::sync::atomic::Atomic` | `load/store/xchg/cmpxchg/fetch_add` |
| Locks | `kernel::sync::{Mutex, Spinlock, RefCount, Completion}` | |
| Memory barriers | `kernel::sync::barrier::{smp_mb, smp_rmb, smp_wmb}` | the *same* primitives the C code uses (see checkpoint 003) |
| Per-CPU state | **missing** | no `kernel::percpu` module |
| Wait queue | **missing** | no waitqueue abstraction |
| Bitmap | `kernel::bitmap::OwnedBitmap` | a Rust bitmap with a *different*, higher-level API than the C one |

The two structural gaps are the **run-time GFP mask** and the **C-owned
lifetime handle**. Everything else a mid-sized component needs is already
there.

## 3. Real C candidates, measured in this tree

LOC and export counts are from this tree. "Rust prerequisite" is what is
missing before the component can be ported.

| Component | LOC | Exports | Exercises | Rust prerequisite | Verdict |
|---|---|---|---|---|---|
| `lib/refcount.c` | 186 | 6 | reference counting, locking | none (`kernel::sync::Refcount` exists) | feasible, but the *primitive* (`refcount_t`) stays in the C header, so the win is partial |
| `lib/llist.c` | 94 | 3 | lock-free list, atomics, memory ordering | none (`Atomic::cmpxchg` exists) | feasible, small; exercises the ordering rules rather than ownership |
| `lib/bitmap.c` | 882 | 31 | allocation, bit operations, 1 191 call sites | **run-time GFP mask** (for `bitmap_alloc`) | feasible after the small prerequisite; widest reach |
| `mm/mempool.c` | 664 | 17 | allocation with run-time flags, slab, spinlock | **run-time GFP mask** | feasible after the same prerequisite; used by fs, net and block |
| `lib/idr.c` | 668 | 13 | ID allocation, rbtree, spinlock | none (`kernel::rbtree`, `kernel::sync::Spinlock` exist) | feasible; supports PCI, netdev and block device drivers |
| `lib/objagg.c` | 1 039 | 12 | allocation + refcounting + per-CPU | **`kernel::percpu`** | blocked |
| `lib/percpu-refcount.c` | 479 | 9 | per-CPU + refcounting | **`kernel::percpu`** | blocked |
| `lib/percpu_counter.c` | 407 | 8 | per-CPU + atomics + irqsave lock | **`kernel::percpu`** | blocked (already recorded in the checkpoint 002 investigation) |
| `lib/sbitmap.c` | 802 | 23 | scalable bitmap, kvmalloc, waitqueue | `kvmalloc` binding, waitqueue | blocked |
| `lib/rhashtable.c` | 1 256 | 16 | hashing, resizing, locking | — | large; not a checkpoint-sized unit |
| `lib/xarray.c` | 2 484 | 41 | the kernel's central index | — | far too large |
| `mm/slab.c`, `mm/page_alloc.c` | thousands | — | the memory manager itself | — | explicitly out of scope for now |

## 4. How the candidates support the four target areas

* **Memory management.** The MM needs a bitmap (page flags, memory hotplug,
  `struct zone` free lists) and a per-CPU allocator. The bitmap is
  reachable now; the per-CPU half is not, and `kernel::percpu` is a project
  of its own (allocation, per-CPU access, the architecture assembly boundary).
* **VFS.** Inodes, dentries and files are reference-counted C objects with
  RCU-protected lookups. The missing piece is a Rust handle for a C-owned
  object — `ARef` does not apply — which is why the VFS is not a candidate
  yet, independent of any component.
* **Networking.** `sk_buff` is the hardest data structure in the kernel:
  shared data between clones, headroom/tailroom, paged fragments, per-CPU
  caches. A bitmap is a much better first step (device maps, tc filters).
* **Drivers.** The device model, PCI and netdev registration are dominated by
  ID allocation (`idr`/`ida`) and by reference-counted device objects. Both
  `lib/idr.c` and `lib/refcount.c` are reachable; the per-device lifetime
  handle is the missing piece.

## 5. The one prerequisite: a run-time allocation mask

`kernel::alloc::Flags` is a newtype over `u32` whose constructor is private
and whose raw accessor is `pub(crate)`:

```rust
pub struct Flags(u32);
impl Flags {
    pub(crate) fn as_raw(self) -> u32 { self.0 }
}
```

So Rust code can use `GFP_KERNEL` or `GFP_ATOMIC`, but it cannot forward a
caller's mask. Every C component that takes a `gfp_t` argument — and there
are many — hits this. The kfifo port worked around it with a C helper that
calls `kmalloc_array()`; `lib/bitmap.c` (`bitmap_alloc(nbits, flags)`) and
`mm/mempool.c` (`mempool_alloc(pool, gfp)`) need the same.

The fix is small and reviewable: give `Flags` a public `from_raw(u32)` and a
public `as_raw()`, exactly as the kernel crate already does for the other
wrappers (`gfp_t` is `unsigned int` on every architecture). With that, the
kfifo C helper can be retired and a whole class of components becomes
reachable.

This is an addition to the in-tree `kernel` crate, not a change to upstream
Linux; it is the kind of abstraction upstream Rust for Linux would accept,
because it exposes an existing C primitive without changing it.

## 6. Selected target: `lib/bitmap.c`

**Why this one.** It is the widest-reach component that is reachable, it
exercises the allocation half of this checkpoint's theme, it has a complete
object boundary (no header change, no C fallback), and it has an independent
upstream test suite (`lib/test_bitmap.c`) to validate the port.

* `lib/bitmap.c`: 882 lines, 31 `EXPORT_SYMBOL` exports, ~1 191 files and
  ~7 700 references in the tree. The heaviest users are `drivers/net` (201
  files), `drivers/gpu` (62), `drivers/iio` (30), `fs/xfs` (29),
  `drivers/infiniband` (28) and `drivers/scsi` (25).
* The exported functions fall into four groups: allocation (`bitmap_alloc`,
  `bitmap_zalloc`, `bitmap_alloc_node`, `bitmap_zalloc_node`, `bitmap_free`),
  conversion (`bitmap_from_arr32/64`, `bitmap_to_arr32/64`), the logical and
  arithmetic operations (`bitmap_and`, `bitmap_or`, `bitmap_xor`,
  `bitmap_andnot`, `bitmap_replace`, `bitmap_complement`, `bitmap_equal`,
  `bitmap_intersects`, `bitmap_subset`), and the bit-scanning and shifting
  helpers (`bitmap_weight*`, `bitmap_find_next_zero_area_off`,
  `bitmap_remap`, `bitmap_bitremap`, `bitmap_shift_left/right`).
* The static inlines in `include/linux/bitmap.h` stay in C and call the
  exported symbols, exactly as the kfifo header does — no header change.
* The upstream KUnit suite (`lib/test_bitmap.c`, `CONFIG_BITMAP_KUNIT_TEST`)
  must pass unchanged.
* A differential suite compiles the unmodified `lib/bitmap.c` under
  test-only names and compares the two implementations over random bitmaps of
  random sizes, with every operation applied to both.

**Prerequisite in the same checkpoint.** `Flags::from_raw()`/`as_raw()`, as
described in section 5, plus the retirement of the kfifo `kmalloc_array()`
helper once the bitmap uses the sanctioned path.

**Risks, stated up front.**

* The API is large; the risk is a *subtle* divergence in the scanning and
  shifting helpers, not a missing function. The differential suite is what
  catches it, and it has to be thorough (random sizes, random bit patterns,
  and the boundary cases the upstream suite already knows about).
* The bitmap is on hot paths in many subsystems; a behavioural regression
  would be serious. That is an argument for the differential suite and for
  the upstream KUnit suite, not against the port.
* `bitmap_alloc()` takes a run-time GFP mask, so the prerequisite is not
  optional.

**Explicitly not in this checkpoint:** the per-CPU family
(`percpu_counter`, `percpu-refcount`, `objagg`), `mempool` (it needs the same
prerequisite but also the waitqueue for its blocking path), `idr` (a good
candidate, but it competes with the bitmap for the same review attention), and
the memory manager itself.

## 6.1 What the port needed (checkpoint 003)

The plan in section 7 was followed, and it confirmed the two gaps of section 5:

* **The run-time allocation mask.** `bitmap_alloc(nbits, flags)` takes the
  caller's GFP mask, so the kernel crate needed `Flags::from_gfp()` before the
  bitmap could be written at all. The same extension let the kernel FIFO drop
  its C helper.
* **The node-aware allocation.** `bitmap_alloc_node()` needs a NUMA node, and
  the kernel crate's `Vec` only allocated on the local node. The allocator
  interface already carries the node, so exposing it (`Vec::reserve_in()`,
  `Vec::with_capacity_in()`) was enough.

The port itself found one real defect, and the *upstream* test suite found it:
`bitmap_cut()` in the C original reads and writes the word that `first` points
at, even when that word is outside the bitmap (the C code then accesses memory
past the allocation). The Rust version only touches the words that are inside
the bitmap; the bits of the bitmap itself are the same. The upstream
`test_bitmap` kselftest, which is built into the test kernel, caught this on
the first boot.

The upstream suite is a strong independent check: **390 447 cases**, all
passing against the Rust implementation.

## 7. Plan

1. Land the `Flags` run-time mask in the kernel crate, with unit tests, and
   retire the kfifo C helper.
2. Investigate whether the C bitmap API is best served by extending
   `kernel::bitmap::OwnedBitmap` or by a new `Bitmap` type with the C API
   shape; write the decision down before writing the port.
3. Port `lib/bitmap.c` to `lib/bitmap_rust.rs`, function by function, with
   the `#[export]` signature check as the ABI gate.
4. Add the differential suite and the upstream KUnit suite to the cycle.
5. Prove the linkage: the C object absent, one definition per symbol, the
   export class preserved, and the sizes compared.
6. Measure the size and the throughput of the bit operations, the way
   checkpoint 003 did, and publish the numbers with the TCG caveat.

## 8. What would change this plan

* If the bitmap turns out to need the waitqueue or the per-CPU API, it moves
  behind `mempool` and `lib/idr.c` becomes the target.
* If the `Flags` change is rejected upstream, the bitmap's allocation
  functions stay behind the C helper and the port covers the operations only —
  which is not a complete object and therefore not a migration.
* If the differential suite shows that the C API's edge cases cannot be
  reproduced faithfully (for example the shifting helpers on 64-bit words
  with sizes above `ULONG_MAX`), the scope is reduced to the parts that can
  be, and the decision is recorded here.

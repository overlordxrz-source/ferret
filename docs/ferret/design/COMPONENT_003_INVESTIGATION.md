# Ferret: substantial-component investigation (checkpoint 002)

Status: the investigation below is the record of the analysis that selected
`lib/kfifo.c`. The component was **implemented in checkpoint 003**
(`lib/kfifo_rust.rs`), and the section "4.1" now describes what was actually
built. The other candidates are unchanged.

The only component migrated in checkpoint 002 is CRC-CCITT; the target below
is the proposed next step, with the evidence needed to plan it honestly.

## 1. Why this document exists

Checkpoint 001 and 002 migrated two leaf CRC implementations. They are real
kernel code and they prove the toolchain, linkage and test story, but they do
not exercise ownership, allocation or synchronization. The checkpoint brief
explicitly asks to investigate a component that does, and to stop drifting into
"trivial utility ports" indefinitely.

This document:

1. defines the selection criteria,
2. audits six candidate components against the actual Rust-for-Linux API that
   exists in this tree (not against what upstream might add later),
3. records exact blockers with the commands used to establish them,
4. recommends an order, and
5. specifies the first slice in enough detail that the next agent can start
   without redoing the analysis.

## 2. Selection criteria

| Criterion | Why it matters |
|---|---|
| Exercises ownership, allocation or synchronization | The point of the exercise |
| Self-contained ABI (few kernel internals) | Keeps a first substantial port reviewable |
| Upstream tests exist | Independent validation, not just self-consistency |
| Rust API prerequisites already exist | Avoids a prerequisite project disguised as a port |
| Replaceable as a whole object | No partial fallback, per PORTING_POLICY |
| Observable from userspace or boot | Evidence that reaches the working system |

## 3. Candidates

LOC = current C source, exports = `EXPORT_SYMBOL*` in that file.

| Component | LOC | Exports | Ownership | Allocation | Synchronization | Upstream tests | Rust prerequisites | Verdict |
|---|---|---|---|---|---|---|---|---|
| `lib/kfifo.c` | 595 | 23 | yes (caller buffer, wraparound) | yes (`__kfifo_alloc`) | no (caller-managed) | `KFIFO_KUNIT_TEST` (224 lines) | all present | **feasible now** |
| `lib/percpu_counter.c` | 407 | 8 | yes (per-cpu blobs) | yes (`alloc_percpu_gfp`) | yes (irqsave spinlock, atomics) | none | **missing per-CPU API** | blocked on prerequisite |
| `lib/sbitmap.c` | 802 | 23 | yes (bitmap + waitqueue) | yes (kvmalloc) | yes (irqsave spinlock, waitqueue) | none | partial (no kvmalloc binding, no waitqueue API) | deferred |
| `lib/ratelimit.c` | 105 | 1 | no | no | yes (irqsave spinlock, trylock, atomics, jiffies) | `RATELIMIT_KUNIT_TEST` | **missing irqsave trylock primitive** | blocked |
| `fs/eventfd.c` | 432 | 2 (GPL) | yes (kref lifetime) | no | yes (waitqueue, spinlock) | kselftest (userspace) | **missing anon-inode fops + poll** | blocked on prerequisite |
| `lib/refcount.c` | 151 | 1 | yes (saturation semantics) | no | atomics | `refcount` KUnit (header-driven) | atomics present | too small to count as substantial |

## 4. Deep dives

### 4.1 `lib/kfifo.c` — recommended next migration

Mechanism: a byte FIFO with a power-of-two size and one slot kept empty so that
`in == out` means empty and `in == out + size` is unreachable. The struct
(`struct __kfifo`) is embedded in caller objects by the `DECLARE_KFIFO` macros;
all functions take `struct __kfifo *` and the buffer is either caller-provided
(`__kfifo_init`) or allocated (`__kfifo_alloc`).

Why it qualifies:

* **Ownership**: the buffer lifetime and the `in`/`out` indices are the entire
  contract; getting aliasing or wrap-around wrong corrupts caller memory.
* **Allocation**: `__kfifo_alloc()` allocates the backing buffer and
  `__kfifo_free()` releases it; both are exported and used by modules.
* **Real consumers**: it is not a toy; drivers and subsystems use it.
* **Independent validation**: `lib/tests/kfifo_kunit.c` (224 lines) exercises
  the public API, and the same differential pattern as CRC-16/CCITT can compile
  the original C file under renamed symbols for randomized comparison.

Rust prerequisites, verified against the generated bindings of this tree:

| Need | Available | Evidence |
|---|---|---|
| Kernel allocation | yes | `kernel::alloc::{KVec, KBox, Flags, Allocator}`; `bindings::krealloc`, `bindings::kfree` |
| User copies | yes | `bindings::_copy_from_user`, `bindings::_copy_to_user`; `kernel::iov` |
| Scatterlist / DMA | yes | `kernel::scatterlist`, `bindings::scatterlist` |
| Atomics | not needed | FIFO indices are non-atomic by contract |
| Locks | not needed | caller-managed |

Shape of the port (`lib/kfifo.c` -> `lib/kfifo_rust.rs`):

1. `#[no_mangle]` / `#[export]` for all 23 exported symbols; the struct layout
   stays the C one (`bindings::__kfifo`), so no ABI change and no header change.
2. Pure index arithmetic in safe Rust on `usize` with explicit power-of-two
   invariants; `unsafe` only at the raw copies and the struct-pointer boundary.
3. `__kfifo_alloc` uses the kernel allocator with the caller's `gfp` flags;
   `__kfifo_free` releases it. No `kvmalloc` fallback is needed for the sizes
   kfifo accepts (power of two, `size * esize`), because the allocation API
   already maps to the kernel allocator.
4. User-copy variants call `_copy_from_user` / `_copy_to_user` and must return
   the same "bytes actually moved" contract, including partial copies.
5. Record mode (`_r` variants) is carried over function by function with the
   framing arithmetic kept identical; this is the highest-risk area and gets
   the randomized differential test first.

Test plan:

* Upstream `KFIFO_KUNIT_TEST` unchanged (it calls the exported functions).
* Ferret differential KUnit suite: compile `lib/kfifo.c` with
  `-D__kfifo_in=__kfifo_in_c_ref` etc. (or a single `-Dkfifo=...` mapping set)
  and compare against Rust over edge cases (empty/full, size-1 buffers,
  wrap-around, record alignment, partial user copies).
* Boot with the Ubuntu userspace image: any driver that uses kfifo at boot is
  an additional, unplanned integration test; the boot markers must stay green.

Risk register:

| Risk | Mitigation |
|---|---|
| Record-mode off-by-one changes framing | Differential test on randomized record streams, byte-for-byte |
| `gfp` flags misused in Rust allocation | Keep the C signature and pass the flags straight through |
| User-copy partial failure semantics | Test with `copy_from_user` fault injection via a bad user pointer |
| Compiler reordering with caller-managed indices | Same non-atomic contract as C; document it, do not add atomics |

### 4.1.1 What was actually built (checkpoint 003)

The plan above was followed, with three adjustments that the implementation
forced:

1. **The allocation flags are a run-time value.** The `kernel` crate's `Flags`
   type only exposes the fixed flag sets, so `__kfifo_alloc()` could not be
   expressed with it. `kmalloc_array()` is reached through a three-line C
   helper (`rust/helpers/slab.c`); the allocation is the kernel's, the FIFO
   logic is Rust's.
2. **`sg_set_buf()` is a static inline without a binding.** It is reached the
   same way (`rust/helpers/scatterlist.c`). The DMA address and length are
   written directly, because `sg_dma_len()` is a macro that selects between
   two struct fields depending on `CONFIG_NEED_SG_DMA_LENGTH`.
3. **The kernel's FFI maps `unsigned long` to `usize`**, not to
   `core::ffi::c_ulong` (`rust/ffi.rs`), and `size_t` is `usize` as well. The
   exported signatures use those types, so `#[export]`'s bindgen signature
   check passes.

What was verified: the upstream `KFIFO_KUNIT_TEST` suite, ten Rust unit tests
and a differential suite (`kfifo_diff`) that compares the Rust implementation
against the unmodified C implementation compiled under test-only names — over
initialisation and allocation (including the error paths), 2 000 randomised
operation sequences, wrap-around, the record API, and the scatterlist setup.
The C object is not built in the Rust variant, all 23 symbols are defined
exactly once by `lib/kfifo_rust.o`, and the non-GPL `EXPORT_SYMBOL` visibility
is preserved through the ksymtab shim.

### 4.2 `lib/percpu_counter.c` — deferred, needs a per-CPU abstraction

Mechanism: per-CPU `s64` counters plus a global approximate sum, flushed when a
CPU's local delta exceeds a batch. Uses `alloc_percpu_gfp`, `free_percpu`,
`this_cpu_add`, `for_each_possible_cpu`, an irqsave spinlock, and an atomic64.

Why it is attractive: it is pure ownership (the per-CPU blob is owned by the
counter), allocation, and synchronization in one small file, with 8 exports.

Blocker, established by inspection:

* `rust/kernel/` has **no per-CPU module**: no `alloc_percpu`, no
  `this_cpu_ptr`, no address-space-tagged pointer type. A grep for
  `alloc_percpu` over `rust/kernel/` returns nothing.
* `this_cpu_*` are per-architecture inline asm; a Rust port needs an arch
  abstraction with a documented safety contract, which is a project of its own
  and exactly the kind of prerequisite that must not be smuggled into a port.

Plan if promoted: first land `kernel::percpu` (allocation, per-CPU access,
`ThisCpu` guard) with its own tests; then port `percpu_counter` with a
differential suite. That is a checkpoint-004 sized body of work, not a slice.

### 4.3 `fs/eventfd.c` — deferred, needs an anon-inode file-operations layer

Mechanism: `eventfd_ctx` with `kref`, a waitqueue used as both lock and wakeup
source, `anon_inode_getfile()` to create the fd, `poll` support, and
`eventfd_signal()` used by other kernel subsystems.

Blockers, established by inspection:

* Rust `miscdevice` supports `open/release/read_iter/write_iter/ioctl` only; it
  has no `poll` and no `anon_inode_getfile` path, and anon-inode files cannot be
  registered as misc devices.
* `eventfd_signal` and `eventfd_ctx_put` are `EXPORT_SYMBOL_GPL` and are called
  from C subsystems; replacing the object must keep those symbols and their
  exact semantics.
* `poll_wait` integration and wakeup ordering are the whole point of the
  component, so a "port" that reuses the C waitqueue helpers would not be a
  Rust implementation of eventfd.

Plan if promoted: add a `kernel::fs::anon_inode` + `FileOps` trait with `poll`
to the Rust abstraction layer (upstream work), then port eventfd with a
userspace kselftest (`tools/testing/selftests/eventfd`) run inside the Ubuntu
guest. This is the right showcase target once the fops layer exists.

### 4.4 `lib/ratelimit.c` — blocked on a binding, despite looking easy

The 6.18 implementation is no longer the simple "count and jiffies" function:
it uses `raw_spin_trylock_irqsave`, an atomic `rs_n_left`, `printk_deferred`,
and a two-counter contention path.

Binding audit (generated `rust/bindings/bindings_generated.rs`):

* present: `_raw_spin_lock_irqsave`, `_raw_spin_unlock_irqrestore`,
  `_raw_spin_trylock`, `jiffies`, `_printk_deferred`, `atomic_*` helpers.
* missing: `_raw_spin_trylock_irqsave`. The symbol does not exist in
  `vmlinux` either (`nm` finds only `_raw_spin_lock_irqsave`), because it is
  composed from a trylock plus an IRQ-save macro that Rust cannot expand.

A port that silently replaces trylock with a blocking lock would change the
contention behavior the comments in the file explicitly reason about. That is
not acceptable for a migration whose contract is "same behavior". Options:
provide an arch-level `irqsave trylock` primitive in the Rust abstraction layer
(with a C shim per architecture, documented as a primitive rather than an
algorithm), or defer. Deferred.

## 5. Recommendation

| Checkpoint | Target | Why |
|---|---|---|
| 003 | `lib/kfifo.c` | Ownership + allocation, upstream KUnit, all Rust prerequisites exist, no lock/per-CPU prerequisites |
| 004 | `kernel::percpu` then `lib/percpu_counter.c` | The first genuinely concurrent, allocating component; requires the per-CPU abstraction first |
| 005 | `kernel::fs::FileOps` (+ poll) then `fs/eventfd.c` | Userspace-visible, kselftest-validated; requires the fops layer first |
| later | `lib/sbitmap.c` | Block-layer core; waitqueue + allocation, larger blast radius |

This ordering deliberately puts the *prerequisite* work in the open instead of
pretending a port is possible when the abstraction is missing.

## 6. First slice for `lib/kfifo.c` (checkpoint 003 start)

1. `lib/kfifo/Kconfig` (or `lib/Kconfig` next to `KFIFO`): add
   `config KFIFO_RUST` (`depends on KFIFO = y && RUST`) and
   `config KFIFO_DIFF_KUNIT_TEST`.
2. `lib/Makefile`: `ifeq ($(CONFIG_KFIFO_RUST),y)` -> `obj-y += kfifo_rust.o`,
   else the existing `obj-$(CONFIG_KFIFO) += kfifo.o`.
3. `lib/kfifo_rust.rs`: all 23 exported entry points, `#[export]`, struct
   pointer ABI unchanged.
4. `lib/kfifo_export.c`: ksymtab entries only, same pattern as CRC-16/CCITT
   (the Rust `#[export]` macro is GPL-only for the `kernel` crate, D-004).
5. `lib/kfifo_diff_kunit.c`: original C compiled under renamed symbols, plus
   the upstream `KFIFO_KUNIT_TEST` left untouched.
6. Boot the Ubuntu image; kfifo users at boot are extra coverage.

Exit criteria: clean build with no `kfifo.o`, one definition per symbol, size
match against the Rust object, all upstream and differential tests pass, Ubuntu
shell + systemd boots still pass, registry and report updated.

## 7. Evidence appendix

Commands used for the binding audit (run against the checkpoint-002 build tree):

```sh
B=/build/linux-rust/rust/bindings/bindings_generated.rs
grep -oE 'fn _raw_spin_[a-z_]+' "$B" | sort -u
nm /build/linux-rust/vmlinux | grep -E ' _raw_spin_(try)?lock_irqsave$'
grep -rn 'alloc_percpu' rust/kernel/
grep -n 'fn read_iter\|fn write_iter\|fn poll\|fn ioctl' rust/kernel/miscdevice.rs
```

Observed: `_raw_spin_lock_irqsave` and `_raw_spin_unlock_irqrestore` present;
`_raw_spin_trylock_irqsave` absent from both bindings and `vmlinux`;
`alloc_percpu` absent from `rust/kernel/`; `miscdevice` has no `poll` and no
anon-inode path; `_copy_from_user`/`_copy_to_user`/`kfree`/`krealloc` present.

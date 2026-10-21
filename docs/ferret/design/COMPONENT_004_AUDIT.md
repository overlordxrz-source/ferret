# Ferret: audit of the kernel bitmap port (checkpoint 004)

Status: the audit of the Rust bitmap port on the `ferret/004-bitmap` branch.
Every claim here is backed by a measurement or by a source reference; the
places where the Rust code deliberately differs from the C are listed
explicitly.

## 1. Method

* Every `unsafe` block in `lib/bitmap_rust.rs` was read against the
  corresponding C statement in `lib/bitmap.c`.
* The boundary conditions were derived from the C code and the callers:
  zero-length bitmaps, bit counts at and around the word boundary, the tail
  bits, overlapping buffers, allocation failures and the overflow of the
  `kmalloc_array()` multiplication.
* The upstream `test_bitmap` kselftest module (390 447 cases) and the
  differential suite are the executable evidence; the audit explains what
  those tests do *not* cover.

## 2. The `Words` abstraction

```rust
struct Words { ptr: *const usize, len: usize }      // read-only
struct WordsMut { ptr: *mut usize, len: usize }    // read-write
```

The C interface passes a word array and a bit count separately, so the Rust code
carries both. The invariant is:

> `len` is `bits_to_longs(bits)`, the number of words the bitmap occupies; the
> buffer is valid for `len` words; every index used below is provably below
> `len`.

The length is *derived*, never taken from the caller, so a wrong length cannot
be injected. The read-only and read-write views are separate types, so a
source bitmap can never be written through: the C code takes `const unsigned
long *` for the sources, and the Rust code keeps that guarantee in the type
system rather than in a comment.

The accesses inside the views are unchecked. That is sound *because* the index
is always provably below `len`:

| Access | Bound |
|---|---|
| `weight()`: words `0..bits/64` and the tail word | `bits/64 <= len`, and the tail word exists only when `bits % 64 != 0`, i.e. `bits/64 < len` |
| `find_next_bit()` / `find_next_zero_bit()` | the scan starts at `offset/64 <= (size-1)/64` and only advances while `(idx+1)*64 < size`, so `idx < len` |
| `find_nth_bit()` | the loop covers the words fully inside the bitmap (`idx < size/64`), and the tail word is `size/64 < len` |
| `set_bit()` / `test_bit()` | `nr < bits`, so `nr/64 < len` |
| `zero()` | `0..bits_to_longs(bits)` |
| `__bitmap_set()` / `__bitmap_clear()` | the view is sized by `start + len`, and the last word touched is `(start + len - 1)/64` |

`debug_assert!` checks the bound in debug builds; in release builds the bound
follows from the bit count, which is why an unchecked access cannot go out of
bounds.

### Zero-length bitmaps

`bits_to_longs(0) == 0`, so the view is empty. Every function handles that
without touching the buffer: `find_next_bit()` and `find_next_zero_bit()`
return `size` immediately, `weight()` and `find_nth_bit()` loop zero times,
`zero()` writes nothing, and the logical operations write nothing. The C code
behaves the same way (its loops are bounded by `bits / BITS_PER_LONG`).

### Overflow

* `bits_to_longs()` is computed in `usize`, so it cannot overflow for a `u32`
  bit count.
* `last_word_mask()` uses `nbits.wrapping_neg()`, which is the C's
  `-(nbits) & (BITS_PER_LONG - 1)` for an unsigned value ✓.
* `__bitmap_set()` / `__bitmap_clear()` compute `start + len` with
  `wrapping_add()` ✓ (the C wraps too, in `unsigned int`).
* The allocation multiplies the word count by the word size with
  `checked_mul()` and fails like `kmalloc_array()` does ✓.

### Aliasing

The C code allows `dst` to alias a source. The Rust code keeps that:

* the logical operations read the operands and write the result for the same
  index, in that order — the C does `result |= (dst[k] = b1[k] & b2[k])`, and
  the Rust does the same read-then-write ✓;
* `bitmap_cut()` copies with `memmove()` semantics (`core::ptr::copy`) before
  it modifies the destination ✓, so partially overlapping buffers work;
* `bitmap_remap()`, `bitmap_onto()` and `bitmap_fold()` return early when
  `dst == src`, exactly as the C does (the C does not handle in-place
  mappings) ✓.

### Allocation failure

`bitmap_alloc()` returns null when the allocation fails ✓, and
`bitmap_zalloc()` adds `__GFP_ZERO` ✓. `devm_bitmap_alloc()` registers the
free action with `devm_add_action_or_reset()`, which releases the bitmap if the
registration fails — the same contract as the C ✓. `bitmap_free()` uses
`kfree()`, which is the matching release for the `kmalloc()` family that the
kernel crate's allocator uses ✓.

## 3. Every unsafe access, classified

| Access | Why it is sound |
|---|---|
| `&mut *fifo`-style pointer dereferences (the FFI entry points) | the C contract: the caller passes a valid `struct __kfifo` |
| `Words::new()` | the buffer is valid for `bits_to_longs(bits)` words (the C contract) |
| `Words::get()` / `WordsMut::get()` / `WordsMut::set()` | the index is provably below `len` (section 2) |
| `core::ptr::copy(src, dst, len)` in `bitmap_cut()` | `memmove()` semantics; both buffers are valid for `len` words |
| `from_raw_parts()` for the `u32`/`u64` conversion buffers | the caller guarantees `DIV_ROUND_UP(nbits, 32)` (or 64) words |
| `bindings::kfree()` | the pointer came from `bitmap_alloc()` and is freed once |
| `bindings::devm_add_action_or_reset()` | the device is valid and bound; the callback frees the bitmap once |
| `bindings::hweight64()` | a pure function of its argument |

There is no `static mut`, no allocation in the hot path, no `unsafe` trait
implementation and no panic path in the exported functions.

## 4. Where the C code is undefined and the Rust code is not

The port preserves the *documented* behaviour of the C code and refuses to
reproduce its undefined behaviour. There are exactly three such places, and
all three were found by the upstream test suite or by the audit:

1. **`bitmap_cut()` with `first` outside the bitmap.** The C code reads
   `src[first / BITS_PER_LONG]` and writes `dst[first / BITS_PER_LONG]`
   unconditionally, even when that word is outside the bitmap. With
   `first == nbits` the word is one past the end of the allocation.
   *This is an upstream C bug, not a contract violation by the caller*: the
   upstream test suite itself exercises it
   (`{ BITS_PER_LONG, BITS_PER_LONG, BITS_PER_LONG, ... }` in
   `lib/test_bitmap.c`, with `first == nbits`), and it only does not crash
   because the test's buffer is larger than the bitmap. The Rust version only
   touches the words that are inside the bitmap; the bits of the bitmap itself
   are identical, which the differential suite checks
   (`bitmap_diff_cut_out_of_range`).
2. **The shifts with a shift larger than the bitmap.** The C code computes
   `lim - off` in unsigned arithmetic, where `off > lim` wraps, and then
   `memset()`s a huge area outside the bitmap. The Rust version clamps the
   clearing to the bitmap; the bits of the bitmap are the same.
3. **`bitmap_fold()` with a size of zero.** The C code divides by zero (a
   kernel oops). The Rust version leaves the bitmap cleared instead.

In addition, two C behaviours are *not* reproduced because they are
unreachable in a well-formed program but reachable in Rust's type system as
undefined behaviour:

* `bitmap_remap()` writes the bit that `find_nth_bit()` returns. If there were
  no such bit, the C would write outside the bitmap; the Rust version checks
  the result before writing ✓ (the bit always exists in practice, because the
  ordinal is below the weight of `new`).
* `__bitmap_set()` / `__bitmap_clear()` with a negative length: the C computes
  `start + len` in unsigned arithmetic and writes a word with a garbage mask.
  The Rust version treats a negative length as zero ✓.

## 5. Memory ordering

The bitmap has no concurrency: the C implementation has no atomics, no
barriers and no locks, and the locking contract is the caller's (the same
contract as the kernel FIFO, see `RUST_SAFETY.md`). The port adds none either.

The barrier question was settled during the kernel FIFO review, and the same
answer applies here: the port calls the kernel's own `smp_wmb()` primitive
(`kernel::sync::barrier::smp_wmb()`), never a Rust release fence, because the
two are not the same instruction on arm64 (`dmb ishst` versus `dmb ish`).

## 6. The GFP and NUMA additions

`kernel::alloc::Flags::from_gfp()` / `as_gfp()` and
`kernel::alloc::Vec::reserve_in()` / `with_capacity_in()` were added to the
in-tree kernel crate. They expose primitives that already exist in C
(`gfp_t` is an `unsigned int`; the allocator interface already carried the
NUMA node), and they are verified by:

* the kernel FIFO, which now allocates through
  `KVec::with_capacity(total, Flags::from_gfp(gfp_mask))` and passes its
  allocation and free tests (`kfifo_rust_alloc_and_free`, `kfifo_diff_alloc`);
* the bitmap, which allocates with the caller's flags
  (`bitmap_alloc(nbits, flags)`) and on a node (`bitmap_alloc_node()`), both
  compared against the C reference in `bitmap_diff_alloc` and
  `bitmap_diff_alloc_node`.

The C helper that existed only because the GFP mask could not be expressed
(`rust_helper_kmalloc_array` in `rust/helpers/slab.c`) is **removed**: the
kernel FIFO no longer needs it, and the bitmap never did.

## 7. 32-bit support: NOT TESTED

`bitmap_from_arr64()` and `bitmap_to_arr64()` are implemented (they are the
32-bit counterparts of the 32-bit-word conversions) and are compiled only on a
32-bit target. Ferret builds and tests 64-bit targets only, so those two
functions are **not compiled, not linked and not tested** in this checkpoint.
The 64-bit counterparts (`bitmap_from_arr32()`, `bitmap_to_arr32()`) are
covered by the differential suite.

The 32-bit path also has a build-system consequence: on a 64-bit build
`bitmap_from_arr64()` and `bitmap_to_arr64()` are macros in
`include/linux/bitmap.h`, so the differential test cannot rename them (the
rename would redefine the macro). The Makefile therefore applies those two
renames only when `CONFIG_64BIT` is not set.

## 8. Performance

The measurement is the `bitmap_diff_benchmark` case, which runs the same
workload on the Rust implementation and on the C reference in the same kernel
and the same run, keeping the best of three rounds.

| Workload | Rust | C | ratio |
| --- | --- | --- | --- |
| all operations (2 000 iterations over 1 024 bits) | 5.72 ms | 4.31 ms | **1 327 per mille** |
| without the weight helpers | 4.18 ms | 4.73 ms | **884 per mille** |

The reading is unambiguous: the logical operations, the set/clear ranges and
the shifts are at parity (slightly faster in Rust), and the whole gap comes
from the **weight helpers**.

The cause is architectural, not algorithmic. The C `hweight64()` is a macro
over the architecture's `__arch_hweight64()`, which uses the *alternatives*
mechanism: the generic software popcount is patched to the `popcnt`
instruction at boot. That patch cannot be applied to Rust code — Rust for
Linux has no equivalent of the alternatives mechanism for Rust translation
units. A Rust `count_ones()` is therefore compiled to the software sequence,
and calling the kernel's `hweight64()` through a helper costs a call per word.

Measured variants: the software sequence (`count_ones()`) gives 1 590 per
mille, the helper call gives 1 327 per mille; the helper is kept.

**Caveat on the numbers themselves:** they are QEMU TCG figures (the test
kernel runs under an instruction-level emulator), and their run-to-run spread
is large — the upstream kselftest, which measures *identical* header code in
both builds, varied by about 30 % between two boots of the same kernel. Only
the ratio between the two implementations, measured in the same run, is
meaningful.

## 9. Code size

| | C | Rust | Δ |
| --- | --- | --- | --- |
| the 29 exported symbols (x86_64) | 3 712 B | 5 807 B | +2 095 B (+56 %) |

The absolute cost is 0.004 % of the 57 MB kernel. The difference is in the
per-bit helpers (`bitmap_remap()`, `bitmap_bitremap()`,
`bitmap_find_next_zero_area_off()` and the weight helpers): the C expands
the scanning macros inline at every call site, while the Rust code calls a
helper that is inlined at the same places but carries the extra argument
setup. The first version of the port, which used checked slice accesses
everywhere, was +85 %; the panic paths of the checked accesses were a large
part of that.

## 10. What the tests do not cover

* The 32-bit variants (section 7).
* The bitmap's behaviour on a 32-bit word size in general.
* The user-copy and DMA paths of the *other* components; the bitmap has
  neither.
* Native performance: every number above is a TCG number.

// SPDX-License-Identifier: GPL-2.0-only

//! Ferret: the kernel bitmap, implemented in Rust.
//!
//! Replaces `lib/bitmap.c`. The C ABI in `include/linux/bitmap.h` is
//! unchanged: the same exported symbols with the same signatures, the same
//! `EXPORT_SYMBOL` visibility, and no change to the header or to the inline
//! helpers that call into the exported functions.
//!
//! # Model
//!
//! A bitmap is an array of machine words (`unsigned long`, i.e. `usize` here)
//! holding a number of bits. The number of valid bits does not have to be a
//! multiple of the word size; the bits above the valid range in the last word
//! are "don't care", exactly as in the C original. Every function that
//! returns a boolean or a count filters those tail bits out.
//!
//! The bit numbering is the same as in C: bit `n` is bit `n % BITS_PER_LONG`
//! of word `n / BITS_PER_LONG`, counted from the least significant bit. The
//! scanning helpers (`find_next_bit()` and friends) reproduce the semantics of
//! the C `FIND_NEXT_BIT`/`FIND_FIRST_BIT`/`FIND_NTH_BIT` macros, including
//! their treatment of the bits above the valid range.
//!
//! # Aliasing
//!
//! The C functions are allowed to alias their arguments: `dst` may be the same
//! pointer as one of the sources, and `bitmap_cut()` explicitly supports
//! partially overlapping buffers. The Rust code keeps that behaviour: the
//! operands are read before the result is written for the same index, which is
//! what the C code does, and `bitmap_cut()` uses a move, not a copy.
//!
//! # Differences from the C original
//!
//! Two, both in cases where the C code has undefined behaviour:
//!
//! * `__bitmap_shift_left()` and `__bitmap_shift_right()` with a shift larger
//!   than the bitmap make the C code compute a word count that underflows and
//!   then clear memory outside the bitmap. The Rust code clamps the clearing
//!   to the bitmap.
//! * The number of words is computed in 64-bit arithmetic, where the C code
//!   computes it in 32-bit `unsigned int` and wraps for a bit count above
//!   2^32 - 64. No caller can reach that (the allocation would fail long
//!   before), and the Rust code cannot wrap.

use core::ffi::{c_int, c_uint, c_void};

use kernel::alloc::{Flags, KVec, NumaNode};
use kernel::bindings;
use kernel::prelude::*;

/// The number of bits in a machine word (`BITS_PER_LONG`).
const BITS_PER_LONG: u32 = usize::BITS;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// The words of a bitmap.
///
/// The C interface passes the word array and the number of bits separately,
/// so this carries both. The length is the *validated* view of the buffer: it
/// is derived from the bit count, and every access below is inside it. The
/// accesses themselves do not check the bounds again, because the index is
/// always provably below the length (the bit count is what defines it), and a
/// check would only add a panic path to a kernel that must not panic.
struct Words {
    ptr: *mut usize,
    len: usize,
}

impl Words {
    /// The words of a bitmap.
    ///
    /// # Safety
    ///
    /// `ptr` must be valid for `bits_to_longs(bits)` words, and the caller
    /// must keep the buffer alive for as long as the view is used.
    #[inline]
    unsafe fn new(ptr: *mut usize, bits: u32) -> Self {
        Words {
            ptr,
            len: bits_to_longs(bits),
        }
    }

    /// The number of words of the bitmap.
    #[inline]
    fn len(&self) -> usize {
        self.len
    }

    /// The `i`-th word of the bitmap.
    ///
    /// The index must be below [`Words::len()`].
    #[inline]
    fn get(&self, i: usize) -> usize {
        debug_assert!(i < self.len);

        // SAFETY: the caller guarantees that the buffer is valid for `len`
        // words, and `i` is below `len` (checked above in debug builds and
        // provable from the bit count in release builds).
        unsafe { *self.ptr.add(i) }
    }

    /// Set the `i`-th word of the bitmap.
    ///
    /// The index must be below [`Words::len()`].
    #[inline]
    fn set(&self, i: usize, value: usize) {
        debug_assert!(i < self.len);

        // SAFETY: as in `Words::get()`.
        unsafe { *self.ptr.add(i) = value }
    }
}

/// The number of words needed to hold `nbits` bits.
#[inline]
fn bits_to_longs(nbits: u32) -> usize {
    (nbits as usize).div_ceil(BITS_PER_LONG as usize)
}

/// The mask of the valid bits in the last word of a `nbits`-bit bitmap.
///
/// This is `BITMAP_LAST_WORD_MASK()`: all bits are valid when the bit count
/// is a multiple of the word size.
#[inline]
fn last_word_mask(nbits: u32) -> usize {
    usize::MAX >> (nbits.wrapping_neg() & (BITS_PER_LONG - 1))
}

/// The mask of the bits of the first word, from `start` onwards.
///
/// This is `BITMAP_FIRST_WORD_MASK()`.
#[inline]
fn first_word_mask(start: u32) -> usize {
    usize::MAX << (start & (BITS_PER_LONG - 1))
}

/// The position of the `n`-th set bit in a word, counted from zero.
///
/// This is the C `fns()`: it returns `BITS_PER_LONG` when the word does not
/// have that many set bits.
#[inline]
fn fns(word: usize, n: u32) -> u32 {
    let mut word = word;
    let mut n = n;

    while word != 0 && n > 0 {
        word &= word - 1;
        n -= 1;
    }

    if word != 0 {
        word.trailing_zeros()
    } else {
        BITS_PER_LONG
    }
}

/// Find the next set bit at or after `offset`; returns `size` if there is none.
///
/// This is the Rust equivalent of the C `find_next_bit()`, which is built on
/// the `FIND_NEXT_BIT()` macro: the search starts in the word that contains
/// `offset`, masked to the bits from `offset` onwards, and continues with the
/// following words. The last word is *not* masked: the result is clamped to
/// `size` instead, exactly as in the C macro.
#[inline]
fn find_next_bit(words: &Words, size: u32, offset: u32) -> u32 {
    if offset >= size {
        return size;
    }

    let mut idx = (offset / BITS_PER_LONG) as usize;
    let mut mask = first_word_mask(offset);

    loop {
        let tmp = words.get(idx) & mask;
        if tmp != 0 {
            let bit = (idx as u32) * BITS_PER_LONG + tmp.trailing_zeros();
            return bit.min(size);
        }

        if (idx as u32 + 1) * BITS_PER_LONG >= size {
            return size;
        }

        idx += 1;
        mask = usize::MAX;
    }
}

/// Find the next clear bit at or after `offset`; returns `size` if there is
/// none.
///
/// This is the Rust equivalent of the C `find_next_zero_bit()`, which is the
/// same search with the bits inverted.
#[inline]
fn find_next_zero_bit(words: &Words, size: u32, offset: u32) -> u32 {
    if offset >= size {
        return size;
    }

    let mut idx = (offset / BITS_PER_LONG) as usize;
    let mut mask = first_word_mask(offset);

    loop {
        let tmp = !words.get(idx) & mask;
        if tmp != 0 {
            let bit = (idx as u32) * BITS_PER_LONG + tmp.trailing_zeros();
            return bit.min(size);
        }

        if (idx as u32 + 1) * BITS_PER_LONG >= size {
            return size;
        }

        idx += 1;
        mask = usize::MAX;
    }
}

/// Find the `n`-th set bit, counted from zero; returns `size` if there is
/// none.
///
/// This is the Rust equivalent of the C `find_nth_bit()`, which is built on
/// the `FIND_NTH_BIT()` macro: the words that are fully inside the bitmap are
/// counted first, and the last, partial word is masked.
#[inline]
fn find_nth_bit(words: &Words, size: u32, n: u32) -> u32 {
    let mut nr = n;
    let mut idx = 0;

    while (idx as u32 + 1) * BITS_PER_LONG <= size {
        if (idx as u32) * BITS_PER_LONG + nr >= size {
            return size;
        }

        let tmp = words.get(idx);
        let w = tmp.count_ones();
        if w > nr {
            return (idx as u32) * BITS_PER_LONG + fns(tmp, nr);
        }

        nr -= w;
        idx += 1;
    }

    if size % BITS_PER_LONG != 0 {
        let tmp = words.get(idx) & last_word_mask(size);
        return (idx as u32) * BITS_PER_LONG + fns(tmp, nr);
    }

    size
}

/// The number of set bits in a bitmap, ignoring the bits above `bits`.
///
/// This is the C `BITMAP_WEIGHT()` macro.
#[inline]
fn weight(words: &Words, bits: u32) -> u32 {
    let lim = (bits / BITS_PER_LONG) as usize;
    let mut w = 0;

    for k in 0..lim {
        w += words.get(k).count_ones();
    }

    if bits % BITS_PER_LONG != 0 {
        w += (words.get(lim) & last_word_mask(bits)).count_ones();
    }

    w
}

/// Clear `bits` bits of a bitmap.
///
/// This is the C `bitmap_zero()`.
#[inline]
fn zero(words: &Words, bits: u32) {
    let len = bits_to_longs(bits);

    for k in 0..len {
        words.set(k, 0);
    }
}

/// Set one bit.
#[inline]
#[inline]
fn set_bit(words: &Words, nr: u32) {
    let idx = (nr / BITS_PER_LONG) as usize;

    words.set(idx, words.get(idx) | (1 << (nr % BITS_PER_LONG)));
}

/// Test one bit.
#[inline]
#[inline]
fn test_bit(words: &Words, nr: u32) -> bool {
    let idx = (nr / BITS_PER_LONG) as usize;

    words.get(idx) & (1 << (nr % BITS_PER_LONG)) != 0
}

/// Combine a caller's flags with the kernel's `__GFP_ZERO`.
#[inline]
fn with_zero(flags: bindings::gfp_t) -> bindings::gfp_t {
    (Flags::from_gfp(flags) | kernel::alloc::flags::__GFP_ZERO).as_gfp()
}

// ---------------------------------------------------------------------------
// Exported API
// ---------------------------------------------------------------------------

/// Compare two bitmaps for equality.
///
/// C: `bool __bitmap_equal(const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// Both pointers must be valid for `bits_to_longs(bits)` words.
#[export]
pub unsafe extern "C" fn __bitmap_equal(
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) -> bool {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b1, b2) = unsafe {
        (
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };
    let lim = (bits / BITS_PER_LONG) as usize;

    for k in 0..lim {
        if b1.get(k) != b2.get(k) {
            return false;
        }
    }

    if bits % BITS_PER_LONG != 0 && (b1.get(lim) ^ b2.get(lim)) & last_word_mask(bits) != 0 {
        return false;
    }

    true
}

/// Compare the union of two bitmaps with a third one.
///
/// C: `bool __bitmap_or_equal(const unsigned long *bitmap1, const unsigned long *bitmap2, const unsigned long *bitmap3, unsigned int bits)`
///
/// This symbol is not exported by the C original either; the header's inline
/// `bitmap_or_equal()` calls it.
///
/// # Safety
///
/// All three pointers must be valid for `bits_to_longs(bits)` words.
#[no_mangle]
pub unsafe extern "C" fn __bitmap_or_equal(
    bitmap1: *const usize,
    bitmap2: *const usize,
    bitmap3: *const usize,
    bits: c_uint,
) -> bool {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b1, b2, b3) = unsafe {
        (
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
            Words::new(bitmap3 as *mut usize, bits),
        )
    };
    let lim = (bits / BITS_PER_LONG) as usize;

    for k in 0..lim {
        if (b1.get(k) | b2.get(k)) != b3.get(k) {
            return false;
        }
    }

    if bits % BITS_PER_LONG == 0 {
        return true;
    }

    let tmp = (b1.get(lim) | b2.get(lim)) ^ b3.get(lim);

    tmp & last_word_mask(bits) == 0
}

/// Invert a bitmap.
///
/// C: `void __bitmap_complement(unsigned long *dst, const unsigned long *src, unsigned int bits)`
///
/// # Safety
///
/// `dst` must be valid for `bits_to_longs(bits)` words and writable.
#[export]
pub unsafe extern "C" fn __bitmap_complement(
    dst: *mut usize,
    src: *const usize,
    bits: c_uint,
) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, s) = unsafe {
        (
            Words::new(dst, bits),
            Words::new(src as *mut usize, bits),
        )
    };

    for k in 0..d.len() {
        d.set(k, !s.get(k));
    }
}

/// Shift a bitmap to the right.
///
/// C: `void __bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned shift, unsigned nbits)`
///
/// # Safety
///
/// `dst` and `src` must be valid for `bits_to_longs(nbits)` words.
#[export]
pub unsafe extern "C" fn __bitmap_shift_right(
    dst: *mut usize,
    src: *const usize,
    shift: c_uint,
    nbits: c_uint,
) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, s) = unsafe {
        (
            Words::new(dst, nbits),
            Words::new(src as *mut usize, nbits),
        )
    };

    let lim = d.len();
    let off = (shift / BITS_PER_LONG) as usize;
    let rem = shift % BITS_PER_LONG;
    let mask = last_word_mask(nbits);

    let mut k = 0;
    while off + k < lim {
        let upper = if rem == 0 || off + k + 1 >= lim {
            0
        } else {
            let mut upper = s.get(off + k + 1);
            if off + k + 1 == lim - 1 {
                upper &= mask;
            }
            upper << (BITS_PER_LONG - rem)
        };

        let mut lower = s.get(off + k);
        if off + k == lim - 1 {
            lower &= mask;
        }
        lower >>= rem;

        d.set(k, lower | upper);
        k += 1;
    }

    if off > 0 {
        // The C code clears the last `off` words with a `memset()`; if the
        // shift is larger than the bitmap, the word count underflows there.
        // Here the clearing is clamped to the bitmap.
        let start = lim.saturating_sub(off);
        for k in start..lim {
            d.set(k, 0);
        }
    }
}

/// Shift a bitmap to the left.
///
/// C: `void __bitmap_shift_left(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits)`
///
/// # Safety
///
/// `dst` and `src` must be valid for `bits_to_longs(nbits)` words.
#[export]
pub unsafe extern "C" fn __bitmap_shift_left(
    dst: *mut usize,
    src: *const usize,
    shift: c_uint,
    nbits: c_uint,
) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, s) = unsafe {
        (
            Words::new(dst, nbits),
            Words::new(src as *mut usize, nbits),
        )
    };

    let lim = d.len();
    let off = (shift / BITS_PER_LONG) as usize;
    let rem = shift % BITS_PER_LONG;

    if off < lim {
        for k in (0..lim - off).rev() {
            let lower = if rem != 0 && k > 0 {
                s.get(k - 1) >> (BITS_PER_LONG - rem)
            } else {
                0
            };

            d.set(k + off, lower | (s.get(k) << rem));
        }
    }

    if off > 0 {
        // As in the right shift, the C code clears `off` words with a
        // `memset()`, which writes outside the bitmap when the shift is
        // larger than the bitmap. Here the clearing is clamped.
        let start = core::cmp::min(off, lim);
        for k in 0..start {
            d.set(k, 0);
        }
    }
}

/// Remove a region of bits and shift the rest down.
///
/// C: `void bitmap_cut(unsigned long *dst, const unsigned long *src, unsigned int first, unsigned int cut, unsigned int nbits)`
///
/// `dst` and `src` may overlap.
///
/// # Safety
///
/// `dst` and `src` must be valid for `bits_to_longs(nbits)` words.
#[export]
pub unsafe extern "C" fn bitmap_cut(
    dst: *mut usize,
    src: *const usize,
    first: c_uint,
    cut: c_uint,
    nbits: c_uint,
) {
    let len = bits_to_longs(nbits);
    let first_word = (first / BITS_PER_LONG) as usize;
    let first_bit = first % BITS_PER_LONG;

    // The bits of the first word that are below `first` are kept aside and
    // restored at the end.
    //
    // When `first` is at or past the end of the bitmap, the word it points at
    // is outside the bitmap; the C original reads and writes it anyway (the
    // bitmap only has `len` words), which is an out-of-bounds access. Here
    // the word is only touched when it is inside the bitmap, and the bits of
    // the bitmap itself are the same either way.
    let keep = if first_bit != 0 && first_word < len {
        // SAFETY: the caller guarantees the buffer is valid.
        let s = unsafe { Words::new(src as *mut usize, nbits) };
        s.get(first_word) & (usize::MAX >> (BITS_PER_LONG - first_bit))
    } else {
        0
    };

    // The C code uses a `memmove()`, so the destination may overlap the
    // source.
    // SAFETY: the caller guarantees that both buffers are valid for `len`
    // words; `copy()` has the semantics of `memmove()` and tolerates
    // overlapping buffers.
    unsafe {
        core::ptr::copy(src, dst, len);
    }

    // SAFETY: the caller guarantees the buffer is valid.
    let d = unsafe { Words::new(dst, nbits) };

    let mut cut = cut;
    while cut > 0 {
        for i in first_word..len {
            let carry = if i < len - 1 { d.get(i + 1) & 1 } else { 0 };
            d.set(i, (d.get(i) >> 1) | (carry << (BITS_PER_LONG - 1)));
        }
        cut -= 1;
    }

    if first_word < d.len() {
        d.set(first_word, d.get(first_word) & (usize::MAX << first_bit));
        d.set(first_word, d.get(first_word) | keep);
    }
}

/// The intersection of two bitmaps.
///
/// C: `bool __bitmap_and(unsigned long *dst, const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// `dst` must be valid for `bits_to_longs(bits)` words and writable.
#[export]
pub unsafe extern "C" fn __bitmap_and(
    dst: *mut usize,
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) -> bool {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, b1, b2) = unsafe {
        (
            Words::new(dst, bits),
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };
    let lim = (bits / BITS_PER_LONG) as usize;
    let mut result = 0usize;

    for k in 0..lim {
        let v = b1.get(k) & b2.get(k);
        d.set(k, v);
        result |= v;
    }

    if bits % BITS_PER_LONG != 0 {
        let v = b1.get(lim) & b2.get(lim) & last_word_mask(bits);
        d.set(lim, v);
        result |= v;
    }

    result != 0
}

/// The union of two bitmaps.
///
/// C: `void __bitmap_or(unsigned long *dst, const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// `dst` must be valid for `bits_to_longs(bits)` words and writable.
#[export]
pub unsafe extern "C" fn __bitmap_or(
    dst: *mut usize,
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, b1, b2) = unsafe {
        (
            Words::new(dst, bits),
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };

    for k in 0..d.len() {
        d.set(k, b1.get(k) | b2.get(k));
    }
}

/// The symmetric difference of two bitmaps.
///
/// C: `void __bitmap_xor(unsigned long *dst, const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// `dst` must be valid for `bits_to_longs(bits)` words and writable.
#[export]
pub unsafe extern "C" fn __bitmap_xor(
    dst: *mut usize,
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, b1, b2) = unsafe {
        (
            Words::new(dst, bits),
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };

    for k in 0..d.len() {
        d.set(k, b1.get(k) ^ b2.get(k));
    }
}

/// The difference of two bitmaps.
///
/// C: `bool __bitmap_andnot(unsigned long *dst, const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// `dst` must be valid for `bits_to_longs(bits)` words and writable.
#[export]
pub unsafe extern "C" fn __bitmap_andnot(
    dst: *mut usize,
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) -> bool {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, b1, b2) = unsafe {
        (
            Words::new(dst, bits),
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };
    let lim = (bits / BITS_PER_LONG) as usize;
    let mut result = 0usize;

    for k in 0..lim {
        let v = b1.get(k) & !b2.get(k);
        d.set(k, v);
        result |= v;
    }

    if bits % BITS_PER_LONG != 0 {
        let v = b1.get(lim) & !b2.get(lim) & last_word_mask(bits);
        d.set(lim, v);
        result |= v;
    }

    result != 0
}

/// Replace the bits of `dst` that are set in `mask` with the ones of `new`.
///
/// C: `void __bitmap_replace(unsigned long *dst, const unsigned long *old, const unsigned long *new, const unsigned long *mask, unsigned int nbits)`
///
/// # Safety
///
/// `dst` must be valid for `bits_to_longs(nbits)` words and writable.
#[export]
pub unsafe extern "C" fn __bitmap_replace(
    dst: *mut usize,
    old: *const usize,
    new: *const usize,
    mask: *const usize,
    nbits: c_uint,
) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, o, n, m) = unsafe {
        (
            Words::new(dst, nbits),
            Words::new(old as *mut usize, nbits),
            Words::new(new as *mut usize, nbits),
            Words::new(mask as *mut usize, nbits),
        )
    };

    for k in 0..d.len() {
        d.set(k, (o.get(k) & !m.get(k)) | (n.get(k) & m.get(k)));
    }
}

/// Do two bitmaps have a bit in common?
///
/// C: `bool __bitmap_intersects(const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// Both pointers must be valid for `bits_to_longs(bits)` words.
#[export]
pub unsafe extern "C" fn __bitmap_intersects(
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) -> bool {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b1, b2) = unsafe {
        (
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };
    let lim = (bits / BITS_PER_LONG) as usize;

    for k in 0..lim {
        if b1.get(k) & b2.get(k) != 0 {
            return true;
        }
    }

    bits % BITS_PER_LONG != 0 && (b1.get(lim) & b2.get(lim)) & last_word_mask(bits) != 0
}

/// Is every bit of the first bitmap also set in the second one?
///
/// C: `bool __bitmap_subset(const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// Both pointers must be valid for `bits_to_longs(bits)` words.
#[export]
pub unsafe extern "C" fn __bitmap_subset(
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) -> bool {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b1, b2) = unsafe {
        (
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };
    let lim = (bits / BITS_PER_LONG) as usize;

    for k in 0..lim {
        if b1.get(k) & !b2.get(k) != 0 {
            return false;
        }
    }

    if bits % BITS_PER_LONG != 0 && (b1.get(lim) & !b2.get(lim)) & last_word_mask(bits) != 0 {
        return false;
    }

    true
}

/// The number of set bits in a bitmap.
///
/// C: `unsigned int __bitmap_weight(const unsigned long *bitmap, unsigned int bits)`
///
/// # Safety
///
/// `bitmap` must be valid for `bits_to_longs(bits)` words.
#[export]
pub unsafe extern "C" fn __bitmap_weight(bitmap: *const usize, bits: c_uint) -> c_uint {
    // SAFETY: the caller guarantees the buffer is valid.
    let b = unsafe { Words::new(bitmap as *mut usize, bits) };

    weight(&b, bits)
}

/// The number of bits set in both bitmaps.
///
/// C: `unsigned int __bitmap_weight_and(const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// Both pointers must be valid for `bits_to_longs(bits)` words.
#[export]
pub unsafe extern "C" fn __bitmap_weight_and(
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) -> c_uint {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b1, b2) = unsafe {
        (
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };
    let lim = (bits / BITS_PER_LONG) as usize;
    let mut w = 0;

    for k in 0..lim {
        w += (b1.get(k) & b2.get(k)).count_ones();
    }

    if bits % BITS_PER_LONG != 0 {
        w += (b1.get(lim) & b2.get(lim) & last_word_mask(bits)).count_ones();
    }

    w
}

/// The number of bits set in the first bitmap and not in the second one.
///
/// C: `unsigned int __bitmap_weight_andnot(const unsigned long *bitmap1, const unsigned long *bitmap2, unsigned int bits)`
///
/// # Safety
///
/// Both pointers must be valid for `bits_to_longs(bits)` words.
#[export]
pub unsafe extern "C" fn __bitmap_weight_andnot(
    bitmap1: *const usize,
    bitmap2: *const usize,
    bits: c_uint,
) -> c_uint {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b1, b2) = unsafe {
        (
            Words::new(bitmap1 as *mut usize, bits),
            Words::new(bitmap2 as *mut usize, bits),
        )
    };
    let lim = (bits / BITS_PER_LONG) as usize;
    let mut w = 0;

    for k in 0..lim {
        w += (b1.get(k) & !b2.get(k)).count_ones();
    }

    if bits % BITS_PER_LONG != 0 {
        w += (b1.get(lim) & !b2.get(lim) & last_word_mask(bits)).count_ones();
    }

    w
}

/// Set a range of bits.
///
/// C: `void __bitmap_set(unsigned long *map, unsigned int start, int len)`
///
/// # Safety
///
/// `map` must be valid for the words that the range covers.
#[export]
pub unsafe extern "C" fn __bitmap_set(map: *mut usize, start: c_uint, len: c_int) {
    let len = len.max(0);
    let size = start.wrapping_add(len as u32);

    // SAFETY: the caller guarantees the buffer is valid for the words the
    // range covers.
    let words = unsafe { Words::new(map, size) };

    let mut len = len;
    let mut bits_to_set = BITS_PER_LONG as i32 - (start % BITS_PER_LONG) as i32;
    let mut mask_to_set = first_word_mask(start);
    let mut p = (start / BITS_PER_LONG) as usize;

    while len - bits_to_set >= 0 {
        words.set(p, words.get(p) | mask_to_set);
        len -= bits_to_set;
        bits_to_set = BITS_PER_LONG as i32;
        mask_to_set = usize::MAX;
        p += 1;
    }

    if len > 0 {
        mask_to_set &= last_word_mask(size);
        words.set(p, words.get(p) | mask_to_set);
    }
}

/// Clear a range of bits.
///
/// C: `void __bitmap_clear(unsigned long *map, unsigned int start, int len)`
///
/// # Safety
///
/// `map` must be valid for the words that the range covers.
#[export]
pub unsafe extern "C" fn __bitmap_clear(map: *mut usize, start: c_uint, len: c_int) {
    let len = len.max(0);
    let size = start.wrapping_add(len as u32);

    // SAFETY: the caller guarantees the buffer is valid for the words the
    // range covers.
    let words = unsafe { Words::new(map, size) };

    let mut len = len;
    let mut bits_to_clear = BITS_PER_LONG as i32 - (start % BITS_PER_LONG) as i32;
    let mut mask_to_clear = first_word_mask(start);
    let mut p = (start / BITS_PER_LONG) as usize;

    while len - bits_to_clear >= 0 {
        words.set(p, words.get(p) & !mask_to_clear);
        len -= bits_to_clear;
        bits_to_clear = BITS_PER_LONG as i32;
        mask_to_clear = usize::MAX;
        p += 1;
    }

    if len > 0 {
        mask_to_clear &= last_word_mask(size);
        words.set(p, words.get(p) & !mask_to_clear);
    }
}

/// Find a contiguous, aligned run of clear bits.
///
/// C: `unsigned long bitmap_find_next_zero_area_off(unsigned long *map, unsigned long size, unsigned long start, unsigned int nr, unsigned long align_mask, unsigned long align_offset)`
///
/// # Safety
///
/// `map` must be valid for `bits_to_longs(size)` words.
#[export]
pub unsafe extern "C" fn bitmap_find_next_zero_area_off(
    map: *mut usize,
    size: c_ulong,
    start: c_ulong,
    nr: c_uint,
    align_mask: c_ulong,
    align_offset: c_ulong,
) -> c_ulong {
    // SAFETY: the caller guarantees the buffer is valid.
    let words = unsafe { Words::new(map, size as u32) };
    let size = size as u32;
    let mut start = start as u32;

    loop {
        let index = find_next_zero_bit(&words, size, start);

        // Align the allocation.
        let index = (index.wrapping_add(align_offset as u32) & !align_mask as u32)
            .wrapping_sub(align_offset as u32);

        let end = index.wrapping_add(nr);
        if end > size {
            return end as c_ulong;
        }

        let i = find_next_bit(&words, end, index);
        if i < end {
            start = i + 1;
            continue;
        }

        return index as c_ulong;
    }
}

/// Apply a bit mapping to a bitmap.
///
/// C: `void bitmap_remap(unsigned long *dst, const unsigned long *src, const unsigned long *old, const unsigned long *new, unsigned int nbits)`
///
/// # Safety
///
/// All the pointers must be valid for `bits_to_longs(nbits)` words.
#[export]
pub unsafe extern "C" fn bitmap_remap(
    dst: *mut usize,
    src: *const usize,
    old: *const usize,
    new: *const usize,
    nbits: c_uint,
) {
    if dst as *const usize == src {
        // The C original does not handle in-place remaps.
        return;
    }

    // SAFETY: the caller guarantees the buffers are valid.
    let (s, o, n) = unsafe {
        (
            Words::new(src as *mut usize, nbits),
            Words::new(old as *mut usize, nbits),
            Words::new(new as *mut usize, nbits),
        )
    };
    // SAFETY: the caller guarantees the buffer is valid.
    let d = unsafe { Words::new(dst, nbits) };

    zero(&d, nbits);

    let w = weight(&n, nbits);

    let mut oldbit = 0;
    while oldbit < nbits {
        if !test_bit(&s, oldbit) {
            oldbit += 1;
            continue;
        }

        // The ordinal of the bit in `old`, or -1.
        let ord = if oldbit >= nbits || !test_bit(&o, oldbit) {
            -1i64
        } else {
            weight(&o, oldbit) as i64
        };

        if ord < 0 || w == 0 {
            set_bit(&d, oldbit);
        } else {
            set_bit(&d, find_nth_bit(&n, nbits, (ord % w as i64) as u32));
        }

        oldbit += 1;
    }
}

/// Apply a bit mapping to a single bit.
///
/// C: `int bitmap_bitremap(int oldbit, const unsigned long *old, const unsigned long *new, int bits)`
///
/// # Safety
///
/// `old` and `new` must be valid for `bits_to_longs(bits)` words.
#[export]
pub unsafe extern "C" fn bitmap_bitremap(
    oldbit: c_int,
    old: *const usize,
    new: *const usize,
    bits: c_int,
) -> c_int {
    let bits = bits as u32;

    // SAFETY: the caller guarantees the buffers are valid.
    let (o, n) = unsafe {
        (
            Words::new(old as *mut usize, bits),
            Words::new(new as *mut usize, bits),
        )
    };

    let w = weight(&n, bits);
    let ord = if oldbit < 0 || oldbit as u32 >= bits || !test_bit(&o, oldbit as u32) {
        -1i64
    } else {
        weight(&o, oldbit as u32) as i64
    };

    if ord < 0 || w == 0 {
        oldbit
    } else {
        find_nth_bit(&n, bits, (ord % w as i64) as u32) as c_int
    }
}

/// Translate a bitmap relative to another one.
///
/// C: `void bitmap_onto(unsigned long *dst, const unsigned long *orig, const unsigned long *relmap, unsigned int bits)`
///
/// This symbol is not exported by the C original; it is only compiled when
/// `CONFIG_NUMA` is set.
///
/// # Safety
///
/// All the pointers must be valid for `bits_to_longs(bits)` words.
#[cfg(CONFIG_NUMA)]
#[no_mangle]
pub unsafe extern "C" fn bitmap_onto(
    dst: *mut usize,
    orig: *const usize,
    relmap: *const usize,
    bits: c_uint,
) {
    if dst as *const usize == orig {
        // The C original does not handle in-place mappings.
        return;
    }

    // SAFETY: the caller guarantees the buffers are valid.
    let (o, r) = unsafe {
        (
            Words::new(orig as *mut usize, bits),
            Words::new(relmap as *mut usize, bits),
        )
    };
    // SAFETY: the caller guarantees the buffer is valid.
    let d = unsafe { Words::new(dst, bits) };

    zero(&d, bits);

    let mut m = 0u32;
    let mut n = 0u32;

    while n < bits {
        if !test_bit(&r, n) {
            n += 1;
            continue;
        }

        if m < bits && test_bit(&o, m) {
            set_bit(&d, n);
        }
        m += 1;
        n += 1;
    }
}

/// Fold a bitmap into a smaller one, modulo a size.
///
/// C: `void bitmap_fold(unsigned long *dst, const unsigned long *orig, unsigned int sz, unsigned int nbits)`
///
/// This symbol is not exported by the C original; it is only compiled when
/// `CONFIG_NUMA` is set.
///
/// # Safety
///
/// `dst` and `orig` must be valid for `bits_to_longs(nbits)` words.
#[cfg(CONFIG_NUMA)]
#[no_mangle]
pub unsafe extern "C" fn bitmap_fold(
    dst: *mut usize,
    orig: *const usize,
    sz: c_uint,
    nbits: c_uint,
) {
    if dst as *const usize == orig {
        // The C original does not handle in-place mappings.
        return;
    }

    // SAFETY: the caller guarantees the buffers are valid.
    let o = unsafe { Words::new(orig as *mut usize, nbits) };
    // SAFETY: the caller guarantees the buffer is valid.
    let d = unsafe { Words::new(dst, nbits) };

    zero(&d, nbits);

    let mut oldbit = 0u32;
    while oldbit < nbits {
        if test_bit(&o, oldbit) {
            set_bit(&d, oldbit % sz);
        }
        oldbit += 1;
    }
}

/// Allocate a bitmap of `nbits` bits.
///
/// C: `unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags)`
#[export]
pub unsafe extern "C" fn bitmap_alloc(nbits: c_uint, flags: bindings::gfp_t) -> *mut usize {
    let words = bits_to_longs(nbits);

    // The C original uses kmalloc_array(words, sizeof(unsigned long), flags),
    // which fails when the multiplication overflows.
    if words.checked_mul(core::mem::size_of::<usize>()).is_none() {
        return core::ptr::null_mut();
    }

    match KVec::<usize>::with_capacity(words, Flags::from_gfp(flags)) {
        Ok(buf) => {
            let (ptr, _, _) = buf.into_raw_parts();
            ptr
        }
        Err(_) => core::ptr::null_mut(),
    }
}

/// Allocate a zeroed bitmap of `nbits` bits.
///
/// C: `unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags)`
#[export]
pub unsafe extern "C" fn bitmap_zalloc(nbits: c_uint, flags: bindings::gfp_t) -> *mut usize {
    // SAFETY: the arguments are the caller's, and `bitmap_zalloc()` only
    // allocates.
    unsafe { bitmap_alloc(nbits, with_zero(flags)) }
}

/// Allocate a bitmap of `nbits` bits on a NUMA node.
///
/// C: `unsigned long *bitmap_alloc_node(unsigned int nbits, gfp_t flags, int node)`
#[export]
pub unsafe extern "C" fn bitmap_alloc_node(
    nbits: c_uint,
    flags: bindings::gfp_t,
    node: c_int,
) -> *mut usize {
    let words = bits_to_longs(nbits);

    if words.checked_mul(core::mem::size_of::<usize>()).is_none() {
        return core::ptr::null_mut();
    }

    // An invalid node falls back to the local node, which is what the C code
    // does after its own node validation.
    let nid = NumaNode::new(node).unwrap_or(NumaNode::NO_NODE);

    match KVec::<usize>::with_capacity_in(words, Flags::from_gfp(flags), nid) {
        Ok(buf) => {
            let (ptr, _, _) = buf.into_raw_parts();
            ptr
        }
        Err(_) => core::ptr::null_mut(),
    }
}

/// Allocate a zeroed bitmap of `nbits` bits on a NUMA node.
///
/// C: `unsigned long *bitmap_zalloc_node(unsigned int nbits, gfp_t flags, int node)`
#[export]
pub unsafe extern "C" fn bitmap_zalloc_node(
    nbits: c_uint,
    flags: bindings::gfp_t,
    node: c_int,
) -> *mut usize {
    // SAFETY: the arguments are the caller's, and `bitmap_zalloc_node()` only
    // allocates.
    unsafe { bitmap_alloc_node(nbits, with_zero(flags), node) }
}

/// Free a bitmap allocated by [`bitmap_alloc()`].
///
/// C: `void bitmap_free(const unsigned long *bitmap)`
///
/// # Safety
///
/// `bitmap` must be a bitmap allocated by [`bitmap_alloc()`] and not yet
/// freed.
#[export]
pub unsafe extern "C" fn bitmap_free(bitmap: *const usize) {
    if bitmap.is_null() {
        return;
    }

    // SAFETY: the pointer was allocated by the kernel allocator (through
    // `kmalloc()`), and `kfree()` is the matching release, exactly as in the
    // C original. The size is not needed to free a kmalloc() allocation.
    unsafe { bindings::kfree(bitmap.cast()) };
}

/// The devres action that frees a bitmap.
unsafe extern "C" fn devm_bitmap_free(data: *mut c_void) {
    // SAFETY: the data pointer is the bitmap that was registered with the
    // device, and it is freed exactly once.
    unsafe { bitmap_free(data.cast_const().cast()) };
}

/// Allocate a bitmap that is freed when the device is unbound.
///
/// C: `unsigned long *devm_bitmap_alloc(struct device *dev, unsigned int nbits, gfp_t flags)`
///
/// # Safety
///
/// `dev` must be a valid, bound device.
#[export]
pub unsafe extern "C" fn devm_bitmap_alloc(
    dev: *mut bindings::device,
    nbits: c_uint,
    flags: bindings::gfp_t,
) -> *mut usize {
    // SAFETY: the arguments are the caller's, and `bitmap_alloc()` only
    // allocates.
    let bitmap = unsafe { bitmap_alloc(nbits, flags) };
    if bitmap.is_null() {
        return core::ptr::null_mut();
    }

    // SAFETY: `dev` is a valid device, and the callback frees the bitmap
    // that is passed as its data.
    let ret = unsafe {
        bindings::devm_add_action_or_reset(
            dev,
            Some(devm_bitmap_free),
            bitmap.cast::<c_void>(),
        )
    };

    if ret != 0 {
        // `devm_add_action_or_reset()` has already released the bitmap.
        return core::ptr::null_mut();
    }

    bitmap
}

/// Allocate a zeroed bitmap that is freed when the device is unbound.
///
/// C: `unsigned long *devm_bitmap_zalloc(struct device *dev, unsigned int nbits, gfp_t flags)`
///
/// # Safety
///
/// `dev` must be a valid, bound device.
#[export]
pub unsafe extern "C" fn devm_bitmap_zalloc(
    dev: *mut bindings::device,
    nbits: c_uint,
    flags: bindings::gfp_t,
) -> *mut usize {
    // SAFETY: the arguments are the caller's, and `devm_bitmap_alloc()`
    // only allocates and registers the free action.
    unsafe { devm_bitmap_alloc(dev, nbits, with_zero(flags)) }
}

/// Copy a bitmap from an array of 32-bit words.
///
/// C: `void bitmap_from_arr32(unsigned long *bitmap, const u32 *buf, unsigned int nbits)`
///
/// This is the 64-bit variant; the 32-bit one uses `bitmap_from_arr64()`.
///
/// # Safety
///
/// `bitmap` must be valid for `bits_to_longs(nbits)` words and `buf` for
/// `DIV_ROUND_UP(nbits, 32)` 32-bit words.
#[cfg(target_pointer_width = "64")]
#[export]
pub unsafe extern "C" fn bitmap_from_arr32(bitmap: *mut usize, buf: *const u32, nbits: c_uint) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b, src) = unsafe {
        (
            Words::new(bitmap, nbits),
            core::slice::from_raw_parts(buf, (nbits as usize).div_ceil(32)),
        )
    };

    let mut i = 0;
    while i < src.len() {
        b.set(i / 2, src[i] as usize);
        i += 1;
        if i < src.len() {
            b.set(i / 2, b.get(i / 2) | ((src[i] as usize) << 32));
            i += 1;
        }
    }

    // Clear the tail bits of the last word beyond `nbits`.
    if nbits % BITS_PER_LONG != 0 {
        let last = (src.len() - 1) / 2;
        b.set(last, b.get(last) & last_word_mask(nbits));
    }
}

/// Copy a bitmap into an array of 32-bit words.
///
/// C: `void bitmap_to_arr32(u32 *buf, const unsigned long *bitmap, unsigned int nbits)`
///
/// This is the 64-bit variant; the 32-bit one uses `bitmap_to_arr64()`.
///
/// # Safety
///
/// `buf` must be valid for `DIV_ROUND_UP(nbits, 32)` 32-bit words and
/// `bitmap` for `bits_to_longs(nbits)` words.
#[cfg(target_pointer_width = "64")]
#[export]
pub unsafe extern "C" fn bitmap_to_arr32(buf: *mut u32, bitmap: *const usize, nbits: c_uint) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b, dst) = unsafe {
        (
            Words::new(bitmap as *mut usize, nbits),
            core::slice::from_raw_parts_mut(buf, (nbits as usize).div_ceil(32)),
        )
    };

    let mut i = 0;
    while i < dst.len() {
        dst[i] = b.get(i / 2) as u32;
        i += 1;
        if i < dst.len() {
            dst[i] = (b.get(i / 2) >> 32) as u32;
            i += 1;
        }
    }

    // Clear the tail bits of the last element beyond `nbits`.
    if nbits % BITS_PER_LONG != 0 {
        dst[dst.len() - 1] &= u32::MAX >> (nbits.wrapping_neg() & 31);
    }
}

/// Copy a bitmap from an array of 64-bit words.
///
/// C: `void bitmap_from_arr64(unsigned long *bitmap, const u64 *buf, unsigned int nbits)`
///
/// This is the 32-bit variant; the 64-bit one uses `bitmap_from_arr32()`.
///
/// # Safety
///
/// `bitmap` must be valid for `bits_to_longs(nbits)` words and `buf` for
/// `DIV_ROUND_UP(nbits, 64)` 64-bit words.
#[cfg(target_pointer_width = "32")]
#[export]
pub unsafe extern "C" fn bitmap_from_arr64(bitmap: *mut usize, buf: *const u64, nbits: c_uint) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (d, src) = unsafe {
        (
            Words::new(bitmap, nbits),
            core::slice::from_raw_parts(buf, (nbits as usize).div_ceil(64)),
        )
    };

    let mut n = nbits as i32;
    let mut i = 0;
    let mut s = 0;

    while n > 0 {
        let val = src[s];
        s += 1;

        d.set(i, val as usize);
        i += 1;

        if n > 32 {
            d.set(i, (val >> 32) as usize);
            i += 1;
        }

        n -= 64;
    }

    // Clear the tail bits of the last word beyond `nbits`.
    if nbits % BITS_PER_LONG != 0 {
        d.set(i - 1, d.get(i - 1) & last_word_mask(nbits));
    }
}

/// Copy a bitmap into an array of 64-bit words.
///
/// C: `void bitmap_to_arr64(u64 *buf, const unsigned long *bitmap, unsigned int nbits)`
///
/// This is the 32-bit variant; the 64-bit one uses `bitmap_to_arr32()`.
///
/// # Safety
///
/// `buf` must be valid for `DIV_ROUND_UP(nbits, 64)` 64-bit words and
/// `bitmap` for `bits_to_longs(nbits)` words.
#[cfg(target_pointer_width = "32")]
#[export]
pub unsafe extern "C" fn bitmap_to_arr64(buf: *mut u64, bitmap: *const usize, nbits: c_uint) {
    // SAFETY: the caller guarantees the buffers are valid.
    let (b, dst) = unsafe {
        (
            Words::new(bitmap as *mut usize, nbits),
            core::slice::from_raw_parts_mut(buf, (nbits as usize).div_ceil(64)),
        )
    };

    let mut i = 0;
    let mut j = 0;

    while i < b.len() {
        dst[j] = b.get(i) as u64;
        i += 1;

        if i < b.len() {
            dst[j] |= (b.get(i) as u64) << 32;
            i += 1;
        }

        j += 1;
    }

    // Clear the tail bits of the last element beyond `nbits`.
    if nbits % 64 != 0 {
        dst[j - 1] &= u64::MAX >> ((64 - (nbits % 64)) % 64);
    }
}

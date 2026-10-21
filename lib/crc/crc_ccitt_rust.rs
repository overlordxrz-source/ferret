// SPDX-License-Identifier: GPL-2.0-only

//! Ferret: CRC-CCITT, implemented in Rust.
//!
//! Replaces `lib/crc/crc-ccitt.c`. The C ABI (`include/linux/crc-ccitt.h`) is
//! unchanged:
//!
//! ```c
//! extern u16 const crc_ccitt_table[256];   /* exported data, used by C inline code */
//! u16 crc_ccitt(u16 crc, const u8 *buffer, size_t len);
//! ```
//!
//! `crc_ccitt_byte()` stays an inline function in the header; it indexes the
//! exported table, so the table contents are part of the ABI and are checked
//! entry by entry against the original C table by the differential KUnit suite.
//!
//! Model: width 16, polynomial 0x1021, reflected (LSB-first), feedback value
//! 0x8408, caller-supplied initial value, no final XOR (CRC-16/KERMIT update).

use kernel::prelude::*;

/// Number of entries in the byte lookup table (one per possible byte value).
const TABLE_SIZE: usize = 256;

/// Build the reflected CRC-CCITT byte table at compile time.
///
/// The construction is the same as the C original: entry `i` is the CRC of the
/// single byte `i` processed bit by bit, with the reflected polynomial 0x8408.
/// Generating the table with a `const fn` removes any possibility of a
/// transcription error, and the differential suite still compares all 256
/// entries against the original C table.
const fn build_table() -> [u16; TABLE_SIZE] {
    let mut table = [0u16; TABLE_SIZE];
    let mut i = 0usize;

    while i < TABLE_SIZE {
        let mut crc = i as u16;
        let mut bit = 0;

        while bit < 8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0x8408
            } else {
                crc >> 1
            };
            bit += 1;
        }

        table[i] = crc;
        i += 1;
    }

    table
}

/// Byte lookup table for the reflected 0x1021 polynomial.
///
/// This is an exported data symbol: `include/linux/crc-ccitt.h` declares it as
/// `u16 const crc_ccitt_table[256]` and every C caller's inline
/// `crc_ccitt_byte()` reads it. `#[no_mangle]` keeps the exact C symbol name;
/// the type (`[u16; 256]`) and `const`-like immutability match the C object.
#[no_mangle]
pub static crc_ccitt_table: [u16; TABLE_SIZE] = build_table();

/// Recompute the CRC-CCITT for a data buffer.
///
/// # Safety
///
/// `buffer` must be valid for reads of `len` bytes, or `len` must be zero (in
/// which case `buffer` is never dereferenced and may be null, matching the C
/// contract).
///
/// # Arguments
///
/// * `crc`    - previous CRC value (0 for a fresh computation)
/// * `buffer` - data pointer
/// * `len`    - number of bytes in the buffer
#[export]
pub unsafe extern "C" fn crc_ccitt(mut crc: u16, mut buffer: *const u8, mut len: usize) -> u16 {
    while len > 0 {
        // SAFETY: `buffer` is valid for reads of `len` bytes (precondition) and
        // `len > 0`, so this read is in bounds. The pointer is advanced by one
        // byte per iteration and `len` is decremented in lockstep.
        let byte = unsafe { *buffer };
        let index = ((crc ^ byte as u16) & 0xff) as usize;
        // SAFETY: `index` is masked to 0..=255 and the table has 256 entries.
        // Using `get_unchecked` keeps the exported path free of bounds-check
        // panics.
        crc = (crc >> 8) ^ unsafe { *crc_ccitt_table.get_unchecked(index) };
        // SAFETY: one byte past the current position is still inside the
        // caller-provided buffer because `len` bytes remain before the
        // decrement below.
        buffer = unsafe { buffer.add(1) };
        len -= 1;
    }

    crc
}

#[cfg(CONFIG_KUNIT)]
#[kunit_tests(crc_ccitt_rust)]
mod tests {
    use super::crc_ccitt;

    /// CRC-CCITT/KERMIT check value: "123456789" with init 0 -> 0x2189.
    #[test]
    fn crc_ccitt_rust_check_value() {
        let data = b"123456789";
        // SAFETY: valid slice pointer and length.
        let out = unsafe { crc_ccitt(0, data.as_ptr(), data.len()) };
        assert_eq!(out, 0x2189);
    }

    /// Init 0xffff over "123456789" -> 0x6f91 (upstream-compatible vectors).
    #[test]
    fn crc_ccitt_rust_check_value_init_ffff() {
        let data = b"123456789";
        // SAFETY: valid slice pointer and length.
        let out = unsafe { crc_ccitt(0xffff, data.as_ptr(), data.len()) };
        assert_eq!(out, 0x6f91);
    }

    #[test]
    fn crc_ccitt_rust_empty_buffer() {
        // Zero length: the pointer is never dereferenced, even if null.
        // SAFETY: len == 0, so the null pointer is not read.
        let out = unsafe { crc_ccitt(0x1234, core::ptr::null(), 0) };
        assert_eq!(out, 0x1234);
    }

    #[test]
    fn crc_ccitt_rust_incremental() {
        let data = b"Ferret rewrites the Linux kernel in Rust, one component at a time.";
        let split = 17;
        // SAFETY: pointers are derived from `data` with in-bounds lengths.
        let whole = unsafe { crc_ccitt(0x1d0f, data.as_ptr(), data.len()) };
        // SAFETY: as above.
        let first = unsafe { crc_ccitt(0x1d0f, data.as_ptr(), split) };
        // SAFETY: as above.
        let rest = unsafe { crc_ccitt(first, data.as_ptr().add(split), data.len() - split) };
        assert_eq!(whole, rest);
    }

    /// Every initial value over one byte must equal the inline C formula
    /// `(crc >> 8) ^ table[(crc ^ byte) & 0xff]`.
    #[test]
    fn crc_ccitt_rust_all_initial_values_single_byte() {
        let byte = [0x5au8];
        let mut init = 0u32;
        while init < 0x10000 {
            // SAFETY: valid slice pointer and length 1.
            let out = unsafe { crc_ccitt(init as u16, byte.as_ptr(), 1) };
            let expected = ((init as u16) >> 8)
                ^ crate::crc_ccitt_table[(((init as u16) ^ 0x5a) & 0xff) as usize];
            assert_eq!(out, expected);
            init += 1;
        }
    }

    /// The compile-time generated table must match a bit-by-bit construction
    /// for every entry (independent of `build_table`'s loop shape).
    #[test]
    fn crc_ccitt_rust_table_matches_bitwise() {
        for i in 0..256usize {
            let mut crc = i as u16;
            let mut bit = 0;
            while bit < 8 {
                if crc & 1 != 0 {
                    crc = (crc >> 1) ^ 0x8408;
                } else {
                    crc >>= 1;
                }
                bit += 1;
            }
            assert_eq!(crate::crc_ccitt_table[i], crc);
        }
        // Known upstream spot checks (lib/crc/crc-ccitt.c).
        assert_eq!(crate::crc_ccitt_table[0], 0x0000);
        assert_eq!(crate::crc_ccitt_table[1], 0x1189);
        assert_eq!(crate::crc_ccitt_table[128], 0x8408);
    }

    #[test]
    fn crc_ccitt_rust_deterministic_prng() {
        // xorshift32: deterministic pseudo-random stream, no allocation.
        let mut state = 0x2545_f491u32;
        let mut buf = [0u8; 257];
        buf[0] = 0x5a;
        let mut i = 1;
        while i < buf.len() {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            buf[i] = (state >> 24) as u8;
            i += 1;
        }
        // Reference: bit-at-a-time reflected CRC-CCITT, independent of the
        // table lookup used by the implementation.
        let mut crc: u16 = 0x1d0f;
        for &b in buf.iter() {
            crc ^= b as u16;
            let mut bit = 0;
            while bit < 8 {
                if crc & 1 != 0 {
                    crc = (crc >> 1) ^ 0x8408;
                } else {
                    crc >>= 1;
                }
                bit += 1;
            }
        }
        // SAFETY: valid slice pointer and length.
        let out = unsafe { crc_ccitt(0x1d0f, buf.as_ptr(), buf.len()) };
        assert_eq!(out, crc);
    }
}

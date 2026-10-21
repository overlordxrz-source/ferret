# Rust safety policy

Rust is not automatically safe in a kernel. A safe wrapper around a C contract
that Rust cannot express can still be unsound, and a panic in kernel Rust takes
the machine down. Ferret's safety argument is therefore explicit and local.

## 1. Rules

1. **Safe by default.** New code is safe Rust unless the FFI boundary requires
   otherwise.
2. **`unsafe` is local and documented.** Every block has a `// SAFETY:` comment
   stating the invariant and who maintains it (the C caller, the type, a lock,
   or the function itself).
3. **No panics on reachable paths.** No `unwrap`, no `expect`, no slice
   indexing that can be driven out of bounds by malformed input, no unchecked
   arithmetic that can overflow on user-controlled values. A kernel `panic!`
   aborts the machine.
4. **Fallible allocation.** `KBox`/`KVBox`/`Vec` construction returns `Result`;
   callers must propagate failure with the same semantics as the C original
   (`-ENOMEM`, `NULL`, or a documented retry), and must respect the original
   GFP context.
5. **No hidden C fallback.** If the Rust path fails, the function fails the same
   way C would. Re-entering the C implementation is a correctness and security
   risk and is forbidden.
6. **No new synchronisation primitives.** Use `kernel::sync` and the kernel's
   lock ordering. Rust's `std` sync types do not exist here.
7. **FFI types are C types.** `#[repr(C)]`, fixed-width integers, raw pointers
   only at the boundary, no Rust enums or trait objects across it.

## 2. Current unsafe inventory

Ferret's migrated components are small and the unsafe surface is auditable.

`lib/crc/crc16_rust.rs` (`crc16`) and `lib/crc/crc_ccitt_rust.rs`
(`crc_ccitt`) each have:

* one `unsafe extern "C" fn` — the ABI entry point. Its safety contract is the
  C declaration's: `p` is valid for `len` bytes (or `len == 0`), as documented
  on the function.
* one loop that reads `*const u8` with `read`, increments the pointer with
  `add`, and stops after exactly `len` bytes. One-past-the-end pointer
  arithmetic is allowed; no dereference past the end occurs.
* `get_unchecked` on a 256-entry table indexed by a `u8`-derived value. This is
  provably in bounds: the index is `(crc ^ byte) & 0xff`, and the table length
  is 256.

The Rust unit tests call the `unsafe` entry point with valid slices (and, for
the empty-buffer test, a null pointer with `len == 0`), each with a `SAFETY`
comment.

There is no allocation, no lock, no global mutable state and no callback in
either component, which is why they are suitable as the first migrations.

`lib/crc/crc_ccitt_rust.rs` exports a `static` table (`crc_ccitt_table`) built
by a `const fn` at compile time. `static` mutability is absent; C's type is
`const u16 *`, which the build checks.

## 2.1 Memory ordering

Components that publish state to another CPU use the kernel's own barrier
primitives (`kernel::sync::barrier::{smp_wmb, smp_rmb, smp_mb}`), not Rust
release fences. The difference is measurable in the generated code: on arm64 a
release fence is `dmb ish`, while the kernel's `smp_wmb()` is `dmb ishst` — the
fence is *stronger* than the C original, not equivalent. Using the kernel
primitive makes the port emit the same instruction as the C code, with the
same LKMM semantics, on every architecture.

## 3. Soundness argument for the FFI boundary

For `crc16(crc, p, len)`:

* **Precondition** (C contract): `p` points to `len` readable bytes, or
  `len == 0`.
* **Rust guarantee**: for `i in 0..len`, the code reads exactly `p + i` once,
  with `i < len`; pointer arithmetic stays within the object or one past it.
* **Postcondition**: a pure function of `(crc, bytes)`; no state is retained.

`len == 0` is handled before any dereference, so a null pointer is valid exactly
when C allows it.

The tables are immutable and const-initialised, so there is no init race or
lazy allocation.

## 4. Review checklist

A reviewer of a migration must be able to answer:

- [ ] What is the C contract, including error and atomic-context behaviour?
- [ ] Which invariants does Rust rely on, and are they written down at the
      `unsafe` block?
- [ ] Can any input reach a panic, an overflow or an out-of-bounds index?
- [ ] Is allocation fallible and context-appropriate?
- [ ] Is every exported symbol's visibility preserved?
- [ ] Does the differential test cover boundaries, not just happy paths?
- [ ] Is there any path that calls the old C implementation?
- [ ] Does the boot test exercise the component?

## 5. Tooling and its limits

* **bindgen** generates the C signatures; a mismatch is a compile error.
* **KUnit** exercises behaviour at runtime, including random inputs.
* **linkage.sh** proves which object defines the symbol.
* **Clippy/rustfmt** are applied where the kernel build permits; they do not
  prove soundness.

There is **no** Miri (it cannot model the kernel), no KASAN/KCSAN boot yet, and
no formal verification. Claims are limited to the tested surface.

## 6. What would make the project unsafe

These are rejected:

* a Rust wrapper that forwards to C and calls itself a migration;
* `unsafe` without an invariant, or an invariant that the caller cannot know;
* `static mut`;
* converting a C `EXPORT_SYMBOL` into GPL-only because Rust's export macro is
  easier;
* a fallback that re-runs the C implementation when Rust returns an error;
* a "safe" API that takes a raw pointer and dereferences it without a
  documented lifetime rule.

The goal is not zero `unsafe` — the goal is a small, reviewed, provable boundary
that shrinks as more abstractions become available.

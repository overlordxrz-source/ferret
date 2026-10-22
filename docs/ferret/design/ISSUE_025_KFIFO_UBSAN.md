# Issue #25 — the UBSAN reports in the FIFO differential suite

Component: `lib/kfifo.c` → `lib/kfifo_rust.rs` (checkpoint 003)
Issue: [#25](https://github.com/overlordxrz-source/ferret/issues/25)
Branch: `ferret/issue-025-kfifo-ubsan`

## 1. Summary

The KASAN+UBSAN run of the independent-verification gate
([`AUDIT_PROTOCOL.md`](../../../AUDIT_PROTOCOL.md), phase 6) reported two
`shift-out-of-bounds` findings. Both are in the **C reference** — the
unmodified upstream `lib/kfifo.c` — and not in the Rust port:

```
[   24.553246] UBSAN: shift-out-of-bounds in /build/src/include/linux/log2.h:67:13
[   24.554047]     # kfifo_diff_init: lib/ubsan.c:228: shift-out-of-bounds in /build/src/include/linux/log2.h
[   24.555226] shift exponent 4294967295 is too large for 64-bit type 'unsigned long'
[   24.558209]  kfifo_c_ref_init+0x1e7/0x1f0
[   24.558253]  kfifo_diff_init+0xa2/0x190
...
[   24.565543] UBSAN: shift-out-of-bounds in /build/src/include/linux/log2.h:57:13
[   24.565769]     # kfifo_diff_alloc: lib/ubsan.c:228: shift-out-of-bounds in /build/src/include/linux/log2.h
[   24.565959] shift exponent 64 is too large for 64-bit type 'unsigned long'
[   24.566646]  kfifo_c_ref_alloc+0x1eb/0x200
[   24.566749]  kfifo_diff_alloc+0xce/0x520
```

The classification is: **an upstream defect, exposed by a Ferret test**. It
is not a Ferret regression, and the Rust implementation does not have it.

## 2. The root cause

Both reports come from `include/linux/log2.h`, which is upstream code:

```c
static inline unsigned long __roundup_pow_of_two(unsigned long n)
{
	return 1UL << fls_long(n - 1);        /* line 57 */
}

static inline unsigned long __rounddown_pow_of_two(unsigned long n)
{
	return 1UL << (fls_long(n) - 1);      /* line 67 */
}
```

* `__roundup_pow_of_two(0)`: `fls_long(0 - 1)` is `fls_long(ULONG_MAX)`,
  which is 64 — the width of the type. `1UL << 64` is undefined behaviour.
  UBSAN: *"shift exponent 64 is too large for 64-bit type"*.
* `__rounddown_pow_of_two(0)`: `fls_long(0)` is 0, so the shift count is
  `0 - 1`, which is negative and becomes `0xFFFFFFFF` when it is used as an
  unsigned shift count. `1UL << 0xFFFFFFFF` is undefined behaviour. UBSAN:
  *"shift exponent 4294967295 is too large for 64-bit type"*.

Neither helper documents that zero is undefined, and the upstream kernel
never calls them with zero — the upstream FIFO KUnit suite
(`lib/tests/kfifo_kunit.c`) always uses a fixed size of 32 elements. The
undefined behaviour is therefore latent upstream.

## 3. How the Ferret differential suite reaches it

`lib/kfifo_diff_kunit.c` deliberately tests the edge cases of the FIFO, and
two of them are the zero cases that the upstream code does not define:

* `kfifo_diff_alloc()` allocates a FIFO of size 0. The C
  `__kfifo_alloc()` does `size = roundup_pow_of_two(size)` → the undefined
  `__roundup_pow_of_two(0)` → the first report.
* `kfifo_diff_init()` initialises a FIFO whose element count is zero, i.e.
  `size < esize` (or `size == 0`). The C `__kfifo_init()` does
  `size /= esize` and then `size = rounddown_pow_of_two(size)` → the
  undefined `__rounddown_pow_of_two(0)` → the second report. (With an
  element size of zero, the C divides by zero, which is undefined as well.)

So the *trigger* is the test, and the *defect* is upstream's.

## 4. Why this is not a Ferret regression

The Rust implementation defines all of these cases:

```rust
/// `roundup_pow_of_two()` for a run-time value (`include/linux/log2.h`).
///
/// The C helper is `1UL << fls_long(n - 1)`. For `n == 0` the C code shifts
/// by the word width, which is undefined; this implementation returns 1 …
fn roundup_pow_of_two(n: u32) -> u32 {
    if n <= 1 {
        return 1;
    }
    …
}

/// `rounddown_pow_of_two()` for a run-time value (`include/linux/log2.h`).
///
/// The C helper is `1UL << (fls_long(n) - 1)`, which is undefined for
/// `n == 0`; this implementation returns 0 there.
fn rounddown_pow_of_two(n: u32) -> u32 {
    if n == 0 {
        return 0;
    }
    …
}
```

and the Rust `__kfifo_init()` rejects a zero element size instead of
dividing by zero, and returns `-EINVAL` for a FIFO of fewer than two
elements — the same error the C code returns for every other small size.

The *observable* behaviour of the C and the Rust implementations agrees on
the two architectures that Ferret builds, because the C result is truncated
back to the 32-bit `size` of the FIFO:

| input | C (x86_64, arm64) | Rust |
| --- | --- | --- |
| `__kfifo_alloc(0)` | `-EINVAL`, `data == NULL` | `-EINVAL`, `data == NULL` |
| `__kfifo_init(.., size < esize)` | `-EINVAL`, `mask == 0` | `-EINVAL`, `mask == 0` |

That is why the differential suite passes: the undefined behaviour changes
an intermediate value, not the result. The UBSAN report is the only symptom.

## 5. The proposed fix

Comparing the Rust implementation against the C reference *for an input
whose C behaviour is undefined* compares noise: the C result is not
specified, it is whatever the architecture's shift instruction happens to
produce. The fix therefore keeps the comparison for every input the C
defines, and checks the Rust implementation against the specification for
the inputs the C does not define.

`lib/kfifo_diff_kunit.c`:

* a helper tells whether the C reference is undefined for an input:

  ```c
  static bool kfifo_diff_c_undefined_init(unsigned int size, size_t esize)
  {
  	return esize == 0 || size < esize;
  }

  static bool kfifo_diff_c_undefined_alloc(unsigned int size)
  {
  	return size == 0;
  }
  ```

* the C reference is **not called** for those inputs, and the Rust
  implementation is checked against the specification instead: `-EINVAL`,
  a zero mask, and no buffer.

The change is confined to the test. The Rust port, the C reference and the
upstream headers are untouched, so the Ferret tree does not diverge from the
upstream baseline.

### What is deliberately *not* done

* **The upstream headers are not patched.** `include/linux/log2.h` is
  upstream code, and a patch there would have to be carried across every
  upstream synchronisation (and it would change the baseline the project
  measures against). The upstream wart is reported here instead.
* **The sanitizer is not silenced.** The alternative would be to build the C
  reference with `-fno-sanitize=shift`, which would also hide a *new*
  upstream shift defect. Not silencing it keeps the sanitizer honest for
  the whole kernel, and the undefined input is avoided instead.

## 6. The reproduction

The finding, on the checkpoint 005 branch (the C reference is built from the
unmodified `lib/kfifo.c`, see `lib/Makefile`):

```sh
tools/ferret/audit/sanitizer.sh kasan-ubsan KASAN KASAN_GENERIC KASAN_INLINE \
	UBSAN UBSAN_BOUNDS UBSAN_SHIFT
```

The two reports above are in the boot log
(`artifacts/audit/sanitizer-kasan-ubsan.txt`).

The same finding without the sanitizer tooling, with the configuration
switches alone:

```sh
# in the build container
scripts/config --file /build/linux-rust/.config --enable UBSAN \
	--enable UBSAN_BOUNDS --enable UBSAN_SHIFT
make -C /build/src O=/build/linux-rust ARCH=x86_64 LLVM=1 olddefconfig
make -C /build/src O=/build/linux-rust ARCH=x86_64 LLVM=1 -j"$(nproc)" bzImage
# boot it and look for "UBSAN: shift-out-of-bounds" in the serial log
```

## 7. The verification

After the fix, the same UBSAN build boots without a single report, and the
FIFO differential suite still passes:

```
# kfifo_diff: pass:10 fail:0 skip:0 total:10
```

The other suites are unaffected: `refcount_rust` 7/7, `refcount_diff` 9/9,
and the upstream `kfifo` suite still passes in both variants.

## 8. A tooling defect found on the way

The first verification run of this branch reported five failures in the
*refcount* differential suite, with the Rust count one lower than the C
reference. That was not a regression: the mutation testing of checkpoint 005
had left a **mutated object** in the build tree. The build script copies the
source with `rsync -a`, which preserves the modification times, so the
restored file looked *older* than the object that had been built from the
mutated one, and the build system kept the stale object.

Two fixes:

* the build script now copies with `--checksum`, so the content is what is
  compared;
* the mutation script touches the files it mutated when it restores the
  pristine tree, so that they are rebuilt.

The mutation testing had therefore been running against a stale object after
its restore step. The clean cycle of checkpoint 005 was run *before* the
last mutation, and it wiped the build tree, so the published evidence of
checkpoint 005 is not affected.

# Porting policy

Rules that decide what counts as a migration, how it is reviewed, and when it
may be enabled. These exist to prevent "Rust theatre": code that compiles,
looks like progress in line counts, and does not change what the kernel runs.

## 1. Eligibility of a component

A component is eligible when it has:

* a narrow, documented C interface (one header, few functions),
* no dependency on a Rust abstraction that does not exist yet,
* deterministic, testable behaviour,
* a path to KUnit or userspace-level tests,
* call sites that can be verified from the build tree.

Components failing this are placed in the registry with `status: blocked` and a
concrete prerequisite.

## 2. Definition of "migrated"

A component is **migrated** only when all of the following hold:

1. The C implementation object is **not built** for the Rust variant.
2. The Rust translation unit defines the exported symbol(s) with the exact C
   ABI, verified by `#[export]`'s bindgen signature check.
3. The symbol is defined exactly once in `vmlinux` (`nm` evidence).
4. The ksymtab entry (if upstream exports the symbol) exists with the same
   visibility (`EXPORT_SYMBOL` vs `EXPORT_SYMBOL_GPL`).
5. Upstream behavioural tests pass on the Rust build.
6. A differential test against the original C implementation passes.
7. A QEMU boot with the Rust build runs the migrated code and the tests.
8. The registry records the evidence paths.

Anything less is `status: partial` and must say precisely which item is
missing.

## 3. Not a migration

The following are rejected in review:

* Rust functions that call the C implementation.
* Keeping both implementations linked with a runtime switch.
* Adding Rust code that is only reachable in tests.
* Replacing a component with an algorithmically different one that "passes
  tests" but changes undocumented behaviour (e.g. table generation at boot
  instead of at compile time, when early-boot callers exist).
* Changing the exported symbol's GPL visibility.
* Leaving the C implementation in the build "just in case".

## 4. Unsafe code

* Every `unsafe` block needs a `// SAFETY:` comment.
* Raw pointers are allowed only in the FFI signature and in code that
  immediately converts them to slices with a validated length.
* The safe core of the component must be callable from safe Rust where the
  interface allows it.
* Panics must be impossible on the exported path. Where a panic cannot be
  ruled out statically, the component is not eligible.

## 5. Behavioural fidelity checklist

For each component, record in the registry:

* input domain and boundary cases (length 0, maximum length, initial state),
* alignment/aliasing requirements,
* concurrency guarantees (pure functions vs. locks),
* arithmetic widths and overflow behaviour,
* iteration order and chunking (matters for streaming components),
* error values and their signs,
* symbol name, linkage and visibility,
* section placement where it matters (e.g. `__init`, `.rodata`).

## 6. Enablement policy

* The Rust variant defaults to `n` until all eight criteria above are met and
  the enablement has survived at least one full checkpoint cycle.
* Test kernels for the Rust variant must enable KUnit and the differential
  suite.
* Reference kernels must not enable any Ferret config symbol; the build script
  disables them explicitly and `linkage.sh ref` asserts their absence.

## 7. Benchmarking

Performance claims are only made with both variants built from the same
toolchain, booted under the same QEMU invocation, using the upstream
`CONFIG_CRC_BENCHMARK`-style in-kernel measurement or an equivalent KUnit
benchmark. Raw Rust LOC is never a success metric; the registry reports
migrated components, tested components, remaining C dependencies and measured
regressions.

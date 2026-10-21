# Ferret migration registry report

Generated from `registry/migrations.json` by `tools/ferret/registry.py`. Do not edit by hand.

Baseline: `v6.18.57` (`2ff600d29ee478d0a8d46ef8a58afb5e038fe692`)

## 1. Components analysed

| Component | Subsystem | Status | Rust compiled | C replaced |
| --- | --- | --- | --- | --- |
| `crc16` | lib/crc | migrated | yes | yes |
| `crc-ccitt` | lib/crc | migrated | yes | yes |
| `crc-itu-t` | lib/crc | candidate | no | no |
| `crc8` | lib/crc | candidate | no | no |
| `crc7` | lib/crc | candidate | no | no |
| `crc32` | lib/crc | candidate-later | no | no |
| `crc64` | lib/crc | candidate-later | no | no |
| `kfifo` | lib | migrated | yes | yes |
| `percpu-counter` | lib | candidate-blocked | no | no |
| `eventfd` | fs | candidate-blocked | no | no |
| `bitmap` | lib | migrated | yes | yes |
| `refcount` | lib | migrated | yes | yes |

## 2. Components genuinely migrated

* `crc16` — CRC-16 (poly 0x8005, reflected): lib/crc/crc16.c → lib/crc/crc16_rust.rs
* `crc-ccitt` — CRC-CCITT (poly 0x1021): lib/crc/crc-ccitt.c → lib/crc/crc_ccitt_rust.rs
* `kfifo` — Generic kernel FIFO (kfifo): lib/kfifo.c → lib/kfifo_rust.rs
* `bitmap` — Kernel bitmap (lib/bitmap.c): lib/bitmap.c → lib/bitmap_rust.rs
* `refcount` — Out-of-line reference counting helpers (lib/refcount.c): lib/refcount.c → lib/refcount_rust.rs

## 3. Components tested

| Component | Test | Kind | Result | Evidence |
| --- | --- | --- | --- | --- |
| `crc16` | kunit-crc-suite | upstream-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `crc16` | kunit-crc16-differential | differential-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `crc16` | kunit-crc16-rust-vectors | rust-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `crc16` | qemu-boot-userspace-abi | boot | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `crc16` | linkage-ownership | static-analysis | PASS | `docs/ferret/evidence/linkage-x86_64.txt` |
| `crc-ccitt` | kunit-crc-ccitt-differential | differential-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `crc-ccitt` | kunit-crc-ccitt-table-and-vectors | rust-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `crc-ccitt` | qemu-boot-userspace-abi | boot | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `crc-ccitt` | qemu-boot-ubuntu-amd64 | boot | PASS | `docs/ferret/evidence/boot-ubuntu-shell-amd64.txt` |
| `crc-ccitt` | linkage-ownership | static-analysis | PASS | `docs/ferret/evidence/linkage-x86_64.txt` |
| `crc-ccitt` | qemu-boot-ubuntu-arm64-systemd | boot | PASS | `docs/ferret/evidence/boot-ubuntu-systemd-arm64.txt` |
| `kfifo` | kunit-kfifo-upstream | upstream-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `kfifo` | kunit-kfifo-rust-vectors | rust-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `kfifo` | kunit-kfifo-differential | differential-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `kfifo` | kunit-kfifo-user-copy-fault | differential-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `kfifo` | kunit-kfifo-dma-wrap | differential-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `kfifo` | kunit-kfifo-throughput | benchmark | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `kfifo` | linkage-ownership | static-analysis | PASS | `docs/ferret/evidence/linkage-x86_64.txt` |
| `bitmap` | upstream-test-bitmap-kselftest | upstream-kselftest | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `bitmap` | kunit-bitmap-differential | differential-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `bitmap` | linkage-ownership | static-analysis | PASS | `docs/ferret/evidence/linkage-x86_64.txt` |
| `refcount` | kunit-refcount-rust | rust-unit-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `refcount` | kunit-refcount-differential | differential-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `refcount` | kunit-refcount-concurrency | differential-kunit | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `refcount` | kunit-refcount-property-and-model | property-test | PASS | `docs/ferret/evidence/boot-x86_64-rust.txt` |
| `refcount` | kunit-refcount-under-kasan-ubsan | sanitizer-kunit | PASS | `artifacts/audit/sanitizer-kasan-ubsan.txt` |
| `refcount` | mutation-testing | mutation-testing | PASS | `artifacts/audit/mutation-test.txt` |
| `refcount` | linkage-ownership | static-analysis | PASS | `docs/ferret/evidence/linkage-x86_64.txt` |
| `refcount` | linkage-ownership-arm64 | static-analysis | PASS | `docs/ferret/evidence/linkage-arm64.txt` |
| `refcount` | clean-cycle | clean-cycle | PASS | `docs/ferret/evidence/results-clean-cycle.txt` |

## 4. Components with remaining C dependencies

* `crc16` — `lib/crc/crc16_export.c` (linkage/metadata): ksymtab entry only, to preserve non-GPL EXPORT_SYMBOL visibility (see DECISIONS D-004)
* `crc-ccitt` — `lib/crc/crc_ccitt_export.c` (linkage/metadata): ksymtab entries only for crc_ccitt and crc_ccitt_table, preserving non-GPL EXPORT_SYMBOL visibility (DECISIONS D-004); no algorithm
* `kfifo` — `lib/kfifo_export.c` (linkage/metadata): ksymtab entries only (23 symbols); no algorithm. Exit condition: per-symbol non-GPL EXPORT_SYMBOL support for Rust translation units.
* `bitmap` — `lib/bitmap_export.c` (linkage/metadata): ksymtab entries only (29 symbols, two of them GPL); no algorithm. Exit condition: per-symbol EXPORT_SYMBOL support for Rust translation units upstream.
* `refcount` — `lib/refcount_export.c` (linkage/metadata): ksymtab entries only (six symbols); no algorithm. Exit condition: per-symbol EXPORT_SYMBOL support for Rust translation units upstream.

## 5. Remaining migration candidates

| Component | Status | Dependencies | Risks |
| --- | --- | --- | --- |
| `crc-itu-t` | candidate | - | Shares the upstream CRC KUnit harness; low risk. |
| `crc8` | candidate | - | API takes caller-owned 256-byte tables; Rust must not impose alignment or aliasing assumptions beyond the C contract. |
| `crc7` | candidate | - | - |
| `crc32` | candidate-later | CONFIG_CRC32_ARCH (x86 SSE4.2/PCLMUL) | Hot path (ext4, btrfs, networking); must not regress throughput. Arch-accelerated variants are selected by alternatives/patching; aggressive; keep C until SIMD abstraction exists. |
| `crc64` | candidate-later | CONFIG_CRC64_ARCH (x86 PCLMUL) | Generated table via hostprog; Rust would need a const table or build-time generation to preserve behaviour. |
| `percpu-counter` | candidate-blocked | - | Requires a kernel::percpu abstraction (alloc_percpu, this_cpu_ptr) that does not exist in this tree. |
| `eventfd` | candidate-blocked | - | Requires anon_inode_getfile() file operations and poll() support in the Rust abstraction layer, which does not exist in this tree. |

## 6. Measured build/runtime regressions

| Benchmark | Reference | Ferret | Delta | Notes |
| --- | --- | --- | --- | --- |
| crc16() text size | 133 bytes (C, x86_64 defconfig) | 147 bytes (Rust, x86_64 defconfig) | +14 bytes (+10.5%) | nm --print-size on the linked vmlinux; runtime throughput NOT TESTED (QEMU TCG only) |
* crc16() code size increased by 14 bytes (+10.5%) versus the C implementation. This is a static footprint measurement, not a runtime measurement.
* Runtime performance of the Rust crc16() has not been measured; no performance claim is made.
* CONFIG_CRC16_RUST requires CRC16=y, so loadable-module CRC16 remains the C implementation.


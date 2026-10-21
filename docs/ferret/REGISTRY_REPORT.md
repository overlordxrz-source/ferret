# Ferret migration registry report

Generated from `registry/migrations.json` by `tools/ferret/registry.py`. Do not edit by hand.

Baseline: `v6.18.57` (`2ff600d29ee478d0a8d46ef8a58afb5e038fe692`)

## 1. Components analysed

| Component | Subsystem | Status | Rust compiled | C replaced |
| --- | --- | --- | --- | --- |
| `crc16` | lib/crc | migrated | yes | yes |
| `crc-ccitt` | lib/crc | candidate | no | no |
| `crc-itu-t` | lib/crc | candidate | no | no |
| `crc8` | lib/crc | candidate | no | no |
| `crc7` | lib/crc | candidate | no | no |
| `crc32` | lib/crc | candidate-later | no | no |
| `crc64` | lib/crc | candidate-later | no | no |

## 2. Components genuinely migrated

* `crc16` — CRC-16 (poly 0x8005, reflected): lib/crc/crc16.c → lib/crc/crc16_rust.rs

## 3. Components tested

| Component | Test | Kind | Result | Evidence |
| --- | --- | --- | --- | --- |
| `crc16` | kunit-crc-suite | upstream-kunit | PASS | `artifacts/rust/boot.log` |
| `crc16` | kunit-crc16-differential | differential-kunit | PASS | `artifacts/rust/boot.log` |
| `crc16` | kunit-crc16-rust-vectors | rust-kunit | PASS | `artifacts/rust/boot.log` |
| `crc16` | qemu-boot-userspace-abi | boot | PASS | `artifacts/rust/boot.log` |
| `crc16` | linkage-ownership | static-analysis | PASS | `artifacts/rust/linkage.txt` |

## 4. Components with remaining C dependencies

* `crc16` — `lib/crc/crc16_export.c` (linkage/metadata): ksymtab entry only, to preserve non-GPL EXPORT_SYMBOL visibility (see DECISIONS D-004)

## 5. Remaining migration candidates

| Component | Status | Dependencies | Risks |
| --- | --- | --- | --- |
| `crc-ccitt` | candidate | - | Two exported symbols share one table; the Rust module must define both with the same visibility. |
| `crc-itu-t` | candidate | - | Shares the upstream CRC KUnit harness; low risk. |
| `crc8` | candidate | - | API takes caller-owned 256-byte tables; Rust must not impose alignment or aliasing assumptions beyond the C contract. |
| `crc7` | candidate | - | - |
| `crc32` | candidate-later | CONFIG_CRC32_ARCH (x86 SSE4.2/PCLMUL) | Hot path (ext4, btrfs, networking); must not regress throughput. Arch-accelerated variants are selected by alternatives/patching; aggressive; keep C until SIMD abstraction exists. |
| `crc64` | candidate-later | CONFIG_CRC64_ARCH (x86 PCLMUL) | Generated table via hostprog; Rust would need a const table or build-time generation to preserve behaviour. |

## 6. Measured build/runtime regressions

| Benchmark | Reference | Ferret | Delta | Notes |
| --- | --- | --- | --- | --- |
| crc16() text size | 133 bytes (C, x86_64 defconfig) | 147 bytes (Rust, x86_64 defconfig) | +14 bytes (+10.5%) | nm --print-size on the linked vmlinux; runtime throughput NOT TESTED (QEMU TCG only) |
* crc16() code size increased by 14 bytes (+10.5%) versus the C implementation. This is a static footprint measurement, not a runtime measurement.
* Runtime performance of the Rust crc16() has not been measured; no performance claim is made.
* CONFIG_CRC16_RUST requires CRC16=y, so loadable-module CRC16 remains the C implementation.


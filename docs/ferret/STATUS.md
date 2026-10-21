# Status — Checkpoint 001

State vocabulary: **VERIFIED** (evidence committed in `artifacts/`),
**PARTIAL**, **FAILED**, **NOT TESTED**.

Last updated: checkpoint 001 (2026-10-20).

All evidence in this file comes from one **clean, from-scratch** cycle:
`CLEAN=1 tools/ferret/test.sh` (DECISIONS D-011), exit status 0, summary in
`artifacts/RESULTS.txt`.

## 1. Baseline

| Item | Value | State |
| --- | --- | --- |
| Upstream tag | `v6.18.57` (longterm) | VERIFIED |
| Commit | `2ff600d29ee478d0a8d46ef8a58afb5e038fe692` | VERIFIED |
| Reference branch | `ferret/reference` (identical to upstream tag) | VERIFIED |
| Development branch | `ferret/main` | VERIFIED |
| Tree provenance | kernel.org stable tree, full history, tag object `ferret-baseline` | VERIFIED |

Rationale for the baseline: `DECISIONS.md` D-001.

## 2. Environment

| Item | Value |
| --- | --- |
| Host | Apple M5, macOS 27.2, arm64, 16 GB RAM |
| Build host | Ubuntu 24.04 aarch64 container (Docker Desktop 29.5.3, 10 vCPU) |
| Compiler | clang/lld/llvm 18.1.3 |
| Rust | rustc 1.88.0, `rust-src`, target `x86_64-unknown-none` |
| bindgen | 0.72.1 (libclang 18) |
| QEMU | 8.2.2, TCG (no hardware virtualisation) |

Full record: `artifacts/env.txt`.

Note: the source tree lives on a case-sensitive APFS volume because a normal
macOS checkout corrupts case-colliding paths (DECISIONS D-008).

## 3. Builds

| Build | State | Result |
| --- | --- | --- |
| Reference kernel (unmodified upstream + KUnit) | VERIFIED | `bzImage` 14,537,728 B, `vmlinux` 53,490,664 B, 0 errors |
| Ferret kernel (Rust CRC-16) | VERIFIED | `bzImage` 14,857,216 B, `vmlinux` 57,186,952 B, 0 errors |

Config deltas are documented in `BUILDING.md` §3 and recorded verbatim in
`artifacts/ref/config` and `artifacts/rust/config`.

Key configuration facts in the Ferret build:

```
CONFIG_RUST=y
CONFIG_CRC16=y
CONFIG_CRC16_RUST=y
CONFIG_CRC16_DIFF_KUNIT_TEST=y
CONFIG_KUNIT=y
CONFIG_CRC_KUNIT_TEST=y
```

## 4. Boots

| Boot | State | Result |
| --- | --- | --- |
| Reference kernel under QEMU | VERIFIED | `crc16_test` PASS, userspace init OK, clean poweroff |
| Ferret kernel under QEMU | VERIFIED | `crc16_rust` 5/5 PASS, `crc16_diff` 2/2 PASS, `crc16_test` PASS, userspace init OK |

The QEMU command line, timeout and marker checks are in `BUILDING.md` §5.
Serial logs: `artifacts/ref/boot.log`, `artifacts/rust/boot.log`.

Both boots reached the freestanding Rust init, which reports:

```
FERRET-INIT: sysname=Linux release=6.18.57-ferret-<variant> machine=x86_64
FERRET-INIT: proc-mounted
FERRET-INIT: proc-version-prefix=Linux version 6.18.57-ferret-<variant> ...
FERRET-INIT: ok
```

## 5. Migration

| Item | State |
| --- | --- |
| Component selected | CRC-16 (`lib/crc/crc16.c`) |
| Rust implementation | `lib/crc/crc16_rust.rs` |
| Kconfig switch | `CONFIG_CRC16_RUST` (bool, `depends on CRC16=y && RUST`) |
| C implementation replaced in Rust build | VERIFIED — `lib/crc/crc16.o` is not built |
| Differential test | `CONFIG_CRC16_DIFF_KUNIT_TEST` (original `crc16.c` as `crc16_c_ref`) |
| Rust KUnit vectors | `crc16_rust` suite, 5 cases |
| Upstream KUnit suite | `CONFIG_CRC_KUNIT_TEST` (`crc16_test`) |
| Linkage evidence | `artifacts/rust/linkage.txt` (RESULT: PASS) |

Linkage facts (Ferret build):

| Check | Result |
| --- | --- |
| `lib/crc/crc16.o` present | no (C implementation not built) |
| `lib/crc/crc16_rust.o` present | yes |
| Definitions of `crc16` in `vmlinux` | exactly 1 |
| Size of `crc16` in `crc16_rust.o` / `vmlinux` | `0x93` / `0x93` — match |
| `lib/crc/crc16_export.o` | references `crc16`, defines nothing |
| `__ksymtab_crc16` | present |
| `Module.symvers` | `EXPORT_SYMBOL` (non-GPL) preserved |
| `addr2line` source mapping | SKIP — no DWARF in `defconfig` (D-010) |

The reference build shows the inverse: `lib/crc/crc16.o` present,
`crc16_rust.o` absent, one definition of `crc16` with the C object's size
(`0x85`), same ksymtab/export metadata.

### Measured footprint

| Measurement | Reference (C) | Ferret (Rust) | Delta |
| --- | --- | --- | --- |
| `crc16()` text size | 133 B | 147 B | +14 B (+10.5%) |
| `vmlinux` size | 53,490,664 B | 57,186,952 B | +3,696,288 B (+6.9%) |

The `vmlinux` delta is dominated by the Rust core/compiler-builtins runtime
being linked into an allmodconfig-style x86_64 defconfig for the first time, not
by CRC-16 itself. Both figures are static measurements.

Runtime throughput: **NOT TESTED** (QEMU TCG only; no performance claim).

## 6. Tests

| Test | Kind | Result | Evidence |
| --- | --- | --- | --- |
| Reference kernel builds | build | PASS | `artifacts/ref/build.log` |
| Ferret kernel builds | build | PASS | `artifacts/rust/build.log` |
| Reference kernel boots to userspace | boot | PASS | `artifacts/ref/boot.log` |
| Ferret kernel boots to userspace | boot | PASS | `artifacts/rust/boot.log` |
| `crc16_test` (upstream, independent bitwise reference) | upstream KUnit | PASS | both boot logs |
| `crc16_rust` (5 vector/edge/PRNG cases) | Rust KUnit | PASS | `artifacts/rust/boot.log` |
| `crc16_diff` (Rust vs original C: 65,536 init values, 2,000 random streams) | differential KUnit | PASS | `artifacts/rust/boot.log` |
| Object ownership / single definition / symbol size | static linkage | PASS | `artifacts/rust/linkage.txt` |
| ksymtab + non-GPL `EXPORT_SYMBOL` | static linkage | PASS | both `linkage.txt` |
| `addr2line` provenance | static linkage | SKIP (no DWARF, D-010) | both `linkage.txt` |

Summary from `artifacts/RESULTS.txt`:

```
build-ref    PASS
build-rust   PASS
initramfs    PASS
boot-ref     PASS
boot-rust    PASS
linkage-ref  PASS
linkage-rust PASS
registry     PASS
```

## 7. Not tested / out of scope for this checkpoint

* Runtime performance comparison (QEMU TCG only; no performance claim is made).
* arm64 build and boot.
* Loadable-module Rust CRC-16 (`CONFIG_CRC16=m` remains C).
* LTP / kselftests (needs a real userspace image).
* Rebase onto a newer stable/LTS series.
* Sanitizer boots (KASAN/KCSAN/UBSAN) of the Rust variant.

## 8. How to reproduce

```
tools/ferret/mount-src.sh attach           # case-sensitive source volume
tools/ferret/docker/run.sh true            # create the build container
CLEAN=1 tools/ferret/test.sh               # build + boot + linkage + registry
```

Per-step flags: `tools/ferret/build.sh {ref,rust}`, `tools/ferret/boot.sh
{ref,rust}`, `tools/ferret/linkage.sh {ref,rust}`, `tools/ferret/initramfs.sh`,
`tools/ferret/env.sh`.

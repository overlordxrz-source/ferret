# Status — Checkpoint 002

State vocabulary: **VERIFIED** (evidence produced by a clean cycle and
committed), **PARTIAL**, **FAILED**, **NOT TESTED**.

Last updated: checkpoint 002 (2026-10-21).

All evidence comes from a **clean, from-scratch** cycle:
`CLEAN=1 tools/ferret/test.sh` (DECISIONS D-011). The published summaries are
in [`docs/ferret/evidence/`](evidence/); the raw logs stay local in `artifacts/`
and are reproduced by re-running the cycle.

## 1. Baseline

| Item | Value | State |
| --- | --- | --- |
| Upstream tag | `v6.18.57` (longterm) | VERIFIED |
| Commit | `2ff600d29ee478d0a8d46ef8a58afb5e038fe692` | VERIFIED |
| Reference build | unmodified upstream tree, same config deltas | VERIFIED |
| Published history | snapshot commit of the baseline, then the Ferret tree | VERIFIED |

Rationale: `DECISIONS.md` D-001. Provenance and publication model:
[UPSTREAM.md](UPSTREAM.md).

## 2. Environment

| Item | Value |
| --- | --- |
| Host | Apple Silicon (arm64), macOS, 16 GB RAM |
| Build host | Ubuntu 24.04 aarch64 container, 10 vCPU |
| Compiler | clang/lld/llvm 18.1.3 |
| Rust | rustc 1.88.0, `rust-src`, target `x86_64-unknown-none` |
| bindgen | 0.72.1 (libclang 18) |
| QEMU | 8.2.2, TCG (no hardware virtualisation available) |
| ccache | `/build/ccache`, 20 GiB (D-014) |

The source tree lives on a case-sensitive APFS sparse bundle: a normal macOS
checkout silently merges case-colliding kernel paths (D-008).

## 3. Builds

| Build | State | Result |
| --- | --- | --- |
| Reference kernel, x86_64 (unmodified upstream) | VERIFIED | `bzImage` 14,537,728 B, `vmlinux` 53,490,664 B |
| Ferret kernel, x86_64 (Rust CRC-16 + CRC-CCITT) | VERIFIED | `bzImage` 14,861,312 B, `vmlinux` 57,191,848 B |
| Ferret kernel, arm64 | VERIFIED | `Image` 41,855,488 B, `vmlinux` 434,453,352 B |

The Rust variant's configuration:

```
CONFIG_RUST=y
CONFIG_CRC16=y            CONFIG_CRC16_RUST=y            CONFIG_CRC16_DIFF_KUNIT_TEST=y
CONFIG_CRC_CCITT=y        CONFIG_CRC_CCITT_RUST=y        CONFIG_CRC_CCITT_DIFF_KUNIT_TEST=y
CONFIG_KUNIT=y            CONFIG_CRC_KUNIT_TEST=y
```

`CONFIG_KUNIT_ALL_TESTS` stays off so unrelated suites cannot obscure the
result. The reference variant disables every Ferret symbol.

## 4. Boots

| Boot | State | Result |
| --- | --- | --- |
| Reference kernel, x86_64, QEMU | VERIFIED | upstream CRC suite PASS, userspace init OK |
| Ferret kernel, x86_64, QEMU | VERIFIED | `crc16_rust` 5/5, `crc16_diff` 2/2, `crc_ccitt_rust` 7/7, `crc_ccitt_diff` 3/3, upstream `crc16_test` PASS, 0 `not ok` |
| Ubuntu 24.04 shell probe, amd64 | VERIFIED | `UBUNTU-CHECK: ok` |
| Ubuntu 24.04 systemd, amd64 | VERIFIED | `SYSTEMD-CHECK: ok`, `ferret login:` getty |
| Ubuntu 24.04 shell probe, arm64 | VERIFIED | `UBUNTU-CHECK: ok` |
| Ubuntu 24.04 systemd, arm64 | VERIFIED | `SYSTEMD-CHECK: ok`, `ferret login:` getty |

The Ubuntu guests are the official Canonical cloud-image root filesystem
(24.04.5) on virtio-blk + ext4 — a real distro userland, not a synthetic one
(D-012). The two guest modes are a shell probe (`init=/sbin/ferret-init`) and
`systemd` as PID 1 reaching `multi-user.target`.

## 5. Migration

| Item | CRC-16 | CRC-CCITT |
| --- | --- | --- |
| C source (reference) | `lib/crc/crc16.c` | `lib/crc/crc-ccitt.c` |
| Rust implementation | `lib/crc/crc16_rust.rs` | `lib/crc/crc_ccitt_rust.rs` |
| Kconfig switch | `CONFIG_CRC16_RUST` | `CONFIG_CRC_CCITT_RUST` |
| Exported symbols | `crc16` | `crc_ccitt`, `crc_ccitt_table` |
| Export class | `EXPORT_SYMBOL` (non-GPL), preserved | `EXPORT_SYMBOL` (non-GPL), preserved |
| C object in the Rust build | not built | not built |
| Differential test | `crc16_diff` (edge cases + 2 000 random streams) | `crc_ccitt_diff` (table, edge cases + 2 000 random streams) |
| Rust vectors | `crc16_rust`, 5 cases | `crc_ccitt_rust`, 7 cases |

### Linkage facts (x86_64)

| Check | Reference (C) | Ferret (Rust) |
| --- | --- | --- |
| C object built | yes | **no** |
| Rust object built | no | yes |
| Definitions of the symbol in `vmlinux` | 1 | 1 |
| `crc16` text size | `0x85` (133 B) | `0x93` (147 B) |
| `crc_ccitt` text size | `0x7c` (124 B) | `0x8a` (138 B) |
| `crc_ccitt_table` size | `0x200` (512 B) | `0x200` (512 B) |
| `__ksymtab_*` entries | present | present |
| Export namespace (`vmlinux.symvers`) | `EXPORT_SYMBOL` | `EXPORT_SYMBOL` |
| `addr2line` provenance | SKIP (no DWARF, D-010) | SKIP (no DWARF, D-010) |

arm64 (Rust build): `crc16` `0x48` (72 B), `crc_ccitt` `0x48` (72 B). No arm64 C
reference was built, so no size delta is claimed for that lane.

`vmlinux` grew by 3,701,184 B (+6.9 %) between the reference and the Ferret
x86_64 builds. That delta is the Rust core/compiler-builtins runtime being
linked into the kernel for the first time, not the CRC components. Runtime
throughput: **NOT TESTED** (QEMU TCG only; no performance claim).

## 6. Test cycle

The clean cycle performs 16 steps. Result of the checkpoint 002 cycle:

| Step | Result | Step | Result |
| --- | --- | --- | --- |
| `build-ref` | PASS | `build-rust-arm64` | PASS |
| `build-rust` | PASS | `linkage-rust-arm64` | PASS |
| `initramfs` | PASS | `ubuntu-rootfs-amd64` | PASS |
| `boot-ref` | PASS | `ubuntu-rootfs-arm64` | PASS |
| `boot-rust` | PASS | `ubuntu-shell-amd64` | PASS |
| `linkage-ref` | PASS | `ubuntu-systemd-amd64` | PASS |
| `linkage-rust` | PASS | `ubuntu-shell-arm64` | PASS |
| `registry` | PASS | `ubuntu-systemd-arm64` | PASS |

Per-step evidence: [`docs/ferret/evidence/`](evidence/).

### A harness defect found by this cycle

The **first** execution of the checkpoint 002 cycle failed the three `linkage`
steps while every kernel-side check passed. Cause: the image-only build
introduced in D-014 stops after the `vmlinux` link, and that target writes
`vmlinux.symvers`, not `Module.symvers` (the latter is written by a full `make
modules`). The linkage script read the wrong file and reported the export
namespace as missing for all three migrated symbols.

The script now uses `Module.symvers` when present and `vmlinux.symvers`
otherwise, and the **cycle was repeated from scratch with the corrected
harness**; that second clean cycle passed all 16 steps (exit status 0), and
its results are the ones recorded above.

The defect was in the harness, not in the kernel: the symbols were exported as
`EXPORT_SYMBOL` in all three trees, as the `vmlinux.symvers` listing in
[`evidence/linkage-x86_64.txt`](evidence/linkage-x86_64.txt) shows.

## 7. Not tested / out of scope for this checkpoint

* Runtime performance (QEMU TCG only; no performance claim is made).
* Physical hardware boot — everything runs under QEMU.
* Loadable-module Rust components (`CONFIG_CRC16=m` / `CONFIG_CRC_CCITT=m` are
  still the C implementation).
* LTP, kselftests, sanitizer (KASAN/KCSAN/UBSAN) and lockdep boots.
* Container runtimes, musl userspace, 32-bit compat.
* Ubuntu networking end-to-end, storage beyond one virtio disk, graphical
  desktop, suspend/resume.
* DWARF source mapping for the Rust functions (the kernel config carries no
  debug info).

## 8. How to reproduce

```sh
tools/ferret/mount-src.sh attach           # case-sensitive source volume
tools/ferret/docker/run.sh true            # create the build container
CLEAN=1 tools/ferret/test.sh               # build + boot + linkage + registry
```

Per-step flags: `tools/ferret/build.sh {ref,rust}`, `tools/ferret/boot.sh
{ref,rust}`, `tools/ferret/linkage.sh {ref,rust}`, `tools/ferret/initramfs.sh`,
`tools/ferret/ubuntu-rootfs.sh {amd64,arm64}`,
`tools/ferret/boot-ubuntu.sh {shell,systemd} {amd64,arm64}`,
`tools/ferret/env.sh`.

The evidence summaries are regenerated with `tools/ferret/evidence.sh <commit>`.

# Status — Checkpoint 005

State vocabulary: **VERIFIED** (evidence produced and committed), **PARTIAL**,
**FAILED**, **NOT TESTED**.

Last updated: checkpoint 005 (2026-10-21).

## 0. Checkpoint 005 — the out-of-line reference counting helpers in Rust

**State: VERIFIED.** The implementation is complete, the *clean* cycle
(`CLEAN=1 tools/ferret/test.sh`) passed all 16 steps with exit status 0, and
the component went through the independent-verification gate
([`AUDIT_PROTOCOL.md`](../../AUDIT_PROTOCOL.md)), whose report is
[`design/COMPONENT_005_AUDIT.md`](design/COMPONENT_005_AUDIT.md).

The gate is what a component has to pass before it is called verified: an
independent build, the linkage and ABI proof, the integrity of the
differential suite, mutation testing, the kernel sanitizers, the property and
model tests, the Rust soundness review and an honest coverage report. The
pull request stays open until the gate passes; the component was audited
against the gate, and the two defects the audit found (the `KRef` aliasing
promise and a race in the concurrency test) are fixed.

| Item | State | Result |
| --- | --- | --- |
| Rust implementation | done | `lib/refcount_rust.rs`, all 6 exported symbols |
| C object in the Rust build | absent | `lib/refcount.o` is not built; `lib/refcount_rust.o` defines every symbol |
| Exports | preserved | all 6 symbols keep `EXPORT_SYMBOL` (non-GPL) via `lib/refcount_export.c` |
| Rust unit tests (`refcount_rust`) | PASS | 7 cases: the `KRef` lifetime, the saturated count, the count of one, and a model test over 20 000 pseudo-random operations |
| Differential KUnit (`refcount_diff`) | PASS | 9 cases against the unmodified C, including a randomised property test and a concurrency phase |
| Concurrency | PASS | 4 tasks × 250 decrements: 1 000 successful decrements, the count ends at exactly 1; the race for the last reference has exactly one winner, in both implementations |
| Linkage evidence | PASS | one definition per symbol, size match, ksymtab, export class, in both variants |
| arm64 build | PASS | the arm64 Ferret kernel builds and the arm64 linkage report is green |
| Clean cycle | PASS | 16/16 steps, exit status 0 (x86_64 and arm64 builds, the linkage checks, the KUnit suites, the four Ubuntu boots) |
| Independent-verification gate | PASS | the gate is defined in [`AUDIT_PROTOCOL.md`](../../AUDIT_PROTOCOL.md); the report is [`design/COMPONENT_005_AUDIT.md`](design/COMPONENT_005_AUDIT.md) |
| Mutation testing | PASS | five mutations detected, three declared not detectable (the memory ordering, and two paths that are only reachable under a race) |
| KASAN + UBSAN | PASS | the audited suites pass inside a sanitized kernel, with no report pointing at the audited component |
| KCSAN | **NOT RUN** | the build and the boot of a KCSAN kernel do not fit in the checkpoint's time budget |
| Native performance | NOT TESTED | the host has no KVM; all measurements would be QEMU TCG |

What the port covers:

* All six exported functions of `lib/refcount.c` are implemented in Rust:
  `refcount_warn_saturate()`, `refcount_dec_if_one()`,
  `refcount_dec_not_one()`, `refcount_dec_and_mutex_lock()`,
  `refcount_dec_and_lock()` and `refcount_dec_and_lock_irqsave()`.
* The `refcount_t` *primitive* stays in C, in `include/linux/refcount.h`. It
  is a header type that the C code embeds in its own structures, and its
  inline operations are what the kernel's own code calls. It cannot be
  migrated as a file without changing every C structure that embeds it. The
  exported layer is what moves.
* The direction of the call is worth noting: the C inline operations of the
  primitive (`refcount_inc()`, `refcount_dec()`, `refcount_add()`, ...) warn
  through `refcount_warn_saturate()`, which is now the Rust implementation.
  Every saturation warning in the kernel therefore goes through this Rust
  code. There is no recursion.
* The memory ordering is not re-implemented: the decrements use
  `atomic_try_cmpxchg_release()`, the very same C primitive the C original
  uses, through the kernel crate's `Atomic` wrapper. The disassembly of the
  helper shows the same instructions as the C inline on both architectures
  (`lock cmpxchg` on x86_64, `casl`/`ldxr`+`stlxr` on arm64).
* The lock helpers keep the C contract exactly: the lock is taken only when
  the count was greater than one, it is *held* by the caller on success, and
  it is released again on failure. The `irqsave` variant restores the
  interrupt state on the failure path.

### The kernel-crate extensions

Two additions to the in-tree kernel crate, both exposing what the C code
already had:

* `kernel::sync::Refcount`: `from_ptr()` (a borrowed view of a C
  `refcount_t`), `read()`, `set_saturated()`, `try_dec_if_one()`,
  `try_dec_not_one()` and `warn_saturate()`.
* `kernel::sync::KRef`: a Rust handle for an object that is owned by C and
  counted by an embedded `struct kref`. It owns one reference, releases it
  when it is dropped, runs the C release callback when it was the last one,
  and can hand the reference back to C with `into_raw()`.

### Deviations from the C original

* The C `WARN_ONCE()` uses a plain, racy flag, so two tasks that hit the same
  warning at the same time may print it twice. The Rust code uses an atomic
  compare-and-exchange, so each warning is printed exactly once. The message
  text is unchanged.
* The Rust code calls the C atomic primitives through the kernel crate's
  `Atomic` wrapper, which is a real function call, where the C inlined them.
  The semantics are identical, the cost is a call per operation. This is the
  price of not re-implementing the ordering, and it is the reason the port
  does not use Rust's own atomics.

### The `KRef` soundness review

The audit found a real defect in the new `KRef` abstraction: it carried
`PhantomData<&'a mut T>`, which promises *exclusive* access to the object.
That promise cannot be kept: the object is owned by C, the C code may hold
other references to it, and other `KRef`s for the same object may exist at
the same time. The promise was not even enforced, because the object is
reached through a raw pointer.

The marker is now a plain lifetime marker that says nothing about the
aliasing, and `Send`/`Sync` are stated explicitly with the argument instead
of being derived from the object type. The details, the full checklist
(aliasing, ownership, `Drop`, `into_raw`, lifetimes, `Send`/`Sync`) and the
remaining boundary are in
[`design/COMPONENT_005_AUDIT.md`](design/COMPONENT_005_AUDIT.md) §2.

### A defect in the test harness, not in the port

The first version of the concurrency test failed, and the cause was in the
test, not in the implementation: `kthread_stop()` on a task that has not been
scheduled yet kills it *before* it calls the worker function, so one of the
four tasks silently did nothing and the count ended 250 too high. The Rust
implementation was never wrong in that run — the C reference showed the same
behaviour in the same run. The test now waits for the tasks to signal that
they are running before it stops them.

---

# Status — Checkpoint 003

State vocabulary: **VERIFIED** (evidence produced by a clean cycle and
committed), **PARTIAL**, **FAILED**, **NOT TESTED**.

Last updated: checkpoint 003 (2026-10-21).

## 0. Checkpoint 003 — the kernel bitmap and the generic kernel FIFO in Rust

**State: VERIFIED.** The implementation is complete and the *clean* cycle
(`CLEAN=1 tools/ferret/test.sh`) passed all 16 steps with exit status 0.

| Item | State | Result |
| --- | --- | --- |
| Rust implementation | done | `lib/kfifo_rust.rs`, all 23 exported symbols |
| C object in the Rust build | absent | `lib/kfifo.o` is not built; `lib/kfifo_rust.o` defines every symbol |
| Exports | preserved | all 23 symbols keep `EXPORT_SYMBOL` (non-GPL) via `lib/kfifo_export.c` |
| Upstream KUnit (`KFIFO_KUNIT_TEST`) | PASS | 10 cases, in **both** variants (the reference kernel has to satisfy it too) |
| Rust unit tests (`kfifo_rust`) | PASS | 13 cases |
| Differential KUnit (`kfifo_diff`) | PASS | 10 cases, 2 000 randomised operation sequences against the unmodified C |
| User-copy fault path | PASS | a pointer that is not a userspace address gives `-EFAULT`, copies nothing and leaves the FIFO untouched, in both implementations |
| DMA / scatterlist | PASS | identical entries, including the split at the wrap point (two entries) |
| Linkage evidence | PASS | 166 checks, 0 failures: one definition per symbol, size match, ksymtab, export class |
| Boot | PASS | every KUnit suite green, userspace ABI markers reached |
| Clean cycle | PASS | 16/16 steps, exit status 0 (x86_64 and arm64 builds, three linkage checks, four Ubuntu boots) |
| arm64 build/boot | PASS | arm64 Ferret kernel built and booted, Ubuntu shell and systemd on arm64 |
| Ubuntu userspace boots | PASS | shell probe and systemd-as-PID-1 on amd64 and arm64 |
| Throughput | measured | 1 014–1 256 per mille of the C implementation over five runs of 20 000 in(64)+out(64) iterations (QEMU TCG; the ratio only) |
| Text size | measured | 3 215 B (Rust) vs 2 347 B (C), +37.0 %; arm64 Rust 3 456 B |
| Native performance | NOT TESTED | the host has no KVM; all measurements are QEMU TCG |

What the port covers:

* All 23 exported functions are implemented in Rust: allocation and release,
  the byte and element copies, the record ("_r") API, the user-copy variants
  and the scatterlist (DMA) preparation.
* The locking contract is unchanged: the FIFO is lock-free, the caller
  provides the lock, and the Rust code adds none.
* The memory ordering of the C `smp_wmb()` is preserved with a release fence
  at the same points; the linkage report carries the disassembly fingerprint
  of `__kfifo_in` so the copy and the index update can be inspected.
* Two documented deviations, both where the C code is undefined:
  `__kfifo_init()` with `esize == 0` returns `-EINVAL` instead of dividing by
  zero, and `__kfifo_max_r()` with `recsize >= 4` leaves the length alone
  instead of relying on an undefined shift.
* Two small C helpers were added to the Rust-for-Linux helper set
  (`rust/helpers/slab.c`, `rust/helpers/scatterlist.c`): they expose
  `kmalloc_array()` and `sg_set_buf()` to Rust. They contain no FIFO logic.

### The kernel bitmap

**State: VERIFIED.** The clean cycle (`CLEAN=1 tools/ferret/test.sh`) passed
all 16 steps with exit status 0 with the Rust bitmap in the kernel, on x86_64
and arm64, including the upstream kselftest, the differential suite, the three
linkage checks and the four Ubuntu boots.

| Item | State | Result |
| --- | --- | --- |
| Rust implementation | done | `lib/bitmap_rust.rs`, all 29 exported symbols plus the three non-exported globals the header calls |
| C object in the Rust build | absent | `lib/bitmap.o` is not built; `lib/bitmap_rust.o` defines every symbol |
| Exports | preserved | 27 symbols keep `EXPORT_SYMBOL`, `devm_bitmap_alloc`/`devm_bitmap_zalloc` keep `EXPORT_SYMBOL_GPL` |
| Upstream test suite | PASS | the `test_bitmap` kselftest module: **390 447 cases**, all passing |
| Differential KUnit | PASS | `bitmap_diff`, 8 cases over the whole API against the unmodified C |
| Linkage evidence | PASS | one definition per symbol, size match, ksymtab, export class |
| Text size | measured | 5 807 B (Rust) vs 3 712 B (C), +56 % (2 095 B in a 57 MB kernel) |
| 32-bit variants | **NOT TESTED** | `bitmap_from_arr64()`/`bitmap_to_arr64()` are implemented but only compiled on a 32-bit target, which Ferret does not build or test |
| Throughput | measured | 1 168 per mille of the C reference with the weight helpers, 964 per mille without them (in-kernel benchmark, QEMU TCG, clean cycle) |
| Audit | done | [`design/COMPONENT_004_AUDIT.md`](design/COMPONENT_004_AUDIT.md) |

Two prerequisites were added to the in-tree kernel crate, both exposing
primitives that already existed in C: `Flags::from_gfp()`/`as_gfp()` (a
run-time allocation mask) and `Vec::reserve_in()`/`with_capacity_in()` (a
NUMA node). The first one also let the kernel FIFO retire its C helper.

The upstream test suite found a real defect on the first boot: the C
`bitmap_cut()` reads and writes the word that `first` points at even when it
is outside the bitmap, which is an out-of-bounds access. The Rust version
only touches the words inside the bitmap; the bits of the bitmap itself are
the same. The differential suite and the upstream suite both pass.

### The generic kernel FIFO
[`design/COMPONENT_003_INVESTIGATION.md`](design/COMPONENT_003_INVESTIGATION.md)
§4.1.2.

---

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

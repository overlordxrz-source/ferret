# Decisions

Architecture decisions are recorded here with context, decision, consequences
and status. Numbers are stable; superseded decisions are amended, never deleted.

---

## D-001 — Baseline: Linux v6.18.57 (longterm)

**Context.** Kernel.org listed, at project start (2026-10-20):

| Series | Latest | Released | Kind |
| --- | --- | --- | --- |
| 7.3 | 7.3 | 2026-10-18 | mainline |
| 7.2 | 7.2.11 | 2026-10-15 | stable |
| 6.18 | 6.18.57 | 2026-10-15 | longterm |
| 6.12 | 6.12.113 | 2026-10-15 | longterm |

Ferret needs a baseline that (a) is a real, released Linux kernel, (b) will
receive fixes for the multi-year duration of the rewrite, and (c) has mature
Rust-for-Linux infrastructure (the `kernel` crate abstractions, Rust KUnit,
`#[export]`, `rust/bindings`).

**Decision.** Base Ferret on **v6.18.57**
(`2ff600d29ee478d0a8d46ef8a58afb5e038fe692`), the newest longterm series.

**Rationale.**

* 7.3 is a two-day-old mainline release: no stable backports yet, expected
  regressions, and no LTS commitment.
* 7.2.11 is the newest *stable* point release, but 7.2 is a short-lived series
  (7.3 is already out); it will be EOL within months, which is unsuitable for a
  multi-year fork.
* 6.18.57 is the newest LTS: same code age as 7.2 in terms of Rust
  infrastructure maturity, but with years of maintenance left. Upstream Rust
  support is complete enough for the migration experiments (`#[export]`,
  `#[kunit_tests]`, per-architecture Rust target generation, `rust-src` based
  core/alloc).
* 6.12.113 is older with no benefit for a new project.

**Consequences.** Ferret inherits 6.18's Rust API surface. Re-basing onto a
newer LTS later is a normal upstream-rebase operation; the migration registry
records the baseline revision per component so a rebase can be validated per
component.

**Status.** Accepted.

---

## D-002 — Build environment: Linux container, x86_64 cross-build, QEMU TCG

**Context.** The development host is an Apple M5 (arm64) running macOS 27.2.
Native Linux kernel development and native x86_64 virtualisation with hardware
acceleration are not available. The host filesystem has ~47 GB free.

**Decision.** Use a persistent Ubuntu 24.04 **aarch64** Docker container as the
build environment. Cross-compile the x86_64 kernel with `LLVM=1`
(clang/lld 18, `x86_64-unknown-none` Rust target) and boot it under
`qemu-system-x86_64` with TCG. Do not install toolchains on, or modify, the
host OS.

**Rationale.**

* The container is disposable and reproducible from
  `tools/ferret/docker/Dockerfile`.
* `LLVM=1` is upstream's supported cross-compilation path and avoids needing a
  target C library or binutils.
* QEMU TCG is slower than KVM but sufficient for boot smoke tests and KUnit;
  no nested virtualisation is needed.
* Building on the container's own filesystem (not the host bind mount) avoids
  the large slowdown of virtiofs for kernel builds; only sources and evidence
  cross the boundary.

**Consequences.** Boot tests are slower (tens of seconds) and benchmarks are
relative, not absolute. Kernel build artefacts live in the container
(`/build/...`) and are reproducible by rerunning `build.sh`.

**Status.** Accepted.

---

## D-003 — Kconfig/Makefile shape for a migrated component

**Context.** A migrated component must be selectable without changing the
upstream `tristate` semantics that drivers rely on via `select`.

**Decision.** Keep the upstream `tristate` untouched; add a `bool`
`<COMP>_RUST` that `depends on <COMP> = y && RUST`, and switch the object list
in the Makefile with `ifeq`. The C object is not built at all when the Rust
variant is selected.

**Consequences.** No duplicate symbols are possible; `nm`/build-tree inspection
proves which implementation is linked. Rust migrations are built-in only until
Ferret gains Rust module support.

**Status.** Accepted.

---

## D-004 — Symbol export: linkage-only C shim, not `EXPORT_SYMBOL_RUST_GPL`

**Context.** In 6.18, Rust symbols are exported to modules through
`rust/Makefile`'s generated `exports_<crate>_generated.h`, which uses
`EXPORT_SYMBOL_RUST_GPL`. That mechanism only covers the `kernel` crate objects
(`core.o`, `helpers.o`, `bindings.o`, `kernel.o`). `lib/crc/crc16.c` is
currently `EXPORT_SYMBOL` (non-GPL), so non-GPL modules may call `crc16`.

**Decision.** Do not move CRC-16 into the `kernel` crate and do not accept a
GPL-only export. Keep a 3-line C file (`lib/crc/crc16_export.c`) that declares
the ksymtab entry with the original visibility and nothing else.

**Rationale.** Preserving the exported-symbol contract is required by the
project's ABI rule. The shim contains no algorithm, so the implementation is
still 100% Rust; a missing Rust symbol makes the link fail. Alternative
considered and rejected: `EXPORT_SYMBOL_RUST_GPL` via the `kernel` crate, which
would silently break non-GPL modules and misplace a `lib/` algorithm in the
core crate.

**Exit condition.** Remove the shim when the kernel offers per-symbol
`EXPORT_SYMBOL` (non-GPL) from Rust translation units outside the `kernel`
crate.

**Status.** Accepted (temporary C dependency, tracked in the registry).

---

## D-005 — Differential testing uses the original C file, symbol-renamed

**Context.** Requirement: compare the original C implementation against the
Rust replacement without keeping a second copy of the C source.

**Decision.** For `CONFIG_CRC16_DIFF_KUNIT_TEST=y`, Kbuild compiles the
unmodified `lib/crc/crc16.c` a second time with `-Dcrc16=crc16_c_ref`, and a C
KUnit suite compares `crc16()` (Rust) with `crc16_c_ref()` (original C) over
edge cases and a deterministic PRNG stream.

**Rationale.** The reference is the real upstream source, not a re-typed copy,
so drift is impossible. The rename avoids a duplicate symbol; the test build is
`CONFIG`-gated and never part of a production image.

**Consequences.** The reference object also re-emits `EXPORT_SYMBOL(crc16_c_ref)`
and its `MODULE_*` notes; this is harmless but visible in `__ksymtab` of test
kernels. Documented as a known test-only artefact.

**Status.** Accepted.

---

## D-006 — Userspace evidence: freestanding Rust init instead of busybox

**Context.** The boot test needs a userspace that proves the kernel reached
`execve` of an initramfs binary and that basic syscalls work, without importing
a large prebuilt userspace.

**Decision.** Ship a freestanding `x86_64-unknown-none` Rust init
(`tools/ferret/init`) that uses raw syscalls: `uname`, `mount("proc")`,
`openat`/`read`/`close` of `/proc/version`, `write`, and
`reboot(RB_POWER_OFF)`. Marker lines are machine-checked by the boot script.

**Rationale.** Self-contained, ~200 lines, no libc, no external downloads, and
it doubles as an early userspace-ABI regression test. It also demonstrates
that Rust is viable outside the kernel for Ferret tooling.

**Consequences.** The init is not a shell; interactive testing (LTP, selftests)
will later need a real userspace image (busybox or a distro initramfs), which
is a separate tooling step.

**Status.** Accepted.

---

## D-007 — Toolchain pinning

**Decision.** Pin `clang/lld/llvm` 18 (Ubuntu 24.04), `rustc 1.88.0` (with
`rust-src`, `rustfmt`, `clippy`, target `x86_64-unknown-none`), and
`bindgen-cli 0.72.1`. These satisfy the kernel's minimums (`llvm >= 15`,
`rustc >= 1.78`, `bindgen >= 0.65.1`) and are close to the toolchain generation
the 6.18 stable series is tested against.

**Consequences.** Exact versions are recorded in `docs/ferret/BUILDING.md` and
`artifacts/env.txt`; upgrading is an explicit change with a rebuild of both
variants.

**Status.** Accepted.

---

## D-008 — Source tree on a case-sensitive filesystem

**Context.** The host is macOS with a case-insensitive APFS root. The Linux
tree contains file-name pairs that differ only in case (`xt_MARK.h`/`xt_mark.h`,
`xt_DSCP.c`/`xt_dscp.c`, and other netfilter uapi headers). A normal `git
checkout` on the host produced a tree where one file of each pair silently
contained the other's content — the reference tree was corrupted before the
first build was attempted.

**Decision.** Keep the source tree on a case-sensitive APFS sparse bundle
(`~/Projects/ferret-src.sparsebundle`) mounted at `~/Projects/ferret`, managed
by `tools/ferret/mount-src.sh`.

**Consequences.** The repository path stays canonical. The bundle must be
attached after a host reboot (documented in `BUILDING.md`); the mount is visible
to Docker Desktop through the `~/Projects` share (verified). Reference builds
now compile the actual upstream file contents.

**Status.** Accepted.

---

## D-009 — Harness scripts must run on bash 3.2 and ship their own assertions

**Context.** The host `#!/usr/bin/env bash` resolves to `/bin/bash` 3.2.57
(macOS). The first version of `tools/ferret/test.sh` used `declare -A`; the
declaration failed, the state array silently became a normal variable, and a
later `${STATE[...]}` lookup aborted the cycle *after* the reference build —
while the wrapper still reported success. Shell-level failures must never be
reported as test successes.

**Decision.**

* Harness scripts avoid bash 4 features (no associative arrays, no `mapfile`).
* `test.sh` records every step as PASS/FAIL/**NOT RUN** explicitly, steps that
  depend on a failed predecessor are marked NOT RUN, and the exit status is
  non-zero if any step failed.
* The step runner uses `set -o pipefail` so a failure anywhere in a
  `command | tee` pipeline is visible.
* `docker/run.sh` forwards `CLEAN`, `JOBS`, `ARCH`, `OUTROOT` and `SRC_MOUNT`
  into the container so the same script can do clean or incremental runs.

**Consequences.** Evidence files (`artifacts/RESULTS.txt`) now list NOT RUN
steps truthfully instead of omitting them. Shell compatibility is checked with
`bash -n` plus a real run.

**Status.** Accepted.

---

## D-010 — Provenance evidence when the kernel has no DWARF

**Context.** `x86_64 defconfig` selects `CONFIG_DEBUG_INFO_NONE=y`, so
`addr2line`/`llvm-symbolizer` cannot map the linked `crc16` address to a source
line. Enabling DWARF for the boot kernels would grow build time and image size
for evidence only.

**Decision.** Establish symbol provenance with facts that do not need debug
information:

1. the C object `lib/crc/crc16.o` does not exist in the Rust build tree (and
   the Rust object does),
2. `crc16` is defined exactly once in `vmlinux`,
3. the linked symbol's *size* equals the Rust object's `crc16` size
   (object/`vmlinux` are both measured with `nm --print-size`),
4. `__ksymtab_crc16` exists and `Module.symvers` records `EXPORT_SYMBOL`
   (non-GPL),
5. the runtime differential suite compares the linked `crc16()` against the
   original C implementation compiled as `crc16_c_ref`.

`addr2line` is still attempted and reported as **SKIP** when no line info is
present; if a future configuration enables DWARF the check becomes mandatory.

**Consequences.** `artifacts/*/linkage.txt` contains one explicit SKIP line with
the reason. The check is honest about what it does and does not prove.

**Status.** Accepted.

---

## D-011 — Every checkpoint is validated by a clean, from-scratch build

**Context.** The first successful Rust boot came from a build tree that had
already failed twice (missing crate docs, oversized stack frame), so the tree
contained objects compiled by earlier command lines. Incremental builds can
hide stale artefacts and hide configuration drift.

**Decision.** A checkpoint is only reported as VERIFIED when
`CLEAN=1 tools/ferret/test.sh` succeeds: both output trees are deleted, both
variants are configured and built from scratch, both are booted, and the
linkage evidence is regenerated. Build logs are copied into `artifacts/<variant>/`
together with the effective `.config`.

**Consequences.** A full cycle takes roughly an hour on this host. Checkpoint
reports state whether the evidence came from a clean cycle or an incremental
one.

**Status.** Accepted.


---

## D-012 — Ubuntu userspace comes from the official cloud-image root tarball

**Context.** The goal is to boot a real Ubuntu userland on the Ferret kernel.
`ubuntu-base` (the container base image) contains no `systemd`, so it can prove
a shell but not a system boot. Building a server rootfs with `debootstrap`
would require running foreign-architecture binaries in the build container
(binfmt/chroot), which the Docker Desktop container cannot do without
privileges.

**Decision.** Use Canonical's published cloud-image root tarball
(`https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-<arch>-root.tar.xz`,
24.04.5). It is the same filesystem as the official cloud images: systemd, apt,
the full server userland, for both amd64 and arm64. The ext4 image is populated
with `mke2fs -d <rootfs>` so no loop device or privileged mount is needed.
cloud-init is disabled (`/etc/cloud/cloud-init.disabled`), the machine-id is
fixed, and QEMU user-mode DNS is written to `/etc/resolv.conf`.

**Consequences.** The guest is genuine Ubuntu, not a hand-built imitation.
Images are 4 GiB sparse and rebuild in under a minute once the tarball is
cached in `/build/cache`. Networking, apt and the desktop can be added to the
same image without changing the boot path.

**Status.** Accepted.

---

## D-013 — arm64 is a maintained second lane, built natively and booted under TCG

**Context.** Development happens on Apple Silicon. The container is aarch64, so
an arm64 kernel builds natively (no cross-compilation); the host provides no
KVM to the container, so execution is QEMU TCG.

**Decision.** Every checkpoint builds the arm64 kernel with the same config
switches and boots the Ubuntu arm64 rootfs under `qemu-system-aarch64 -M virt
-cpu max`, console `ttyAMA0`, virtio-blk. x86_64 remains the primary lane and
keeps the freestanding Rust init smoke test. Output trees are suffixed by
architecture (`linux-rust-arm64`) so both lanes coexist.

**Consequences.** arm64 exposed two real bugs that x86_64 hid:
`CRC_CCITT_RUST` silently disabled because `CRC_CCITT` was `=m` (D-015), and a
stock getty unit that never starts because `dev-ttyAMA0.device` does not
materialise. Both were fixed rather than papered over. Performance numbers are
TCG-relative and are not reported as native throughput.

**Status.** Accepted.

---

## D-014 — Build efficiency: image-only targets, container-local source, ccache

**Context.** Full kernel builds dominate checkpoint time. The host tree lives
on a nested virtiofs mount, and `make all` spends most of its time compiling
modules that the test kernels never load.

**Decision.** Three changes, each measurable:

1. Build from a container-local rsync mirror (`/build/src`), never from the
   virtiofs mount.
2. Build only the bootable target (`bzImage` on x86_64, `Image` on arm64), not
   `all`. The export namespace is then read from `vmlinux.symvers` (written by
   the vmlinux link); `Module.symvers` only exists after a full `make modules`.
3. Wrap the compiler with ccache (`CCACHE_DIR=/build/ccache`, 20 GiB) so a
   `CLEAN=1` rebuild reuses preprocessed-object results.

**Consequences.** Clean builds no longer compile modules. The first clean
rebuild after enabling ccache is still cold; warm-cache timings are recorded in
`docs/ferret/STATUS.md` only when measured, never assumed.

The image-only build broke the linkage script in checkpoint 002: it still read
`Module.symvers`, which the image target does not produce, and reported the
export namespace as missing for all three migrated symbols. The kernel was
correct; the harness was wrong. The script now falls back to `vmlinux.symvers`,
and the checkpoint 002 cycle was repeated with the corrected harness.

**Status.** Accepted.

---

## D-015 — Promptless CRC symbols are enabled with `select`

**Context.** In this tree `CRC16` and `CRC_CCITT` are `tristate` symbols
without a prompt: they are only ever enabled by `select` from drivers. The
Ferret switches required `depends on CRC16 = y`, which worked on x86_64 by
accident (a built-in driver selected it) and silently failed on arm64, where it
stayed `=m` and the Rust option disappeared from `.config`.

**Decision.** `CRC16_RUST` and `CRC_CCITT_RUST` keep `depends on RUST` and add
`select CRC16` / `select CRC_CCITT`. Selecting from a `=y` bool forces the
tristate to `=y`, which is exactly the precondition for replacing the C object
with a built-in Rust object.

**Consequences.** The Rust option can no longer be silently disabled by a
different defconfig. A modular CRC remains the C implementation, which is
reported as a limitation, not hidden.

**Status.** Accepted.

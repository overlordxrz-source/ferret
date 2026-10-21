# Roadmap

Ferret proceeds in checkpoints. Each checkpoint must leave the tree building
and booting, with evidence committed alongside the code. Two tracks run in
parallel: the **migration track** (replace C with Rust without changing
behaviour) and the **usable-system track** (make the Ferret kernel boot a real
Ubuntu userspace, then grow it toward a desktop).

Status of the tracks is measured, never assumed: see `docs/ferret/STATUS.md`
and `artifacts/RESULTS.txt`.

## Workstreams

The work is organised as twelve overlapping workstreams, not a linear queue.
Progress is counted in verified components, never in a percentage of Linux.

| # | Workstream | State |
| --- | --- | --- |
| 0 | Build infrastructure and reproducible testing | **active** — container, QEMU harness, registry, CI definitions |
| 1 | Kernel libraries and algorithms | **active** — CRC-16 and CRC-CCITT migrated |
| 2 | Rust kernel interfaces and ownership primitives | **next** — `kfifo` is the next migration; per-CPU, allocation and file-operations gaps are documented |
| 3 | Drivers and device infrastructure | planned — first feasibility study is DRV-VIRTIO-001 |
| 4 | Filesystems and VFS | planned — dependency analysis is FS-VFS-001 |
| 5 | Memory management | planned — allocation analysis is MM-ALLOC-001 |
| 6 | Processes and scheduling | planned — after refcount/task-handle work |
| 7 | Networking | planned — data-structure analysis is NET-SKB-001 |
| 8 | Security and isolation | continuous — unsafe review, export visibility, coordinated disclosure |
| 9 | Architecture-specific code | x86_64 primary, arm64 maintained second lane |
| 10 | Complete userspace compatibility and hardware coverage | long-term — shell and systemd on Ubuntu 24.04 are the current proof point |
| 11 | Elimination of transitional C implementations | continuous — a component is done when the C file can leave the tree, not just the build |

The machine-readable registry (`registry/migrations.json`) and
[MIGRATION_MATRIX.md](MIGRATION_MATRIX.md) are the source of truth for
individual components. The puzzles themselves are listed in
[MIGRATION_MAP.md](MIGRATION_MAP.md).

## Migration track

### Phase 0 — Infrastructure (checkpoint 001) — done

* [x] Host/virtualisation reconnaissance and documented build environment.
* [x] Pinned upstream baseline (`v6.18.57`) with reference branch.
* [x] Reproducible container image, build, initramfs, boot and linkage scripts.
* [x] Machine-readable migration registry and report generator.
* [x] First component: CRC-16 in Rust, Kconfig-selectable, differentially tested.

Exit criteria: both variants build; both boot; the Rust variant runs KUnit,
including the upstream CRC suite and the differential suite; linkage evidence
shows the C object is absent from the Rust build.

### Phase 1 — Library tier (checkpoint 002) — in progress

* [x] CRC-16 (`lib/crc/crc16.c`).
* [x] CRC-CCITT (`lib/crc/crc-ccitt.c`) — function **and** exported data table,
      differential suite, x86_64 and arm64 Ubuntu boots.
* [ ] CRC-ITU-T (`lib/crc/crc-itu-t.c`) — same family, small marginal cost.
* [ ] CRC-8 / CRC-7 (`lib/crc/crc8.c`, `lib/crc/crc7.c`).

Rule for this phase: each migration follows the D-003/D-004/D-005 pattern and
adds no new C dependency beyond the ksymtab shim.

### Phase 2 — Ownership and allocation: first substantial component

The investigation in `docs/ferret/design/COMPONENT_003_INVESTIGATION.md`
selected the next target explicitly rather than drifting through leaf code.

1. **`lib/kfifo.c` (checkpoint 003)** — 595 lines, 23 exports, upstream KUnit
   suite, allocation (`__kfifo_alloc`) and buffer-ownership semantics, no lock
   or per-CPU prerequisite. This is the first migration that is not a pure
   function over a byte buffer. **Done and verified**: `lib/kfifo_rust.rs`,
   with the differential suite `kfifo_diff`, the upstream `KFIFO_KUNIT_TEST`
   unchanged (in both variants), and a clean cycle that passed all 16 steps.
2. CRC-ITU-T / CRC-8 / CRC-7 as filler work while kfifo is reviewed.
3. Move the ksymtab shim out of existence by contributing per-symbol
   `EXPORT_SYMBOL` support for Rust translation units upstream, or by adopting
   it once it lands.

Exit criteria: kfifo replaced in the active build, upstream `KFIFO_KUNIT_TEST`
plus a differential suite pass, Ubuntu boots unaffected.

### Phase 2b — Allocation, reference counting and ownership (checkpoint 004)

The investigation is in
[`design/COMPONENT_004_INVESTIGATION.md`](design/COMPONENT_004_INVESTIGATION.md).

The kfifo port exposed the first structural gap: the `kernel` crate cannot
express a **run-time GFP mask**, so a Rust component cannot forward the
caller's allocation flags. That gap blocks `lib/bitmap.c` and `mm/mempool.c`,
and it is a small, reviewable change to the in-tree kernel crate.

**Selected target: `lib/bitmap.c`** — 882 lines, 31 exports, ~1 191 files use
it. It is a complete object boundary, it has an independent upstream KUnit
suite, it exercises allocation, and it reaches memory management, filesystems,
networking and drivers at once.

Deferred, with the reason recorded: the per-CPU family (`percpu_counter`,
`percpu-refcount`, `objagg`) is blocked on a `kernel::percpu` module;
`mm/mempool.c` needs the same GFP prerequisite plus a waitqueue abstraction;
`lib/idr.c` is a good candidate but competes with the bitmap for review
attention; the memory manager itself is explicitly out of scope.

### Phase 2c — Ownership primitives (checkpoint 005)

The investigation is in
[`design/COMPONENT_005_INVESTIGATION.md`](design/COMPONENT_005_INVESTIGATION.md).

**Selected target: `lib/refcount.c`** — the portable part of the kernel's
reference-counting machinery (186 lines, 6 exports), together with the two
kernel-crate abstractions it needs: a saturation-aware increment on
`kernel::sync::Refcount`, and `kernel::sync::KRef`, the handle for a
**C-owned** object that the VFS, networking and driver migrations are blocked
on.

Evaluated and deferred: `lib/idr.c` is a thin layer over the XArray, so it
waits for the XArray; `mm/mempool.c` needs a slab and a waitqueue;
`lib/llist.c` is feasible but its ordering risk cannot be verified by a test
and its consumers are outside the target areas.

**Implemented (checkpoint 005)**: `lib/refcount_rust.rs` replaces
`lib/refcount.c`; the `refcount_t` primitive stays in C (it is a header type
that the C code embeds in its own structures), and the exported layer moves to
Rust. The kernel crate gained the smallest sound extensions
(`Refcount::from_ptr()`, the saturation-aware operations, and the new
`kernel::sync::KRef` for C-owned objects). The audit of the component is
[`design/COMPONENT_005_AUDIT.md`](design/COMPONENT_005_AUDIT.md), and it is
the first component to go through the independent-verification gate
([`AUDIT_PROTOCOL.md`](../../AUDIT_PROTOCOL.md)).

### Phase 3 — Synchronization and per-CPU state

Ordered by prerequisite, not by appeal:

1. `kernel::percpu` Rust abstraction (allocation, per-CPU access, arch asm
   boundary) with its own tests.
2. `lib/percpu_counter.c` — allocation + irqsave spinlock + atomics, exported
   widely; first genuinely concurrent component.
3. `lib/ratelimit.c` — only after an IRQ-save trylock primitive exists in the
   Rust API (current blocker documented in the investigation).
4. `lib/sbitmap.c` — block-layer core; larger blast radius, needs waitqueue
   support.

Exit criteria: at least one component that allocates and synchronizes is
migrated, with lockdep-enabled boots and a concurrency stress test.

### Phase 4 — Task, scheduler, syscalls

* Task lifecycle bookkeeping and `task_struct` accessors.
* Runqueue operations behind existing locks; no RT-class changes initially.
* Syscall table plumbing for a small, self-contained syscall family
  (`getpid`/`gettid` family) as the first true userspace-facing migration.

### Phase 5 — VFS, block, networking

Follow the design documents in `docs/ferret/design/`. The invariant: each
migration removes more C interface surface than it adds.

1. `kernel::fs::FileOps` with `poll` + anon-inode support (prerequisite).
2. `fs/eventfd.c` — userspace-visible, validated by the eventfd kselftest
   inside the Ubuntu guest.
3. Pipe buffers, then inode/dentry reference helpers, then a first filesystem
   path.

### Phase 6 — Architecture and early boot

x86_64 first, arm64 maintained as a second lane (already built and booted in
checkpoint 002). Assembly boundary stays assembly; Rust owns the logic around
it.

## Usable-system track

The goal is an Ubuntu-compatible system running on the progressively
Rust-rewritten kernel, not a custom toy userspace.

* [x] **Checkpoint 001**: freestanding Rust init (raw syscalls) as the fast
      smoke test.
* [x] **Checkpoint 002**: real Ubuntu 24.04.5 root filesystem on virtio-blk +
      ext4, booted two ways:
  * `init=/sbin/ferret-init` — Ubuntu `/bin/sh` and `/bin/bash` execute a
    probe script as PID 1;
  * systemd as PID 1 — reaches multi-user, runs a unit, starts a serial
    autologin getty (`ferret login:`).
* [ ] **Next: networking** — virtio-net + DHCP/systemd-networkd, `apt-get`
      from the guest, DNS. Proves socket and netlink surface under load.
* [ ] **Next: storage** — a second virtio disk, partition table, mount by
      UUID, fstab-driven boot; later SCSI/NVMe paths.
* [ ] **Next: session and services** — login session, sshd, package
      installation, reboot with state preserved.
* [ ] **Later: graphical desktop** — DRM/virtio-gpu, a Wayland compositor, a
      display manager. This is the last milestone, not the next one.

Exit criteria for the usable-system track: an Ubuntu userland that can install
packages, start services and survive a reboot, on a kernel whose migrated
components are listed as migrated in the registry.

## Continuous workstreams

* **Upstream rebase**: follow the 6.18 longterm series; re-run the registry
  validation on each rebase.
* **ABI checks**: extend `tools/ferret/linkage.sh` into a symbol/ABI diff
  against the reference `vmlinux` (`Module.symvers`, `__ksymtab` comparison).
* **Userspace testing**: run LTP and kselftests inside the Ubuntu guest;
  keep the freestanding Rust init as the fast smoke test.
* **Dual architecture**: every checkpoint builds and boots x86_64 and arm64;
  a checkpoint that only builds one architecture is reported as partial.
* **Build efficiency**: container-local source mirror, image-only targets and
  ccache for clean rebuilds (DECISIONS D-014).
* **Performance**: QEMU-relative benchmarks per migrated component, reported in
  the registry report; never claim throughput from TCG without saying so.

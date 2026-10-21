# Migration matrix

Baseline: Linux `v6.18.57` (`2ff600d29ee478d0a8d46ef8a58afb5e038fe692`).
Status vocabulary: **migrated** (all porting-policy criteria met), **partial**,
**candidate**, **candidate-later**, **blocked**, **not started**.
The machine-readable source of truth is [`registry/migrations.json`](../../registry/migrations.json);
this document is the narrative view.

## 1. Library / CRC tier

| Component | C sources | Rust sources | Symbols | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| CRC-16 | `lib/crc/crc16.c` | `lib/crc/crc16_rust.rs` | `crc16` | migrated | [`evidence/`](evidence/) — boot markers and linkage |
| CRC-CCITT | `lib/crc/crc-ccitt.c` | `lib/crc/crc_ccitt_rust.rs` | `crc_ccitt`, `crc_ccitt_table` | migrated | [`evidence/`](evidence/) — boot markers and linkage |
| CRC-ITU-T | `lib/crc/crc-itu-t.c` | — | `crc_itu_t` | candidate | — |
| CRC-8 | `lib/crc/crc8.c` | — | `crc8`, `crc8_populate_*` | candidate | — |
| CRC-7 | `lib/crc/crc7.c` | — | `crc7_be` | candidate | — |
| CRC-32 / CRC-32c | `lib/crc/crc32-main.c` | — | `crc32_le`, `crc32_be`, `crc32c` | candidate-later | hot path + arch SIMD |
| CRC-64 | `lib/crc/crc64-main.c` | — | `crc64_be`, `crc64_nvme` | candidate-later | generated table + arch SIMD |

CRC-16 migration details:

* Call sites: 40 C files reference `crc16(` in-tree (drivers, net, fs).
* Interface: `include/linux/crc16.h` (unchanged).
* Export: `EXPORT_SYMBOL` (non-GPL) preserved via `lib/crc/crc16_export.c`
  (ksymtab only, no algorithm — DECISIONS D-004).
* Tests: upstream `CONFIG_CRC_KUNIT_TEST` (independent bit-at-a-time reference,
  randomised buffers, guard page, IRQ context), `crc16_rust` Rust vectors,
  `crc16_diff` differential against the original C compiled as `crc16_c_ref`.
* Measured footprint (x86_64 defconfig): 133 B (C) → 147 B (Rust) of text,
  +14 B (+10.5%); runtime throughput NOT TESTED.
* Linkage evidence: [`evidence/linkage-x86_64.txt`](evidence/linkage-x86_64.txt)
  (single definition, size match,
  ksymtab present, `EXPORT_SYMBOL` preserved).

CRC-CCITT migration details:

* Call sites: 34 C/header files call `crc_ccitt()`; 4 reference the exported
  `crc_ccitt_table[]` directly. `crc_ccitt_byte()` stays a static inline in
  `include/linux/crc-ccitt.h` and indexes the table.
* Interface: `include/linux/crc-ccitt.h` (unchanged).
* Export: `crc_ccitt` **and** `crc_ccitt_table` are `EXPORT_SYMBOL`; both
  ksymtab entries are emitted by `lib/crc/crc_ccitt_export.c` (no algorithm).
* Table: generated at compile time by a `const fn` (reflected polynomial
  0x8408); the differential suite compares all 256 entries against the
  original C literal table.
* Tests: `crc_ccitt_rust` vectors (including every one of the 65536 initial
  values over one byte), `crc_ccitt_diff` differential (table, edge cases,
  2000 randomised streams), boot on x86_64 and arm64, Ubuntu shell and systemd
  boots, linkage ownership.
* Kconfig note: `CRC_CCITT` has no prompt, so `CRC_CCITT_RUST` **selects** it
  to force `= y`; a built-in Rust object cannot replace a modular C one
  (DECISIONS D-015).
* Measured footprint (x86_64 defconfig): `crc_ccitt()` 124 B (C) -> 138 B
  (Rust), +14 B (+11.3%). arm64 Rust object: 72 B (no arm64 C reference was
  built, so no delta is claimed there). Runtime throughput NOT TESTED
  (QEMU TCG only).

### Remaining C dependencies for CRC-16

| File | Kind | Exit condition |
| --- | --- | --- |
| `lib/crc/crc16_export.c` | linkage/ksymtab only | per-symbol non-GPL `EXPORT_SYMBOL` support for Rust translation units |

### Remaining C dependencies for CRC-CCITT

| File | Kind | Exit condition |
| --- | --- | --- |
| `lib/crc/crc_ccitt_export.c` | linkage/ksymtab only (two symbols, one of them data) | per-symbol non-GPL `EXPORT_SYMBOL` support for Rust translation units |

## 2. Investigated substantial components

Full analysis: `docs/ferret/design/COMPONENT_003_INVESTIGATION.md`.

| Component | Mechanism | Status | Prerequisite / decision |
| --- | --- | --- | --- |
| `lib/kfifo.c` | allocation + buffer ownership, 23 exports, upstream KUnit | **migrated (checkpoint 003)** | none; implemented in `lib/kfifo_rust.rs` |
| `lib/percpu_counter.c` | per-CPU allocation + irqsave spinlock + atomics | blocked | `kernel::percpu` abstraction does not exist yet |
| `lib/ratelimit.c` | irqsave trylock + atomics + jiffies | blocked | no `_raw_spin_trylock_irqsave` binding or symbol |
| `lib/sbitmap.c` | kvmalloc + irqsave spinlock + waitqueue | deferred | waitqueue API and larger blast radius |
| `fs/eventfd.c` | kref lifetime + waitqueue + anon-inode fops + poll | blocked | Rust `FileOps`/anon-inode layer with `poll` |

### The kernel bitmap (checkpoint 003)

* C source: `lib/bitmap.c` (882 lines, 31 exported symbols, 1 191 files use
  the API).
* Rust source: `lib/bitmap_rust.rs`; the C object is **not built** in the Rust
  variant, and all 29 exported symbols (27 `EXPORT_SYMBOL`, two
  `EXPORT_SYMBOL_GPL`) are defined exactly once by the Rust object, with their
  export class preserved by the ksymtab-only shim `lib/bitmap_export.c`.
* The three global symbols that the C file does *not* export
  (`__bitmap_or_equal()`, and, with `CONFIG_NUMA`, `bitmap_onto()` and
  `bitmap_fold()`) are provided as well, because the header's inline helpers
  call them.
* The 32-bit variants (`bitmap_from_arr64()`, `bitmap_to_arr64()`) are
  implemented as well; they are only compiled on a 32-bit target, which Ferret
  does not build or test yet.
* The bit numbering (bit `n` is bit `n % BITS_PER_LONG` of word
  `n / BITS_PER_LONG`), the word size, the treatment of the tail bits above
  the valid range and the overlapping-buffer behaviour of `bitmap_cut()` are
  the same as in the C original.
* Tests: the upstream `test_bitmap` kselftest module (**390 447 cases**, all
  passing) and a differential suite (`bitmap_diff`, 8 cases) that compiles the
  unmodified `lib/bitmap.c` a second time under test-only symbol names and
  compares the two implementations over the whole API: the logical operations,
  the weight helpers, the set/clear ranges, the shifts, the cut, the zero-area
  search, the remapping, the 32-bit word conversions and the allocation.
* Two documented deviations, both where the C code is undefined: a shift
  larger than the bitmap makes the C code clear memory outside the bitmap
  (the Rust code clamps it), and `bitmap_cut()` with a `first` past the end of
  the bitmap makes the C code read and write a word outside the bitmap (the
  Rust code only touches the words inside it; the bits of the bitmap itself are
  the same).
* Remaining C dependency: `lib/bitmap_export.c` (ksymtab only).
* Text size: 5 807 bytes against 3 712 bytes in C (+56 %, 2 095 bytes in a
  57 MB kernel). The difference is in the per-bit helpers, which have no
  unchecked fast path in the C original; the absolute cost is 0.004 % of the
  kernel.
* Throughput (in-kernel benchmark, Rust against the C reference in the same
  run): 1 327 per mille with the weight helpers, 884 per mille without them.
  The gap is entirely in the population count: the kernel's alternatives
  mechanism patches the C `hweight64()` to the `popcnt` instruction at boot,
  and that patch cannot be applied to Rust code.
* Audit: [`design/COMPONENT_004_AUDIT.md`](design/COMPONENT_004_AUDIT.md) — the
  `Words` invariant, every unchecked access, the three places where the C code
  is undefined, and the 32-bit status (**NOT TESTED**).

### The generic kernel FIFO (checkpoint 003)

* C sources: `lib/kfifo.c` (595 lines, 23 `EXPORT_SYMBOL` exports).
* Rust source: `lib/kfifo_rust.rs`; the C object is **not built** in the Rust
  variant (`lib/kfifo.o` is absent, `lib/kfifo_rust.o` defines all 23
  symbols).
* Interface: `include/linux/kfifo.h` is unchanged. `struct __kfifo` keeps its
  layout, and the Rust code operates on the caller's object in place through
  the bindgen-generated type.
* Export: all 23 symbols keep their non-GPL `EXPORT_SYMBOL` visibility through
  the ksymtab-only shim `lib/kfifo_export.c` (no algorithm).
* Concurrency: the FIFO is lock-free by contract, exactly as in C; the Rust
  code adds no locks and no atomics, and the memory ordering of the C
  `smp_wmb()` is kept with a release fence.
* Tests: the upstream `KFIFO_KUNIT_TEST` suite (in both variants, so the
  reference kernel has to satisfy it too), thirteen Rust unit tests
  (`kfifo_rust`), and a differential suite (`kfifo_diff`) that compiles the
  unmodified `lib/kfifo.c` a second time under test-only symbol names and
  compares the two implementations over initialisation, allocation, 2 000
  randomised operation sequences, wrap-around, the record ("_r") API, the
  length clamp, the scatterlist setup (including the split at the wrap
  point) and the user-copy fault path.
* Documented deviations, both in cases where the C code is undefined:
  `__kfifo_init()` with `esize == 0` returns `-EINVAL` instead of dividing by
  zero, and `__kfifo_max_r()` with `recsize >= 4` returns `u32::MAX` instead of
  shifting a 32-bit integer by 32 or more.
* Remaining C dependency: `lib/kfifo_export.c` (ksymtab only).
* Size and throughput: the Rust text is 3 162 bytes against 2 347 bytes in C
  (+34.7 %, 815 bytes in a 57 MB kernel). The cause is the bounds checking
  and the wider arithmetic of the slice-based core, not a different
  algorithm. A KUnit micro-benchmark measures the two implementations in the
  same run; the absolute numbers are QEMU TCG figures and only the ratio is
  meaningful. Details in
  [`design/COMPONENT_003_INVESTIGATION.md`](design/COMPONENT_003_INVESTIGATION.md)
  §4.1.2.

## 3. Platform matrix

| Platform | Build | Boot (custom Rust init) | Ubuntu shell | Ubuntu systemd | Notes |
| --- | --- | --- | --- | --- | --- |
| x86_64 | PASS | PASS | PASS | PASS | primary lane, QEMU TCG |
| arm64 | PASS | not used | PASS | PASS | QEMU `virt`, TCG (no KVM on the host) |

## 4. Other candidate tiers

| Tier | Examples | Status | Prerequisite |
| --- | --- | --- | --- |
| Parsing / formatting | `lib/kstrtox.c`, `lib/hexdump.c`, `lib/parser.c` | not started | — |
| Data structures | `lib/rbtree.c`, `lib/list_sort.c`, `lib/hashtable.c` | not started | collection wrappers with kernel semantics |
| String helpers | `lib/string.c` (`strscpy`, `strreplace`) | not started | arch-sensitive parts excluded |
| MM | buddy, SLUB, VMA, page tables | not started | allocator-safe Rust (no alloc/panic), lock wrappers |
| Scheduler | task lifecycle, CFS, runqueues | not started | `task_struct` accessor model, preemption guards |
| VFS | pipe, eventfd, inode/dentry refs | not started | `file_operations` bridge, refcount types |
| Networking | skb helpers, UDP, TCP | not started | skb handle model |
| Security | cred refcounts, capability arithmetic | not started | immutable cred handle |
| Architecture | x86_64 dispatch, CPU features; arm64 later | not started | asm boundary contract |

## 5. Rules for updating this matrix

1. A row moves to `migrated` only when the registry validation passes
   (`tools/ferret/registry.py` exits 0) and the evidence files exist.
2. Every new migration adds: C sources, Rust sources, Kconfig switch, ABI
   symbols with export type, dependency list, tests with evidence paths,
   compatibility risks and remaining unsafe boundaries.
3. Remaining C dependencies are listed per component; the count must trend
   downwards. A migration that increases it must say why in DECISIONS.md.

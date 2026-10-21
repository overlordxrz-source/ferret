# Design: memory management in Rust

Status: design target, not implemented. This document records the intended
migration boundary and the invariants Rust is expected to encode.

## Scope

* `mm/page_alloc.c` — buddy allocator, per-CPU page lists, free-area management.
* `mm/slub.c` — slab allocator (SLUB) core.
* `mm/mmap.c`, `mm/vma.c` — virtual memory areas.
* `mm/memory.c`, `mm/pgtable-generic.c` — page-table walking and fault handling.
* `mm/vmscan.c`, `mm/filemap.c` — reclaim and page cache.

## Why it is not first

Every subsystem depends on these. A mistake here is not a bug in a component,
it is a dead machine. Migration therefore proceeds bottom-up from leaf helpers
with explicit preconditions, keeping the C allocator in place while Rust owns
increasingly large fractions of the surrounding bookkeeping.

## Invariants to encode

| Invariant | C encoding today | Rust encoding |
| --- | --- | --- |
| A page is owned by exactly one allocator state | implicit, `PageBuddy`/refcount bits | `Page` newtype + typestate (`Free`, `Allocated`, `Slab`) |
| Zone lock protects free lists | `spin_lock_irqsave(&zone->lock)` | `Zone` struct owning `SpinLock<FreeLists>`, access only through guard |
| PCP lists are per-CPU and IRQ-unsafe without disable | implicit + `local_lock` | `LocalLock` guard required by API signature |
| Page table entries alias physical memory | raw `pte_t` | `Pte` with accessor functions; no `Deref` to pointers |
| VMA tree ordered by address | maple tree | ordered collection wrapper enforcing comparator |

## Proposed Rust layout

```
mm/rust/
  page.rs        struct Page, flags, refcount (bit ops only)
  buddy.rs       free-area arithmetic, no locking
  zone.rs        Zone, SpinLock<FreeLists>, watermark checks
  pcp.rs         per-CPU lists; requires local_lock guard
  slab.rs        SLUB freelist encoding (pointer obfuscation kept identical)
  vma.rs         VMA metadata and ordering
```

Rules:

* Physical addresses are not pointers. Address types (`PhysAddr`, `VirtAddr`)
  are distinct newtypes; conversion is explicit and unsafe.
* No Rust type owns a page frame until the C allocator is removed; during the
  transition Rust operates on `*mut page` handles with documented provenance.
* Locking is expressed by the C `spinlock_t` bridged through
  `kernel::sync::SpinLock` only where the lock is not also taken by C in the
  same critical section. Shared-lock ownership remains C until the whole
  critical section moves.

## Migration order and exit criteria

1. **Buddy arithmetic helpers** (no locking): `__find_buddy_pfn`, `buddy_order`,
   expansion/splitting free lists. Tests: property tests over page-frame ranges,
   compared with the C helpers through a differential KUnit suite.
2. **Per-CPU page lists**: requires a Rust `local_lock` wrapper. Tests: boot
   with `CONFIG_DEBUG_PREEMPT`, allocation stress under KUnit.
3. **Zone allocation slow path**: must keep `__GFP_*` semantics bit-for-bit;
   a `GfpFlags` bitflags wrapper generated from the same constants.
4. **SLUB**: last, because freelist encoding and object lifetime are the
   hardest invariants in the kernel.

## Risks

* **Allocator recursion**: Rust code in the allocator may itself allocate
  (formatting, panics). No allocations, no formatting, no panics in this tier.
* **Lock ordering**: any new lock wrapper must preserve the existing order or
  lockdep will flag it; every migration boot must run with lockdep on.
* **Per-CPU assumptions**: preemption/IRQ state must be part of the type
  system, not a comment.

## Verification

* `CONFIG_DEBUG_VM`, `CONFIG_DEBUG_PAGEALLOC`, KASAN, lockdep boots.
* `tools/testing/selftests/mm` and LTP `mm` subset on the Ferret kernel.
* Differential KUnit for the pure arithmetic helpers before any locking code
  moves.

# Design: architecture tier

Status: design target.

## Purpose

Define where the Rust/C/assembly boundary sits for each architecture, and what
"architecture-specific kernel logic in Rust" means in practice.

## x86_64

| Area | Owner after migration | Notes |
| --- | --- | --- |
| `head_64.S` boot stub | assembly | compression/decompression, page-table bootstrap |
| `arch/x86/kernel/setup.c` parameter handling | Rust | consumes `boot_params`, validates, fills `boot_params` extensions |
| CPU identification (`cpu/common.c`) | Rust + CPUID asm wrapper | feature bits are bitfields; Rust gains named accessors |
| IDT/GDT setup | C or Rust? | descriptor table construction is data, not asm; candidate for Rust |
| Exception entry/exit (`entry_64.S`) | assembly | register save/restore and stack switching stay in asm |
| Exception *dispatch* (`traps.c` handlers) | Rust | returns `enum ExceptionAction { Handled, Fatal(...) }` |
| Interrupt controllers (APIC, IOAPIC) | Rust after MMIO abstraction | register-level wrappers with volatile access |
| `percpu` accessors | Rust inline asm + typed percpu variables | `this_cpu_read` equivalents with preemption rules documented |
| Timekeeping (TSC, clocksource) | Rust arithmetic; asm for TSC read | |
| Page-table manipulation | Rust after MM tier | leaves remain in `pgtable_64.c` initially |

## arm64

| Area | Owner after migration | Notes |
| --- | --- | --- |
| `head.S` | assembly | |
| `kernel/setup.c`, `cpuinfo.c` | Rust | feature register decoding is pure logic |
| `entry.S`, `entry-common.c` | assembly + Rust dispatch | same split as x86_64 |
| `arch/arm64/mm/*` | Rust after x86_64 proves the abstraction | |
| `percpu` (TPIDR_EL1) | Rust inline asm | |

## Cross-architecture rules

1. **One abstraction, two backends.** New Rust APIs are architecture-neutral;
   the arch-specific part is a small trait or a `cfg(target_arch)` module.
2. **No asm outside `.S` plus documented inline-asm helpers.** Inline asm is
   allowed only for single instructions or well-specified sequences (CPUID,
   MSR reads, TSC, barriers), wrapped in a function with a `// SAFETY:` contract.
3. **Boot order is preserved exactly.** Any Rust initialisation added to the
   boot path must be callable before the allocator, without allocation, and
   without panicking; `#[cfg(CONFIG_FERRET_EARLY)]` code is reviewed as if it
   were assembly.
4. **`no_std`, no unwinding.** The kernel cannot unwind; Rust builds use
   `panic=abort` semantics. Any `Result`-based error handling is compile-time
   and explicit.

## Verification

* Both architectures build on every change (`ARCH=x86_64` and `ARCH=arm64`
  defconfig + RUST).
* Boot QEMU for x86_64 on every change; arm64 boot once the harness gains a
  bootable arm64 guest path.
* `objdump` review of entry/exit functions to ensure Rust did not change the
  instruction sequences it is not supposed to own.

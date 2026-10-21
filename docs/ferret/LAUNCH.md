# Launch notes

Short, accurate copy for the first public announcement. Adjust links only if the
repository moves. Do not add claims that are not in `STATUS.md`.

## X / Twitter (single post)

> Linux kernel, but the C components are being replaced by real Rust
> implementations — not wrappers.
>
> Ferret is a fork of Linux v6.18.57. CRC-16 and CRC-CCITT now compile as Rust,
> the C objects are gone from the build, and differential tests + QEMU boots
> prove it. It boots Ubuntu 24.04 to systemd on x86_64 and arm64.
>
> Experimental, early, and built to be contributed to. Every migration is a
> puzzle with a test: pick one, claim it, prove it.
>
> https://github.com/overlordxrz-source/ferret

## Alternative pinned thread

1/ Ferret is an incremental rewrite of the **real** Linux kernel from C into
Rust. Same source tree, same Kconfig/Kbuild, same userspace ABI. Not a
Linux-inspired OS, not a driver experiment.

2/ The rule is simple: a migrated component is the implementation the kernel
links. The C object is not built. The Rust object defines the exported symbol.
Differential KUnit compares it against the original C. A QEMU boot exercises it.

3/ Checkpoint 1: CRC-16. Checkpoint 2: CRC-CCITT. Both linked into x86_64
kernels, booted, tested. A reference kernel is built from the same tree for
comparison.

4/ It also boots a real Ubuntu 24.04 root filesystem — shell and systemd — on
x86_64 and arm64, in QEMU. That is compatibility evidence, not a desktop.

5/ The interesting part is the queue. Each migration issue names the C file, the
symbols, the safety invariants, the tests and the exit criteria. Starters are
genuinely starter-sized; the expert ones say so.

6/ It is early. Two utilities is not "Linux rewritten". The point is the method:
a reproducible path from one C file to one Rust implementation, repeated until
there is no C left.

https://github.com/overlordxrz-source/ferret

## Hacker News

**Show HN: Ferret – rewriting the Linux kernel in Rust, one component at a time**

Ferret is a fork of Linux v6.18.57 that replaces C kernel components with
native Rust implementations. The first two are CRC-16 and CRC-CCITT; the C
objects are removed from the active build and the Rust objects define the
exported symbols, including the original non-GPL export via a ksymtab-only shim.

What makes it more than a wrapper:

* a differential KUnit suite compiles the original C file under renamed symbols
  and compares behaviour over edge cases and 2000 random streams;
* linkage checks prove object ownership, single definition, function size and
  export class;
* the kernel boots in QEMU and a real Ubuntu 24.04 root filesystem reaches a
  shell and systemd as PID 1, on x86_64 and arm64.

It is explicitly early: two library components, no driver or MM work, all
performance numbers are TCG-only. The repository is structured so each migration
is an independently reviewable task with defined acceptance criteria.

I would especially value feedback on the unsafe boundary, the differential test
design, and whether the porting policy is strict enough.

## Reddit r/rust

**Ferret: incremental C→Rust migration of the actual Linux kernel (checkpoint 2:
CRC-CCITT)**

Ferret keeps the real Linux tree and swaps implementations at the object level:
Kconfig selects a Rust variant, the Makefile stops building the C object, and a
ksymtab-only C file preserves the exact export visibility that Rust's `#[export]`
cannot express today.

Verification is the main work: differential KUnit against the unmodified C,
Rust unit tests, upstream KUnit, symbol/linkage proofs, and QEMU boots with a
Rust init plus Ubuntu userspace (shell + systemd, amd64 + arm64).

The roadmap and the migration matrix are public, and every issue is a puzzle
with acceptance criteria. Starter tasks are small on purpose.

https://github.com/overlordxrz-source/ferret

## What not to say

* Do not claim a percentage of Linux rewritten.
* Do not claim hardware support, a desktop, containers, or musl.
* Do not call the CRC replacements a performance improvement.
* Do not imply endorsement by the Linux Foundation or Rust for Linux.

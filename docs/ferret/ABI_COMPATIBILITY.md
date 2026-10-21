# ABI compatibility

Ferret's reason to exist is that the kernel is Linux: if userspace can tell
which language a component is written in, the migration failed.

## 1. What must not change

| Surface | Rule |
| --- | --- |
| Syscall numbers and signatures | Unchanged, per architecture |
| Syscall semantics and `errno` | Identical, including edge cases and `-ERANGE`/`-EINVAL` choices |
| UAPI headers (`include/uapi/`) | Byte-identical structures, constants, ioctl numbers |
| `/proc`, `/sys`, `sysfs` attributes | Same names, formats, permissions |
| Exported kernel symbols | Same name, signature and visibility (`EXPORT_SYMBOL` vs `EXPORT_SYMBOL_GPL`) |
| Module ABI | `Module.symvers` entries keep symbol type and export class |
| Kernel command line, boot protocol | Unchanged |
| Locking and context rules | A function callable in atomic context stays callable in atomic context |
| Allocation semantics | Same fallibility and same GFP expectations |

A migration that changes any of these is rejected, even if the new behaviour is
"nicer". This is why the C reference stays in the tree until the Rust version
has been differentially tested.

## 2. Symbol exports

The kernel's module ABI depends on the exact export class. Rust's `#[export]`
macro produces GPL-only exports, which would silently break any non-GPL module
using the symbol.

For a component exported with `EXPORT_SYMBOL`, Ferret adds a **ksymtab-only**
shim (`<component>_export.c`) that contains:

* `EXPORT_SYMBOL(<symbol>)`,
* `EXPORT_SYMBOL(<symbol>_table)` where the original exported a table,
* no algorithm, no state, no fallback.

`linkage.sh` verifies that the shim only references the symbol and that
`Module.symvers` still records `EXPORT_SYMBOL`. See `DECISIONS.md` D-004.

## 3. Data layout and calling convention

* `extern "C"` signatures must match the C declaration exactly, including
  `unsigned` widths (`u8`, `u16`, `u32`, `size_t`) and pointer constness.
* Rust types at the boundary are `#[repr(C)]` or primitives; no Rust enums or
  slices cross the FFI.
* Structure layout is checked by `bindgen`-generated bindings from the real
  headers, not by hand-written mirrors.
* Tables exported to C (`crc_ccitt_table`) keep their element type, size and
  contents; the differential test compares every entry.
* Calling into C from Rust and Rust from C must respect the kernel's context
  rules (IRQ, softirq, atomic); a Rust `&mut` alias across a callback is a bug.

## 4. Userspace compatibility goals

| Goal | Status |
| --- | --- |
| glibc userspace boots to a shell | verified (Ubuntu 24.04, amd64 + arm64) |
| `systemd` as PID 1, multi-user | verified (Ubuntu 24.04, amd64 + arm64) |
| musl userspace | not tested |
| Container runtimes (namespaces, cgroups, overlayfs) | not tested |
| 32-bit compat (`CONFIG_COMPAT`) | not tested |
| Desktop (Wayland/X11, DRM, input) | not tested, no driver work yet |
| Networking end-to-end (DHCP, DNS, TCP) | not asserted by the harness |

The Ubuntu guests are real distro root filesystems, but the assertions are
deliberately narrow: the shell probe and a systemd unit. "Ubuntu boots" does not
mean "Ubuntu is fully functional".

## 5. How ABI preservation is verified

1. **Compile-time:** bindgen-generated signatures; a mismatch fails the build.
2. **Link-time:** one definition per symbol, correct object ownership, size
   match, ksymtab entry, export class (`linkage.sh`).
3. **Runtime:** upstream KUnit plus differential KUnit.
4. **Boot:** KUnit output and userspace markers in QEMU.
5. **Integration:** the Ubuntu shell/systemd probes.

There is no ABI diff tool that compares two kernels symbol-by-symbol yet; that
is tracked as a testing-infrastructure task. Current coverage is limited to the
migrated component and the boot markers.

## 6. Rust for Linux relationship

Ferret uses the in-tree Rust for Linux infrastructure (`kernel` crate, build
integration, `#[export]`, bindings). Those abstractions themselves promise
kernel-ABI compatibility; Ferret's job is to not weaken it at the component
boundary. Where the abstraction does not exist yet (for example non-GPL
per-symbol exports), Ferret adds the smallest documented glue rather than a
parallel mechanism.

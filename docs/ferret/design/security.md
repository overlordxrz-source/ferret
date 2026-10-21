# Design: security, isolation and architecture boundaries

Status: design target.

## Scope

* `kernel/cred.c` — credentials and reference counting.
* `security/*` — LSM hooks and the capability checks in `security/commoncap.c`.
* `kernel/user_namespace.c`, `kernel/cgroup/*` — namespaces and cgroups.
* `kernel/seccomp.c` — syscall filtering.
* `arch/x86/*` — boot, exceptions, interrupts, percpu, low-level CPU helpers.
* `arch/arm64/*` — same, as the secondary architecture.

## Security model in Rust

The security value of a Rust migration is not "memory safety" in the abstract;
it is that *invariants which are currently comments become types*:

| Invariant | Today | Target |
| --- | --- | --- |
| `cred` is immutable once published | convention + RCU | `Arc<KernelCred>`-style shared immutable handle |
| Capability checks use the caller's cred, not `current` implicitly | convention | check function takes `&Cred` explicitly |
| `seccomp` filters are valid BPF and immutable | validation at install | validated filter type; only constructible via `try_from` |
| Namespace membership changes only with the owning lock | convention | accessor requires `NamespaceLock` guard |
| LSM hook ordering is fixed | link order | explicit chain type with compile-time order |

## Order of work

1. **Credential refcounting helpers** (`get_cred`/`put_cred`, `prepare_creds`,
   `commit_creds`) — small, well-tested by `tools/testing/selftests/` and
   exercisable from KUnit.
2. **Capability arithmetic** (`cap_capable`, `ns_capable`, bitmap ops in
   `kernel/capability.c`) — pure functions, differential-testable.
3. **seccomp filter validation** — a self-contained parser that rejects exactly
   what the C implementation rejects; fuzz it against the C version.
4. **LSM hook dispatch** — deferred until the LSM API itself is stabilised
   upstream; changing hook order or `-errno` semantics is a security bug.
5. **Namespaces/cgroups** — last; they touch the scheduler, MM and VFS.

## Architecture boundary

Ferret's rule for `arch/`: **assembly stays assembly**. Rust owns:

* boot parameter parsing and early setup decisions,
* exception/interrupt dispatch *tables and bookkeeping*,
* CPU feature detection and validation logic,
* percpu accessors (with the underlying GS/TPIDR access in inline asm).

Rust does not own:

* the entry/exit asm stubs (context save/restore, `swapgs`, `iretq`),
* page-table switching asm,
* anything whose correctness depends on a specific instruction sequence the
  compiler may not reorder.

The FFI contract for entry code is documented per function ("called with
interrupts disabled, on the kernel stack, returning through `iretq`"), and the
Rust side keeps those functions `unsafe extern "C"` with a single call site.

## x86_64 first, arm64 second

* x86_64 is the first target because the QEMU test harness is fastest there and
  the Rust-for-Linux support is most exercised.
* Every new abstraction must be architecture-neutral or explicitly gated; the
  arm64 build is kept green in CI by building `defconfig` for both
  architectures even when boot-testing only x86_64.
* Arch-specific Rust code goes in `arch/<arch>/rust/` and is compiled only for
  that architecture.

## Verification

* `tools/testing/selftests/seccomp`, `capabilities`, `namespaces`.
* LTP subset for credentials and capabilities.
* `CONFIG_DEBUG_CREDENTIALS`, `CONFIG_SECURITY_SELINUX_DEVELOP` boots.
* Boot tests for both x86_64 and (once virtualised guests support it) arm64.

# Security policy

Ferret is an **experimental** kernel fork. It is not a supported production
kernel, and it does not have a security response team. That said, memory-safety
bugs in the migrated Rust components, unsafe-code soundness issues, and
privilege or sandbox escapes are taken seriously.

## Reporting a vulnerability

**Do not open a public issue for a security vulnerability.**

Report privately through GitHub's [private vulnerability
reporting](https://github.com/overlordxrz-source/ferret/security/advisories/new)
(Security → Advisories → "Report a vulnerability"). If that form is unavailable,
contact the maintainer ([@overlordxrz-source](https://github.com/overlordxrz-source))
privately and ask for a channel.

Please include:

* the commit SHA and the kernel configuration (`artifacts/<variant>/config`),
* the affected component and the C/Rust files involved,
* a minimal reproduction (commands, QEMU invocation, or a KUnit reproducer),
* the impact you believe it has,
* whether the bug exists in unmodified upstream Linux as well.

## What to expect

* Acknowledgement within 7 days.
* An assessment of whether the issue is Ferret-specific or inherited from
  upstream. Inherited bugs should also be reported to the upstream Linux
  security process (`security@kernel.org`); Ferret does not speak for upstream.
* A coordinated fix and disclosure timeline agreed with you. Default: 90 days
  or earlier if a fix is available and users are affected.
* Credit in the advisory unless you ask otherwise.

## Scope

In scope:

* Rust implementations in `lib/**/*_rust.rs` and their `extern "C"` boundary.
* The ksymtab shims (`*_export.c`) if they change symbol visibility.
* The build/test tooling in `tools/ferret/` only where it can produce a kernel
  that misrepresents what it is.
* Unsafe blocks whose documented invariants are violated by safe Rust callers.

Out of scope:

* Bugs that reproduce identically on unmodified Linux `v6.18.57` and are not
  made worse by Ferret (report those upstream).
* The experimental Ubuntu/QEMU test guests; they are disposable test fixtures,
  not a hardened product.
* Denial of service caused by intentionally building a kernel with debug or
  KUnit options that are documented as test-only.

## Hardening expectations for contributions

* No new `unsafe` without a `// SAFETY:` justification that a reviewer can
  check against the caller contract.
* No panics on reachable paths (panic in kernel Rust aborts the kernel).
* No silent fallback to a C implementation behind a Rust wrapper.
* Symbol visibility must be preserved; changing `EXPORT_SYMBOL` to GPL-only is
  an ABI and security-relevant change.

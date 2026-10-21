# Contributing to Ferret

Ferret is an experimental fork of the Linux kernel that replaces C components
with native Rust implementations. Contributions are welcome — but a migration
is only done when the C implementation is gone from the active build and the
replacement is proven.

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).

## Ways to contribute

| Kind | Where to start |
| --- | --- |
| Migrate a component to Rust | Issues labelled `type: migration` |
| Improve testing / verification tooling | Issues labelled `area: testing` |
| Improve documentation | Issues labelled `area: documentation` |
| Report a bug | [Bug template](.github/ISSUE_TEMPLATE/bug.yml) |
| Report a security problem | [SECURITY.md](SECURITY.md) — never a public issue |

If you are new to kernel work, start with `difficulty: starter` and
`good first issue`. Those labels are kept honest: a starter puzzle is a bounded
patch with a clear test, not "rewrite the scheduler".

## Claiming a puzzle

Ferret is not an opaque queue. To claim an issue:

1. Read the issue. It names the C file, the Rust destination, the symbols, the
   tests and the acceptance criteria.
2. Comment on the issue with your plan and the date you expect to open a PR.
3. A maintainer replies, adds `status: claimed` and assigns the issue when
   permissions allow (external contributors cannot self-assign).
4. If you stop working on it, say so — a maintainer will return it to
   `status: ready` rather than leaving it stalled.

Open a draft PR early. Duplicate work is avoided by the `status: claimed` label
and by the "Claim" section of the issue; check the issue list before starting.

## Development setup

See [docs/ferret/BUILDING.md](docs/ferret/BUILDING.md). The short version:

```sh
git clone https://github.com/overlordxrz-source/ferret.git
cd ferret

# Optional but recommended: the pinned container (same toolchain as CI)
docker build -t ferret-buildenv:1 tools/ferret/docker

tools/ferret/build.sh rust     # x86_64 Ferret kernel
tools/ferret/build.sh ref      # unmodified upstream reference kernel
tools/ferret/boot.sh rust      # QEMU boot + KUnit + userspace markers
tools/ferret/linkage.sh rust   # symbol ownership / export evidence
```

A focused component usually only needs an incremental build and a single boot
test; you do not need a full clean cross-architecture cycle to send a PR.

## The migration recipe

Every component follows the same pattern. `lib/crc/crc16_rust.rs` and
`lib/crc/crc_ccitt_rust.rs` are the worked examples.

1. **Keep the C original.** Do not delete it. It becomes the reference used by
   the differential test.
2. **Add the Rust implementation** next to it (`<component>_rust.rs`) with the
   exact `extern "C"` signatures from the existing header.
3. **Add a Kconfig switch** next to the upstream symbol:
   `config <COMP>_RUST bool ... depends on RUST` (and `select <COMP>` when the
   upstream symbol has no prompt).
4. **Switch the object in the Makefile** with an `ifeq`, so the C object is not
   built at all when the Rust variant is selected.
5. **Export symbols correctly.** Rust `#[export]` exports are GPL-only; when
   the C symbol was `EXPORT_SYMBOL` (non-GPL), add a ksymtab-only shim
   (`<component>_export.c`) that contains no algorithm. See
   [DECISIONS D-004](docs/ferret/DECISIONS.md).
6. **Add a differential KUnit suite** that compiles the original C file under
   renamed symbols and compares it with the Rust code over edge cases and
   randomised inputs.
7. **Add Rust unit tests** for vectors and invariants.
8. **Prove the link:** `tools/ferret/linkage.sh rust` must show the C object
   absent, one definition per symbol, matching sizes, and the original export
   type.
9. **Boot it:** `tools/ferret/boot.sh rust` must pass, including the upstream
   KUnit suites and the userspace ABI markers.
10. **Update the registry** (`registry/migrations.json`) and regenerate the
    report (`python3 tools/ferret/registry.py --markdown`). A registry entry
    claiming `migrated` without evidence is a build failure.

Read [docs/ferret/PORTING_POLICY.md](docs/ferret/PORTING_POLICY.md) before you
start. It defines what counts as a migration, what is rejected, and how
unsafe code is reviewed.

## Rust conventions

* `#![no_std]` kernel code; use the in-tree `kernel` crate.
* Safe Rust by default. Every `unsafe` block needs a `// SAFETY:` comment that
  states the invariant and who upholds it.
* No panics in reachable kernel paths: avoid `unwrap`, indexing that can go out
  of bounds on malformed input, and arithmetic that can overflow. Use `Result`
  and explicit error paths.
* Allocation is fallible. Never allocate with an implicit `GFP_KERNEL`-style
  assumption in a context where the C original did not.
* Keep the Rust/C boundary as small as possible; convert raw pointers to slices
  or typed references immediately and keep ownership obvious.
* Follow `.rustfmt.toml` and the style of the surrounding Rust for Linux code.

## Commits and PRs

* Small, self-contained commits. A migration commit should be reviewable on its
  own.
* Sign off your commits (`git commit -s`) — Ferret uses the Developer
  Certificate of Origin, like the kernel.
* Reference the issue (`Fixes: #12` or `Part of #12`).
* The [PR template](.github/PULL_REQUEST_TEMPLATE.md) asks for: what C code was
  replaced, whether the C object is still compiled, tests run, build/boot
  status, performance evidence (if relevant), safety invariants, and registry
  updates.
* A PR is merged only when the required checks pass and a maintainer has
  reviewed the safety and ABI claims. "The tests pass on my machine" is not
  evidence; paste the commands and the relevant output.

## Licensing

Ferret is GPL-2.0-only, like Linux. Every new file starts with
`// SPDX-License-Identifier: GPL-2.0-only` (or the appropriate existing
license). By contributing you agree to license your work under the same terms.
Do not copy code from projects with incompatible licenses.

## Reporting bugs and security issues

Bugs: use the issue templates. Include the exact commit SHA, the config
(`artifacts/<variant>/config`), the commands you ran, and the relevant log.

Security: read [SECURITY.md](SECURITY.md). Do not open a public issue for a
memory-safety or privilege-escalation bug.

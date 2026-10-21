# AGENTS.md — instructions for coding agents

This file is for automated coding agents working in the Ferret repository. It is
not contributor documentation; humans should read
[CONTRIBUTING.md](CONTRIBUTING.md).

## Project contract

Ferret is a source-level fork of Linux `v6.18.57`. A migration is **only
complete** when:

1. the C implementation is not compiled into the active build,
2. a Rust object defines the ABI symbols,
3. the exported symbols keep their exact visibility (`EXPORT_SYMBOL` vs GPL),
4. differential tests compare Rust against the original C,
5. a QEMU boot with the Rust component active passes,
6. `tools/ferret/linkage.sh` proves object ownership and symbol sizes,
7. the registry (`registry/migrations.json`) records the evidence.

A Rust wrapper that calls the C implementation is **not** a migration. Do not
describe one as such.

## Never fabricate results

* Do not write PASS for a test you did not run. Use `NOT RUN` and say why.
* Do not claim a clean build if you used an incremental tree with stale
  objects. `CLEAN=1 tools/ferret/test.sh` is the only clean-cycle evidence.
* Do not report throughput from QEMU TCG as if it were native performance.
* If a command fails, report the failure and the exact error. Do not retry
  silently until something passes and then report only the last run.

## Commands

```sh
tools/ferret/build.sh ref            # upstream reference kernel (x86_64)
tools/ferret/build.sh rust           # Ferret kernel (x86_64)
ARCH=arm64 tools/ferret/build.sh rust
tools/ferret/initramfs.sh
tools/ferret/boot.sh ref|rust
tools/ferret/linkage.sh ref|rust
tools/ferret/ubuntu-rootfs.sh amd64|arm64
tools/ferret/boot-ubuntu.sh shell|systemd amd64|arm64
CLEAN=1 tools/ferret/test.sh         # full validation cycle
python3 tools/ferret/registry.py --markdown
```

Builds run inside the `ferret-build` container via `tools/ferret/docker/run.sh`.
The host tree may be a case-sensitive APFS sparsebundle; do not move it to a
case-insensitive volume (filenames such as `xt_MARK.h`/`xt_mark.h` collide).

## Editing rules

* Do not modify upstream Linux files unless the change is part of a migration
  (Kconfig/Makefile switch, `bindings_helper.h` include, new Rust file).
* Do not delete the C reference implementation in the same commit as the Rust
  replacement. It is needed by the differential tests.
* Do not add a fallback path that silently uses C when the Rust path fails.
* Do not reformat unrelated code, rewrite history, force-push, or delete
  branches. The local repository contains the full upstream history and must
  stay intact.
* Keep `unsafe` local, and every block documented with a `// SAFETY:` invariant.
* Kernel Rust must not panic on reachable paths.

## When blocked

Record the blocker in the issue or the checkpoint report with the exact command
and output. Prefer an honest "blocked on X" over an unverified claim.

## Publication

The public repository is a curated snapshot; the canonical development tree is
the private/local one. Public pushes happen from a publication worktree/branch,
never by rewriting the development history. See
`docs/ferret/UPSTREAM.md` for the synchronization workflow.

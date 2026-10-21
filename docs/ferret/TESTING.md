# Testing Ferret

Testing is the product. A migration that cannot be demonstrated is not a
migration, and a test that did not run is not a pass.

## 1. Levels of evidence

| Level | Question it answers | Mechanism |
| --- | --- | --- |
| Unit | Does the Rust function compute the right values? | in-kernel KUnit tests in the `_rust.rs` file |
| Differential | Does Rust behave like the C original? | the original C file compiled with renamed symbols, compared under KUnit |
| Upstream | Does the component still satisfy upstream's own suite? | the upstream KUnit suite, unchanged |
| Linkage | Is the C object really gone, and is the symbol the Rust one? | `tools/ferret/linkage.sh` on `vmlinux` and the build tree |
| Boot | Does the kernel run and reach userspace? | QEMU boot, KUnit output, initramfs markers |
| Integration | Does a real distro userspace work? | Ubuntu 24.04 shell and `systemd` boots |
| Registry | Is the recorded claim consistent with the evidence? | `tools/ferret/registry.py` |

Every result is `PASS`, `FAIL`, `SKIP` (deliberately not applicable, with a
reason) or `NOT RUN` (not executed). `SKIP` is never used to hide a failure.

## 2. Running the tests

Focused work:

```sh
tools/ferret/build.sh rust        # incremental kernel build
tools/ferret/boot.sh rust         # QEMU boot + KUnit + userspace markers
tools/ferret/linkage.sh rust      # symbol and object evidence
```

Full clean cycle (the only thing that qualifies as checkpoint evidence):

```sh
CLEAN=1 tools/ferret/test.sh
```

The clean cycle performs, in order:

1. reference build (unmodified upstream, x86_64)
2. Ferret build (x86_64)
3. Rust initramfs build
4. reference boot
5. Ferret boot
6. linkage evidence, reference and Ferret
7. arm64 Ferret build
8. arm64 linkage evidence
9. Ubuntu amd64 root filesystem image
10. Ubuntu arm64 root filesystem image
11. Ubuntu shell boot, amd64
12. Ubuntu systemd boot, amd64
13. Ubuntu shell boot, arm64
14. Ubuntu systemd boot, arm64
15. registry validation

Results are written to `artifacts/RESULTS.txt`; the console log is the source
for the per-step detail. If a step fails, the cycle stops and reports `FAIL`;
later steps are `NOT RUN`.

## 3. What each check really verifies

### Differential KUnit

The C file is compiled into the test build with its symbol renamed
(`-Dcrc16=crc16_c_ref`) and compared with the Rust implementation:

* fixed vectors from the specification and from upstream tests,
* empty buffers and single bytes,
* incremental/partial updates,
* guard bytes around the output buffer,
* 2000 deterministic pseudo-random streams,
* table contents, where the component has a table.

The random generator is a fixed PRNG so a failure is reproducible from the log.

### Linkage

`tools/ferret/linkage.sh` parses the build tree and `vmlinux`:

* the C object (`lib/crc/crc16.o`) must be **absent**,
* the Rust object (`lib/crc/crc16_rust.o`) must be **present**,
* each exported symbol is defined exactly once,
* the definition comes from the Rust object,
* the linked function's size matches the Rust object's function size,
* the shim object only references the symbol (no definition),
* `__ksymtab_<symbol>` exists and `Module.symvers` shows the original export
  type (`EXPORT_SYMBOL`, not GPL),
* disassembly is printed for manual inspection.

If `CONFIG_DEBUG_INFO` is off, DWARF provenance is reported `SKIP` with the
reason; the object-ownership and size checks still apply.

### Boot

The boot harness runs QEMU with the built kernel and a Rust initramfs, waits for
KUnit to finish, extracts `ok`/`not ok` lines, and checks userspace markers
(`uname`, `mount`, `openat/read`, `reboot`). A boot that does not reach the
markers is `FAIL`, even if the kernel printed a lot of output.

### Ubuntu userspace

Two guests per architecture:

* `init=/sbin/ferret-init` — a shell probe that reports kernel release, machine,
  ext4 mount, `sh` and `bash` execution, `/proc` and a working `getty` login.
* `systemd` as PID 1 — multi-user target, a probe unit that touches a marker,
  and a serial `ferret login:` prompt.

The guest is a real Ubuntu 24.04 cloud-image root filesystem, not a custom
userspace. The harness kills QEMU after the marker; guests cannot reliably power
off under TCG.

## 4. Continuous integration

`.github/workflows/docs.yml` runs on every push and pull request: registry
validation, generated-report freshness, required documents, issue-template
YAML, relative-link checking and a committed-build-product/secret scan. It is
fast (seconds) and is the check that must be green for every change.

`.github/workflows/kernel-smoke.yml` is manual plus weekly. It installs the
pinned toolchain on an `ubuntu-24.04` runner, builds the reference and Ferret
kernels, builds the initramfs, boots both, and collects linkage evidence. It is
not run on pull requests because two kernel builds plus boots exceed a
reasonable PR budget.

**Status.** The `docs` workflow runs green on every push to `main`. The
`kernel-smoke` workflow has been executed on GitHub runners: the reference and
Ferret kernel builds succeed there. Its runs exposed three environment-only
defects in the harness — `llvm-strip` was not linked into the toolchain, the
initramfs script could not create `/dev/console` without privileges, and the
init's `.cargo/config.toml` was never committed (the kernel's `.gitignore`
ignores dotfiles), so a fresh checkout built a static-PIE init that segfaulted
when the kernel executed it. All three are fixed
(`tools/ferret/lib/initramfs-in-container.sh`, the workflow, and the
force-added cargo configuration). A full green `kernel-smoke` run is the
remaining item; until it is green, the authoritative verification evidence
remains the local clean cycle documented in
[`docs/ferret/evidence/`](evidence/).

## 5. Resources

| Task | CPU | RAM | Disk | Wall time (reference machine) |
| --- | --- | --- | --- | --- |
| Incremental x86_64 build | 4+ | 8 GiB | 10 GiB | minutes |
| Clean x86_64 build | 10 | 16 GiB | 20 GiB | ~30-45 min (cold ccache) |
| arm64 build | 10 | 16 GiB | 15 GiB | ~10-20 min |
| Full clean cycle | 10 | 16 GiB | 40 GiB | ~1 h (cold), less with ccache |
| Focused KUnit boot | 2 | 2 GiB | 2 GiB | ~1 min |

A contributor does not need the full cycle to send a focused patch: an
incremental `build.sh rust` plus `boot.sh rust` and `linkage.sh rust` is the
expected local evidence for a library component.

## 6. Adding a test

* Put Rust unit tests in the migrated file behind `#[cfg(CONFIG_KUNIT)]`.
* Add a differential suite `<component>_diff_kunit.c` only when the C original
  can be compiled with renamed symbols; keep it test-only.
* Register new evidence in `registry/migrations.json`; the registry validator
  checks that a `migrated` entry has tests and evidence pointers.
* Never add a test that passes when the implementation is absent. A KUnit suite
  that is silently not built is a `NOT RUN`, not a pass — verify the `ok N
  <suite>` lines in the boot log.

## 7. Known gaps

* No LTP or kselftest run; no sanitizer (KASAN/KCSAN) or lockdep boot.
* No loadable-module Rust component; the Rust CRC variant is built-in only.
* Runtime throughput is not measured natively; QEMU TCG numbers are not
  performance data.
* Container runtimes, networking end-to-end, suspend/resume and physical
  hardware are untested.
* The Ubuntu guests are disposable fixtures and are not hardened.

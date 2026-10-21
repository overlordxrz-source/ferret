# Verification evidence

These files are the published, sanitized summaries of a Ferret validation
cycle. They are excerpts, not raw logs: the full build and boot logs stay
local (`artifacts/`) because they are large and mostly noise. Every file
records the source commit it was produced from.

Regenerate them after a cycle with:

```sh
tools/ferret/evidence.sh <source-commit>
```

| File | Produced by | Contents |
| --- | --- | --- |
| `results-clean-cycle.txt` | `tools/ferret/test.sh` | the per-step PASS/FAIL summary of the clean cycle |
| `boot-x86_64-rust.txt` | `tools/ferret/boot.sh rust` | kernel banner, every KUnit `ok` line, and the userspace ABI markers of the x86_64 Ferret kernel |
| `linkage-x86_64.txt` | `tools/ferret/linkage.sh rust` | object ownership, symbol definitions and sizes, ksymtab entries, export namespace, disassembly |
| `linkage-arm64.txt` | `ARCH=arm64 tools/ferret/linkage.sh rust` | the same checks for the arm64 kernel |
| `boot-ubuntu-shell-amd64.txt` | `tools/ferret/boot-ubuntu.sh shell amd64` | Ubuntu 24.04 guest, shell probe mode |
| `boot-ubuntu-systemd-amd64.txt` | `tools/ferret/boot-ubuntu.sh systemd amd64` | Ubuntu 24.04 guest, systemd as PID 1 |
| `boot-ubuntu-shell-arm64.txt` | `tools/ferret/boot-ubuntu.sh shell arm64` | Ubuntu 24.04 guest, shell probe mode, arm64 |
| `boot-ubuntu-systemd-arm64.txt` | `tools/ferret/boot-ubuntu.sh systemd arm64` | Ubuntu 24.04 guest, systemd as PID 1, arm64 |

## How to read them

* A `PASS` in these files means the harness asserted the corresponding
  condition, not that a human inspected the output. The harness is documented
  in [TESTING.md](../TESTING.md).
* The x86_64 boot evidence lists the KUnit suites that ran, including the ones
  from upstream Rust for Linux (`rust_kernel_str`, `rust_atomics`, …) and the
  upstream CRC suite. A suite that did not run is absent, not passed.
* `linkage-*.txt` shows `SKIP` for DWARF provenance on this baseline, because
  the test configuration carries no debug information; object ownership and
  symbol size carry the provenance argument instead.
* The `Source commit` header is the commit whose *kernel sources* were built.
  Documentation-only commits that follow it do not invalidate the evidence.

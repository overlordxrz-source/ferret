# Verification evidence

These files are the published, sanitized summaries of a Ferret validation
cycle. They are excerpts, not raw logs: the full build and boot logs stay
local (`artifacts/`) because they are large and mostly noise.

Every file is **bound to the tree that produced it**. The header carries four
hashes:

| Field | What it hashes |
| --- | --- |
| `Source tree` | the build-relevant files of the tree at the revision named by `Source commit` (everything except `docs/`, `artifacts/`, `registry/` and the top-level prose) |
| `Harness` | `tools/ferret/` and `AUDIT_PROTOCOL.md`: the scripts and the gate definition |
| `Configuration` | the kernel `.config` the artifacts were built with |
| `Source commit` | the commit the evidence was extracted at |

The binding is produced by `tools/ferret/lib/provenance.sh`, which stamps
every artifact, and checked by `tools/ferret/evidence.sh`, which **refuses**
an input whose stamp does not match the current source tree and harness. An
artifact from a dirty tree is rejected as well: it cannot name a revision.

Regenerate them after a cycle with:

```sh
tools/ferret/evidence.sh
```

| File | Produced by | Contents |
| --- | --- | --- |
| `results-clean-cycle.txt` | `tools/ferret/test.sh` | the per-step PASS/FAIL summary of the clean cycle |
| `boot-x86_64-rust.txt` | `tools/ferret/boot.sh rust` | kernel banner, every KUnit `ok` line, the explicit **audited suite verdicts**, and the userspace ABI markers of the x86_64 Ferret kernel |
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
* The x86_64 boot evidence has an **Audited suites** section that names the
  four suites of the reference-counting checkpoint
  (`refcount_rust`, `refcount_diff`, `fvs_refcount`, `fvs_refcount_adv`) with
  their pass/fail/skip/total counts, and then the verdict of every suite in
  the boot. `evidence.sh` fails if one of the four did not run or did not
  pass; a suite that did not run is never published as passed.
* The boot evidence also carries the concurrency line
  `ferret refcount: retry path exercised: N retries (attempt M of 5)`. That
  line is the measurement FVS-AUDIT-001-R2 asked for: the compare-and-exchange
  retry path was counted, not inferred, and the test fails if it was never
  observed.
* `linkage-*.txt` shows `SKIP` for DWARF provenance on this baseline, because
  the test configuration carries no debug information; object ownership and
  symbol size carry the provenance argument instead.
* `Source commit` is the commit whose kernel sources were built.
  Documentation-only commits that follow it do not invalidate the evidence —
  and they do not, because the source hash ignores `docs/` — but a change to
  the kernel, the harness or the configuration does: the evidence is then
  stale and `evidence.sh` refuses it.

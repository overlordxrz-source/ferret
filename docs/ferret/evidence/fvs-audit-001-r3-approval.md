# FVS-AUDIT-001-R3 — approval of checkpoint 005

| Item | Value |
| --- | --- |
| Verdict | **APPROVE** |
| Audited revision | `c49a7437aa1892503360c7b4f87df7cbe7ab9ba6` (PR #24 head) |
| Merged as | PR #24, merge commit `33629bd547af0a3894b0fd9604ece469cc14091c` |
| Merged at | 2026-10-23T01:22:33Z |
| Merged tree | identical to the audited revision (main was an ancestor; `33629bd^{tree} == c49a743^{tree}`) |
| Scope | checkpoint 005, the out-of-line reference counting helpers (`lib/refcount.c` → `lib/refcount_rust.rs`) |
| Earlier rounds | R1 (evidence weaker than it read, 17 findings) and R2 (no implementation defect, three evidence findings) — dispositions in [`../design/COMPONENT_005_AUDIT.md`](../design/COMPONENT_005_AUDIT.md) §12 |

The code revision that the evidence artifacts are stamped with is
`058e0ca1ddce`; `c49a7437aa18` adds the evidence and the report on top of it.

## Verification at the approved revision

| Check | Result |
| --- | --- |
| CI `validate` | pass |
| Clean cycle `CLEAN=1 tools/ferret/test.sh` | 16/16 |
| Gate `tools/ferret/audit/audit.sh --skip-done` | PASS (phases 5 and 6 skipped with `provenance: OK`) |
| Mutation campaign | 9 detected, 1 declared not detectable, 0 holes, 0 invalid |
| Retry path | 5/5 boots on the final code, 2119–3404 retries, every one on the first attempt |
| KASAN + UBSAN | PASS (four audited suites inside) |
| KCSAN | PASS, **non-strict** |
| Provenance | every artifact bound to the source, harness, configuration and image |

## Accepted limitations

These are carried into the checkpoint record; they are limits of the
evidence, not defects that were found and left unfixed.

1. **The memory ordering is not verified by a functional test.** On x86_64 a
   relaxed compare-and-exchange and a release one are the same instruction,
   so no functional test can see the difference; the mitigation is the code
   review and the disassembly. The mutation campaign records the ordering
   mutation as a declared limit, not as a pass.
2. **KCSAN runs in its default, non-strict mode.** It reports races between
   instrumented accesses; it does not model every weak memory ordering. A
   clean KCSAN run is *not* evidence that the ordering is correct.
3. **The arm64 test kernel is not booted.** The arm64 kernel builds and its
   linkage report is green; the boot harness is x86_64-only (the arm64 Ubuntu
   guests do boot).
4. **Native performance is not measured.** The host has no KVM, so any timing
   would be QEMU TCG. No throughput number is claimed.
5. **`KRef` cannot enforce the lifetime of a C-owned object.** The handle
   adopts a reference the caller must already hold; the object must outlive
   the handle, and that remains the caller's responsibility.
6. **The release callback running in another task is not covered.** No test
   drops a handle from a second task; this is declared in
   [`../design/COMPONENT_005_AUDIT.md`](../design/COMPONENT_005_AUDIT.md) §9.
7. **Harness follow-ups** are tracked in
   [issue #27](https://github.com/overlordxrz-source/ferret/issues/27): the
   configuration archive next to the `test.sh` and `mutation-test.sh` stamps,
   and the stale concurrency-test comment.

## What the approval is, and is not

The audit approved the checkpoint on the evidence above. It is not a proof of
correctness: the tests notice an enumerated set of injected defects, and the
review argues about the rest. The limitations above are the parts that are
declared rather than demonstrated.

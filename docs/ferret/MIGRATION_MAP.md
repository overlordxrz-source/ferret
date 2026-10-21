# The Ferret Migration Map

The migration is published as a set of issues, not as a hidden queue. Each issue
is a self-contained engineering puzzle: it names the C code, the Rust
destination, the symbols, the safety invariants, the tests and the exit
criteria, so that an experienced contributor can pick it up without reading the
whole repository.

## 1. Where the puzzles live

* **Migrations** — issues labelled `type: migration`.
* **Verification work** — issues labelled `type: verification`.
* **Design / feasibility** — issues labelled `type: investigation`. These make
  no implementation claim; they produce an evidence-backed plan.

Open issues: <https://github.com/overlordxrz-source/ferret/issues?q=is%3Aissue+is%3Aopen+label%3A%22type%3Amigration%22>

The authoritative list of what has actually been migrated is the
[migration registry](../../registry/migrations.json), rendered as
[REGISTRY_REPORT.md](REGISTRY_REPORT.md) and summarised in
[MIGRATION_MATRIX.md](MIGRATION_MATRIX.md). Issues describe planned work;
the registry describes measured work.

## 2. Status workflow

The board columns are represented by the `status:` label, so the workflow works
with or without a GitHub Project board.

| Column | Label | Meaning |
| --- | --- | --- |
| BACKLOG | *(no `status:` label)* | identified, not yet scoped enough to be claimed |
| READY | `status: ready` | scoped, unclaimed, actionable now |
| CLAIMED | `status: claimed` | a contributor said they are working on it and a maintainer acknowledged it |
| IN PROGRESS | `status: claimed` + a linked draft PR | implementation started |
| REVIEW | `status: claimed` + an open PR | under review |
| VERIFIED | closed as done, registry entry updated | the acceptance criteria are met and the evidence is committed |
| BLOCKED | `status: blocked` | waiting for a named prerequisite |

Difficulty is the `difficulty:` label, the subsystem is the `area:` label, and
the review requirements are the `needs:` labels.

## 3. Claiming a puzzle

1. Check the open issues: an issue labelled `status: ready` is unclaimed.
2. Comment on the issue with a short plan and the date you expect to open a
   pull request.
3. A maintainer replies, adds `status: claimed` and assigns the issue when
   permissions allow. External contributors cannot self-assign, and they do not
   need to: the label is the signal.
4. Open a draft pull request early and link it from the issue. The issue moves
   to "under review" when the pull request is ready.
5. If you stop, say so in the issue. The maintainer returns it to
   `status: ready` rather than leaving it stalled.

Avoiding duplicate work relies on two things: the `status: claimed` label and
the claim comment. Both are checked before a maintainer merges.

## 4. Difficulty levels

| Level | Meaning | Typical examples |
| --- | --- | --- |
| 1 — Starter | small deterministic utility, documentation, registry and build tooling, tests | CRC-8, CRC-7, link checker, registry validator |
| 2 — Intermediate | bounded kernel component with exports, lifecycle, scoped allocation or synchronisation | `lib/kfifo.c`, differential harness generalisation |
| 3 — Advanced | drivers, filesystem internals, networking abstractions, ownership-heavy code | a virtio driver, `fs/eventfd.c` |
| 4 — Expert | memory management, scheduling, SMP, synchronisation primitives, architecture internals | page allocator, runqueue operations, `arch/*` internals |
| 5 — Research | large cross-subsystem redesigns, ABI-preserving migrations, proof and verification challenges | replacing `sk_buff`, a Rust VFS lifetime model |

A component is never called "easy" because it is small. Kernel work carries
ABI, concurrency and failure-mode obligations that a naive reading of the diff
does not show. Levels 4 and 5 are research: they usually begin as
`type: investigation` issues, and a design review is required before
implementation starts.

## 5. What "DONE" means

A puzzle is complete when **all** of the following are true:

1. The C implementation is not compiled in the Rust variant.
2. A Rust object defines the ABI symbols, with the original signatures.
3. The exported symbols keep their exact visibility (`EXPORT_SYMBOL` vs
   `EXPORT_SYMBOL_GPL`).
4. A differential test compares the Rust implementation with the original C
   over edge cases and randomised inputs.
5. The upstream KUnit suite for that component still passes unchanged.
6. `tools/ferret/linkage.sh` proves object ownership, a single definition, the
   function size and the ksymtab/export metadata.
7. `tools/ferret/boot.sh` passes with the component active.
8. The registry entry is updated and `python3 tools/ferret/registry.py --markdown`
   exits 0.

A Rust wrapper that forwards to the C implementation is transitional
infrastructure, not a completed rewrite, and is labelled as such.

## 6. The GitHub Project board

A GitHub Project named **Ferret Migration Map** mirrors the workflow above
(BACKLOG → READY → CLAIMED → IN PROGRESS → REVIEW → VERIFIED, plus BLOCKED),
grouped by subsystem, difficulty, status, contributor and dependencies.

The project board is optional: the `status:` label carries the same state, so
nothing about contributing depends on it. If the board is not visible to you,
use the labels and the issue list.

## 7. Related documents

* [Contributing guide](../../CONTRIBUTING.md) — workflow, Rust conventions, PR
  expectations, licensing.
* [Porting policy](PORTING_POLICY.md) — what counts as a migration, what is
  rejected.
* [Testing](TESTING.md) — the evidence a migration must produce.
* [Architecture](ARCHITECTURE.md) — how the pieces fit together.
* [Roadmap](ROADMAP.md) — the twelve workstreams.

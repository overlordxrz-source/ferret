# Upstream provenance and publishing

Ferret is a fork, not a rewrite from scratch. Every line that is not Ferret's
comes from Linux, and the project must be able to prove which line.

## 1. Baseline

| Item | Value |
| --- | --- |
| Project | Linux kernel (stable tree) |
| Source | <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git> |
| Tag | `v6.18.57` |
| Commit | `2ff600d29ee478d0a8d46ef8a58afb5e038fe692` |
| License | GPL-2.0-only (plus per-file licenses, unchanged) |

The baseline is pinned. Ferret does not track `master`; moving the baseline is a
deliberate, reviewed decision recorded in `DECISIONS.md`.

## 2. Repository model

There are two repositories:

1. **The maintainer development tree** — a full clone of the upstream history
   plus Ferret branches. It is large (several GB) because it contains every
   upstream object. It is never rewritten, filtered or force-pushed.
2. **The public repository** — a curated, contributor-friendly history:
   * commit 1: the pristine `v6.18.57` tree, with no parent (a snapshot, not the
     upstream object graph),
   * subsequent commits: Ferret changes, one checkpoint or merged PR at a time.

The public history deliberately does not contain the ~7 GB of upstream objects.
That history is not needed to build, review or reproduce Ferret; the pinned
commit above is the provenance link.

### Why a snapshot instead of pushing the full history

Upstream Linux's full history is measured in gigabytes. Mirroring it would make
the repository impractical to clone and would add no information that the pinned
commit SHA does not already provide. The snapshot keeps every source file of the
exact baseline, so `diff` against the upstream tarball at `2ff600d2` is
meaningful.

## 3. What is published

Published:

* the complete kernel source tree at the baseline, and the current Ferret tree;
* Ferret's Rust components, Kconfig/Makefile changes, shims and tests;
* `docs/ferret/`, the registry, the build/test harness, contributor docs,
  issue/PR templates, CI workflows;
* sanitized verification summaries in `docs/ferret/evidence/`.

Not published:

* `.git` of the development tree and upstream history objects;
* built kernels, object files, `.config`, initramfs images;
* Docker images, sparsebundles, ccache;
* raw build logs and full boot logs (large and noisy); the summaries contain the
  measured results and the relevant marker lines;
* anything containing credentials or maintainer-specific absolute paths.

The published tree is generated from a clean index, not by copying a working
directory, so untracked scratch files cannot leak.

## 4. How future work is published

The public repository is the canonical collaboration surface. The maintainer
workspace keeps the full upstream history as an archive and tracks the public
`main` branch for day-to-day work:

```sh
# one-time: add the public repository
git remote add public https://github.com/overlordxrz-source/ferret.git

# work on a branch that starts from the published history
git fetch public
git switch -c work public/main

# ... implement, test, commit ...

# publish a checkpoint (fast-forward; never force-push main)
git push public work:main
```

For a release checkpoint, the published commit message summarizes what was
merged and links the issues. The tree is verified with
`CLEAN=1 tools/ferret/test.sh` before it is pushed; the evidence summary is
regenerated from `artifacts/` and committed with it.

External pull requests are merged into `public/main` on GitHub after review and
the required checks. The maintainer then fetches `public/main` into the
workspace before continuing. Because the public history is linear and
append-only, this is always a fast-forward.

Force-pushing `main`, rewriting published commits, or deleting contributor
branches is forbidden.

## 5. Licensing obligations

* Ferret is distributed under **GPL-2.0-only**, the kernel's license.
* Every upstream file keeps its SPDX identifier, copyright notices and license
  text. Ferret does not relicense the kernel.
* New Ferret files carry `SPDX-License-Identifier: GPL-2.0-only` (or a
  compatible identifier already used by the file they extend).
* `COPYING` and `LICENSES/` are unchanged from upstream.
* Binary distribution of a Ferret kernel carries the same source-offer
  obligations as any Linux kernel. The published source tree *is* the
  corresponding source for the published commits.
* Contributors sign off commits (DCO), as in upstream kernel development.

## 6. Relationship to Rust for Linux

Ferret builds on [Rust for Linux](https://rust-for-linux.com/): the in-tree
`kernel` crate, the Rust build integration, `#[export]`, and the
bindgen-generated bindings. Those components are upstream work, not Ferret's.

Ferret is an independent project. It is **not** affiliated with or endorsed by
the Linux Foundation, the upstream Linux maintainers, the Rust for Linux
maintainers, Canonical, or the Rust project. The name "Linux" is used
descriptively, to identify the codebase being modified.

## 7. Reporting provenance problems

If a file appears without the correct license header, or a Ferret change cannot
be traced to a commit in this repository, open an issue with the path and the
commit SHA. Provenance bugs are treated as release blockers.

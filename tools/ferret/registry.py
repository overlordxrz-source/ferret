#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Ferret migration registry report generator.

Reads registry/migrations.json and prints the required summary:

  1. components analysed
  2. components genuinely migrated
  3. components tested
  4. components with remaining C dependencies
  5. remaining migration candidates
  6. measured build/runtime regressions

With --markdown it also (re)writes docs/ferret/REGISTRY_REPORT.md.

Exit status is 1 if a component claims "migrated" without the evidence the
porting policy requires, so the report is also a consistency check.
"""

import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
REGISTRY = ROOT / "registry" / "migrations.json"
REPORT = ROOT / "docs" / "ferret" / "REGISTRY_REPORT.md"

MIGRATED = "migrated"
CANDIDATE_PREFIX = "candidate"


def load():
    with REGISTRY.open() as f:
        return json.load(f)


def validate(data):
    problems = []
    for c in data["components"]:
        if c["status"] == MIGRATED:
            if not c.get("rust_compiled"):
                problems.append(f"{c['id']}: migrated but rust_compiled is false")
            if not c.get("c_implementation_replaced"):
                problems.append(f"{c['id']}: migrated but C implementation not replaced")
            if c.get("c_object_built_in_rust_variant"):
                problems.append(f"{c['id']}: migrated but C object still built")
            if not c.get("rust_sources"):
                problems.append(f"{c['id']}: migrated without Rust sources")
            passing = [t for t in c.get("tests", []) if t.get("result") == "PASS"]
            if not passing:
                problems.append(f"{c['id']}: migrated without passing test evidence")
            for t in c.get("tests", []):
                ev = t.get("evidence")
                if ev and not (ROOT / ev).exists():
                    problems.append(f"{c['id']}: evidence file missing: {ev}")
    return problems


def summary(data):
    comps = data["components"]
    analysed = len(comps)
    migrated = [c for c in comps if c["status"] == MIGRATED]
    tested = [c for c in comps if any(t.get("result") == "PASS" for t in c.get("tests", []))]
    with_cdeps = [c for c in comps if c.get("remaining_c_dependencies")]
    candidates = [c for c in comps if c["status"].startswith(CANDIDATE_PREFIX)]
    return {
        "analysed": analysed,
        "migrated": migrated,
        "tested": tested,
        "with_cdeps": with_cdeps,
        "candidates": candidates,
    }


def render(data):
    s = summary(data)
    base = data["baseline"]
    out = []
    out.append("# Ferret migration registry report\n")
    out.append("Generated from `registry/migrations.json` by "
               "`tools/ferret/registry.py`. Do not edit by hand.\n")
    out.append(f"Baseline: `{base['tag']}` (`{base['commit']}`)\n")

    out.append("## 1. Components analysed\n")
    out.append("| Component | Subsystem | Status | Rust compiled | C replaced |")
    out.append("| --- | --- | --- | --- | --- |")
    for c in data["components"]:
        out.append(
            f"| `{c['id']}` | {c['subsystem']} | {c['status']} | "
            f"{'yes' if c.get('rust_compiled') else 'no'} | "
            f"{'yes' if c.get('c_implementation_replaced') else 'no'} |"
        )
    out.append("")

    out.append("## 2. Components genuinely migrated\n")
    if s["migrated"]:
        for c in s["migrated"]:
            out.append(f"* `{c['id']}` — {c['title']}: "
                       f"{', '.join(c['c_sources'])} → {', '.join(c['rust_sources'])}")
    else:
        out.append("_None._")
    out.append("")

    out.append("## 3. Components tested\n")
    out.append("| Component | Test | Kind | Result | Evidence |")
    out.append("| --- | --- | --- | --- | --- |")
    for c in data["components"]:
        for t in c.get("tests", []):
            out.append(f"| `{c['id']}` | {t['name']} | {t['kind']} | "
                       f"{t['result']} | `{t.get('evidence', '-')}` |")
    out.append("")

    out.append("## 4. Components with remaining C dependencies\n")
    if s["with_cdeps"]:
        for c in s["with_cdeps"]:
            for d in c["remaining_c_dependencies"]:
                kind = "algorithm" if d.get("algorithm") else "linkage/metadata"
                out.append(f"* `{c['id']}` — `{d['file']}` ({kind}): {d['reason']}")
    else:
        out.append("_None._")
    out.append("")

    out.append("## 5. Remaining migration candidates\n")
    out.append("| Component | Status | Dependencies | Risks |")
    out.append("| --- | --- | --- | --- |")
    for c in s["candidates"]:
        out.append(f"| `{c['id']}` | {c['status']} | "
                   f"{', '.join(c.get('dependencies') or ['-'])} | "
                   f"{' '.join(c.get('risks') or ['-'])} |")
    out.append("")

    out.append("## 6. Measured build/runtime regressions\n")
    if data.get("benchmarks"):
        out.append("| Benchmark | Reference | Ferret | Delta | Notes |")
        out.append("| --- | --- | --- | --- | --- |")
        for b in data["benchmarks"]:
            out.append(f"| {b['name']} | {b['reference']} | {b['ferret']} | "
                       f"{b['delta']} | {b.get('notes', '')} |")
    else:
        out.append("_No benchmark data recorded yet. Runtime performance is "
                   "NOT TESTED; no performance claims are made._")
    if data.get("regressions"):
        for r in data["regressions"]:
            out.append(f"* {r}")
    out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--markdown", action="store_true",
                    help="write docs/ferret/REGISTRY_REPORT.md")
    args = ap.parse_args()

    data = load()
    problems = validate(data)
    text = render(data)
    if args.markdown:
        REPORT.write_text(text + "\n")
        print(f"wrote {REPORT.relative_to(ROOT)}")
    else:
        print(text)
    if problems:
        print("\nREGISTRY INCONSISTENCIES:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

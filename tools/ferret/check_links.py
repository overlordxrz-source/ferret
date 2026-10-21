#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check relative links and image references in Markdown files.

External (http/https/mailto) links and pure anchors are ignored. Links inside
fenced code blocks are ignored. Exits non-zero listing every broken reference.

Usage: tools/ferret/check_links.py [root]
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
SKIP_DIRS = {".git", "artifacts", "target", "node_modules"}

LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")


def markdown_files():
    """Ferret's public docs only; upstream Documentation/ is out of scope."""
    for path in ROOT.glob("*.md"):
        yield path
    for base in (ROOT / "docs" / "ferret", ROOT / ".github"):
        if base.is_dir():
            for path in base.rglob("*.md"):
                yield path


def strip_code_fences(text):
    out = []
    in_fence = False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            out.append(line)
    return "\n".join(out)


def main():
    broken = []
    checked = 0
    for md in markdown_files():
        text = strip_code_fences(md.read_text(encoding="utf-8", errors="replace"))
        for target in LINK_RE.findall(text):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            if target.startswith("<") and target.endswith(">"):
                target = target[1:-1]
            target = target.split("#", 1)[0]
            if not target:
                continue
            checked += 1
            resolved = (md.parent / target).resolve()
            if not resolved.exists():
                broken.append(f"{md.relative_to(ROOT)}: {target}")

    print(f"checked {checked} relative links in Markdown files")
    if broken:
        print("broken references:")
        for item in broken:
            print(f"  {item}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Record the exact toolchain and environment used for a checkpoint.
#
#   tools/ferret/env.sh          (host + container)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/artifacts/env.txt
mkdir -p "$ROOT/artifacts"

{
	echo "Ferret environment record"
	echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo
	echo "== host =="
	uname -a
	echo
	echo "== container =="
	"$HERE/docker/run.sh" bash -c '
		set -e
		echo "uname: $(uname -a)"
		echo "cpus: $(nproc)"
		echo
		echo "clang: $(clang --version | head -1)"
		echo "ld.lld: $(ld.lld --version | head -1)"
		echo "llvm-ar: $(llvm-ar --version | head -1)"
		echo "make: $(make --version | head -1)"
		echo "git: $(git --version)"
		echo "rustc: $(rustc --version)"
		echo "cargo: $(cargo --version)"
		echo "rustup: $(rustup --version 2>/dev/null | tail -1)"
		echo "rust targets: $(rustup target list --installed | tr "\n" " ")"
		echo "bindgen: $(bindgen --version)"
		echo "qemu: $(qemu-system-x86_64 --version | head -1)"
		echo "cpio: $(cpio --version | head -1)"
	'
	echo
	echo "== repository =="
	git -C "$ROOT" log -1 --format='commit %H%nsubject %s%ndate %ad'
	git -C "$ROOT" describe --tags --always 2>/dev/null || true
} >"$OUT" 2>&1

echo "wrote ${OUT#"$ROOT/"}"
cat "$OUT"

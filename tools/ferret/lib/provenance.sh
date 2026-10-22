#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Provenance: bind an evidence artifact to the source, the scripts and the
# configuration that produced it.
#
# An artifact is only evidence for the tree it was made from. This file hashes
# the two things that can silently make it stale:
#
#   source   the build-relevant files of the tree (everything except docs/,
#            artifacts/, registry/ and the top-level prose), by blob hash
#   scripts  tools/ferret/ and AUDIT_PROTOCOL.md: the harness and the gate
#            definition. A change to either invalidates the artifact.
#
# and records, for the artifacts that have one:
#
#   config   the sha256 of the kernel .config that was built, with a copy of
#            the file stored next to the artifact (config-<hash>.txt), so the
#            binding can be rechecked after the build tree has been rebuilt
#   image    the sha256 of the kernel image that was built or booted
#
# The stamp line is
#
#   provenance: commit=<sha> source=<sha256> scripts=<sha256> config=<sha256|->
#               image=<sha256|-> dirty=<0|1> generated=<UTC>
#
# and `prov_check` rejects an artifact whose stamp does not match the current
# tree. A dirty tree is never accepted: `git status` of the build-relevant
# paths has to be clean.
#
# Sourced by build.sh, boot.sh, boot-ubuntu.sh, linkage.sh, initramfs.sh and
# by the audit scripts.

PROV_ROOT=${PROV_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
OUTROOT=${OUTROOT:-/build/ferret}

# macOS has shasum, the container has sha256sum.
if command -v sha256sum >/dev/null 2>&1; then
	PROV_SHA=sha256sum
else
	PROV_SHA="shasum -a 256"
fi

prov_hash_file() { # <file>
	$PROV_SHA "$1" | cut -d' ' -f1
}

prov_hash_stdin() {
	$PROV_SHA | cut -d' ' -f1
}

# The build-relevant paths: the kernel source, the Rust crate, the harness.
prov_paths() {
	printf '%s\n' \
		arch block certs crypto drivers fs include init ipc kernel lib mm net \
		rust scripts security sound tools usr virt Makefile Kconfig Kbuild
}

prov_commit() {
	git -C "$PROV_ROOT" rev-parse HEAD 2>/dev/null || echo unknown
}

prov_commit_short() {
	git -C "$PROV_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown
}

prov_dirty() {
	local st
	st=$(git -C "$PROV_ROOT" status --porcelain --untracked-files=normal -- \
		$(prov_paths) 2>/dev/null)
	[ -n "$st" ] && echo 1 || echo 0
}

prov_source_hash() {
	# ls-tree prints "<mode> blob <sha>\t<path>"; the blob hashes are the
	# content, so the hash of the listing is the hash of the tree.
	git -C "$PROV_ROOT" ls-tree -r --full-tree HEAD -- $(prov_paths) 2>/dev/null |
		LC_ALL=C sort | prov_hash_stdin
}

prov_scripts_hash() {
	git -C "$PROV_ROOT" ls-tree -r --full-tree HEAD -- tools/ferret AUDIT_PROTOCOL.md 2>/dev/null |
		LC_ALL=C sort | prov_hash_stdin
}

# Read a field of a "key=value" line. The first field has no space before it.
prov_field() { # <line> <key>
	printf ' %s\n' "$1" | sed -n 's/.* '"$2"'=\([^ ]*\).*/\1/p'
}

# The sha256 of a file inside the build container (the build trees live there).
prov_container_hash() { # <path-in-container>
	"$PROV_ROOT/tools/ferret/docker/run.sh" sha256sum "$1" 2>/dev/null | cut -d' ' -f1
}

prov_arch_suffix() { # <arch>
	[ "${1:-x86_64}" = x86_64 ] || echo "-$1"
}

prov_config_hash() { # <variant> [arch]
	prov_container_hash "$OUTROOT/linux-$1$(prov_arch_suffix "${2:-x86_64}")/.config"
}

prov_image_hash() { # <variant> [arch]
	local arch=${2:-x86_64} img
	if [ "$arch" = arm64 ]; then
		img=arch/arm64/boot/Image
	else
		img=arch/x86/boot/bzImage
	fi
	prov_container_hash "$OUTROOT/linux-$1$(prov_arch_suffix "$arch")/$img"
}

# The stamp itself. config/image may be "-" for an artifact that does not
# depend on a kernel build.
prov_stamp() { # <config|-> <image|->
	printf 'provenance: commit=%s source=%s scripts=%s config=%s image=%s dirty=%s generated=%s\n' \
		"$(prov_commit_short)" "$(prov_source_hash)" "$(prov_scripts_hash)" \
		"${1:--}" "${2:--}" "$(prov_dirty)" \
		"$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}

# Keep a copy of the configuration next to the artifact, named by its hash, so
# that the binding can be rechecked after the build tree is long gone.
prov_store_config() { # <config-file> <artifact-dir>
	local f=$1 dir=$2 hash
	[ -f "$f" ] || return 1
	hash=$(prov_hash_file "$f")
	mkdir -p "$dir"
	[ -f "$dir/config-$hash.txt" ] || cp "$f" "$dir/config-$hash.txt"
	echo "$hash"
}

# The build wrapper calls this after a successful build.
prov_write_build() { # <variant> <arch> <artifact-dir>
	local v=$1 a=$2 dir=$3 cfg img
	mkdir -p "$dir"
	cfg=$(prov_config_hash "$v" "$a")
	img=$(prov_image_hash "$v" "$a")
	if [ -n "$cfg" ] && [ -f "$dir/config" ]; then
		# The copy the build wrote has to be the configuration that was
		# hashed; if it is not, the artifact would name a config that no
		# longer describes it.
		if [ "$(prov_hash_file "$dir/config")" != "$cfg" ]; then
			echo "provenance: $dir/config does not match the built .config" >&2
			return 1
		fi
		prov_store_config "$dir/config" "$dir" >/dev/null
	fi
	prov_stamp "$cfg" "$img" >"$dir/provenance.txt"
	echo "==> provenance written to $dir/provenance.txt"
}

# The boot wrapper calls this after a boot. It also records the image that was
# actually booted, and refuses to stamp a boot of an image other than the one
# the build in this artifact directory produced. The initramfs path is a
# container path (it lives in the build tree).
prov_write_boot() { # <variant> <arch> <artifact-dir> [initramfs-container-path]
	local v=$1 a=$2 dir=$3 init=${4:-} img cfg built init_hash
	mkdir -p "$dir"
	img=$(prov_image_hash "$v" "$a")
	cfg=$(prov_config_hash "$v" "$a")
	if [ -f "$dir/provenance.txt" ]; then
		built=$(prov_field "$(grep -a '^provenance: commit=' "$dir/provenance.txt" | tail -1)" image)
		if [ -n "$built" ] && [ -n "$img" ] && [ "$built" != "$img" ]; then
			echo "provenance: the booted image ($img) is not the one the build" >&2
			echo "            in $dir produced ($built): the tree was rebuilt" >&2
			return 1
		fi
	fi
	init_hash=-
	if [ -n "$init" ]; then
		init_hash=$(prov_container_hash "$init")
		[ -n "$init_hash" ] || init_hash=-
	fi
	{
		prov_stamp "$cfg" "$img"
		printf 'booted-image: %s\n' "$img"
		printf 'initramfs: %s\n' "$init_hash"
	} >"$dir/boot-provenance.txt"
	echo "==> provenance written to $dir/boot-provenance.txt"
}

# Check an artifact's last provenance stamp against the current tree.
# Returns 0 when the artifact was produced by this exact source and these
# exact scripts, from a clean tree; prints the reason and returns 1 otherwise.
prov_check() { # <artifact-or-stamp-file>
	local f=$1 line p_source p_scripts p_dirty p_config p_commit dir
	if [ ! -f "$f" ]; then
		echo "provenance: $f does not exist"
		return 1
	fi
	# The stamp is the line with the fields, not the "wrote ..." progress
	# messages that the wrappers print into the logs.
	line=$(grep -a '^provenance: commit=' "$f" | tail -1)
	if [ -z "$line" ]; then
		echo "provenance: $f has no provenance stamp"
		return 1
	fi
	p_commit=$(prov_field "$line" commit)
	p_source=$(prov_field "$line" source)
	p_scripts=$(prov_field "$line" scripts)
	p_dirty=$(prov_field "$line" dirty)
	p_config=$(prov_field "$line" config)
	dir=$(dirname "$f")

	if [ "$p_dirty" != 0 ]; then
		echo "provenance: $f was produced from a dirty tree (dirty=$p_dirty)"
		return 1
	fi
	if [ "$p_source" != "$(prov_source_hash)" ]; then
		echo "provenance: $f was produced from another source tree"
		echo "            artifact source: $p_source"
		echo "            current  source: $(prov_source_hash)"
		return 1
	fi
	if [ "$p_scripts" != "$(prov_scripts_hash)" ]; then
		echo "provenance: $f was produced by another version of the harness"
		echo "            artifact scripts: $p_scripts"
		echo "            current  scripts: $(prov_scripts_hash)"
		return 1
	fi
	if [ "$p_config" != - ]; then
		if [ ! -f "$dir/config-$p_config.txt" ]; then
			echo "provenance: $f names configuration $p_config, which is not archived"
			return 1
		fi
		if [ "$(prov_hash_file "$dir/config-$p_config.txt")" != "$p_config" ]; then
			echo "provenance: the archived configuration of $f does not match its stamp"
			return 1
		fi
	fi
	echo "provenance: OK  $f (commit $p_commit, config ${p_config:0:12})"
	return 0
}

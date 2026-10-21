#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Static linkage / symbol-ownership evidence for a kernel variant.
#
#   linkage-in-container.sh ref|rust
#
# Writes artifacts/<variant>/linkage.txt and exits non-zero if any mandatory
# check fails. Optional checks report SKIP with a reason.
set -uo pipefail

VARIANT=${1:?usage: linkage-in-container.sh <ref|rust>}
ARCH=${ARCH:-x86_64}
OUTROOT=${OUTROOT:-/build}
case "$ARCH" in
x86_64) ARCH_SUFFIX= ;;
*) ARCH_SUFFIX="-$ARCH" ;;
esac
OUT=$OUTROOT/linux-$VARIANT$ARCH_SUFFIX
ART=/mnt/projects/ferret/artifacts/$VARIANT$ARCH_SUFFIX
LOG=$ART/linkage.txt
VMLINUX=$OUT/vmlinux
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$ART"
: >"$LOG"
fail=0

say() { echo "$*" | tee -a "$LOG"; }
pass() { say "PASS  $*"; }
bad() {
	say "FAIL  $*"
	fail=1
}
skip() { say "SKIP  $*"; }
note() { say "      $*"; }

section() {
	say ""
	say "== $* =="
}

if [ ! -f "$VMLINUX" ]; then
	say "FAIL  vmlinux missing: $VMLINUX"
	exit 1
fi

nm "$VMLINUX" >"$TMP/vmlinux.nm" 2>/dev/null
nm --print-size "$VMLINUX" >"$TMP/vmlinux.nmsize" 2>/dev/null

# nm output for one object, cached in a temp file.
objnm() { # <object-path> <cache-name>
	nm --print-size "$1" >"$TMP/$2" 2>/dev/null
}

# Size of a symbol in an nm --print-size listing.
symsize() { # <nm-file> <symbol>
	awk -v s="$2" '$4==s{print $2; exit}' "$1"
}

# Number of definitions of a symbol in vmlinux (T/t/D/d/R/r/B/b).
defcount() { # <symbol>
	grep -cE "^[0-9a-f]+ [0-9a-f]+ [TtDdRrBb] $1\$" "$TMP/vmlinux.nmsize"
}

# Check that a linked symbol's size equals its size in the defining object.
check_size() { # <symbol> <obj-size> <object-description>
	local sym=$1 obj=$2 desc=$3 link
	link=$(symsize "$TMP/vmlinux.nmsize" "$sym")
	note "size $sym: object=0x$obj vmlinux=0x$link bytes"
	if [ -n "$obj" ] && [ "$obj" = "$link" ]; then
		pass "linked $sym has the size of the function in the $desc"
	else
		bad "linked $sym size differs from the $desc"
	fi
}

check_ksymtab() { # <symbol>
	if grep -qE "__ksymtab_$1\$" "$TMP/vmlinux.nm"; then
		pass "__ksymtab_$1 exists (module ABI preserved)"
	else
		bad "__ksymtab_$1 missing"
	fi
}

check_symvers() { # <symbol> <expected-export>
	local line type
	line=$(awk -F'\t' -v s="$1" '$2==s' "$OUT/Module.symvers" | head -1)
	note "Module.symvers: ${line:-<none>}"
	type=$(printf '%s' "$line" | awk '{print $NF}')
	if [ "$type" = "$2" ]; then
		pass "$1 exported as $2"
	else
		bad "$1 export type is '${type:-none}', expected $2"
	fi
}

section "kernel variant"
say "variant:  $VARIANT"
say "vmlinux:  $VMLINUX"
say "git:      $(git -C /mnt/projects/ferret rev-parse --short HEAD 2>/dev/null || echo unknown)"
say "config:   $(grep -E '^CONFIG_(CRC16|CRC16_RUST|CRC16_DIFF_KUNIT_TEST|CRC_CCITT|CRC_CCITT_RUST|CRC_CCITT_DIFF_KUNIT_TEST|RUST|KUNIT)=' "$OUT/.config" | tr '\n' ' ')"

section "build tree object ownership"
if [ "$VARIANT" = rust ]; then
	check_absent() { # <path> <description>
		if [ -e "$1" ]; then
			bad "$2 is present in the Rust variant"
		else
			pass "$2 is absent from the build"
		fi
	}
	check_absent "$OUT/lib/crc/crc16.o" "C object lib/crc/crc16.o"
	check_absent "$OUT/lib/crc/crc-ccitt.o" "C object lib/crc/crc-ccitt.o"
	for o in crc16_rust.o crc_ccitt_rust.o crc16_export.o crc_ccitt_export.o; do
		if [ -e "$OUT/lib/crc/$o" ]; then
			pass "Rust object lib/crc/$o is present"
		else
			bad "Rust object lib/crc/$o is missing"
		fi
	done
else
	for o in crc16.o crc-ccitt.o; do
		if [ -e "$OUT/lib/crc/$o" ]; then
			pass "C object lib/crc/$o is present (reference)"
		else
			bad "C object lib/crc/$o is missing from the reference build"
		fi
	done
	for o in crc16_rust.o crc_ccitt_rust.o; do
		if [ -e "$OUT/lib/crc/$o" ]; then
			bad "Rust object lib/crc/$o present in the reference build"
		else
			pass "Rust object lib/crc/$o absent from the reference build"
		fi
	done
fi

section "symbol definitions"
for sym in crc16 crc_ccitt crc_ccitt_table; do
	n=$(defcount "$sym")
	note "definitions of $sym in vmlinux: $n"
	if [ "$n" = "1" ]; then
		pass "$sym is defined exactly once"
	else
		bad "$sym must be defined exactly once (found $n)"
	fi
done

if [ "$VARIANT" = rust ]; then
	objnm "$OUT/lib/crc/crc16_rust.o" rust16.nmsize
	objnm "$OUT/lib/crc/crc_ccitt_rust.o" rustccitt.nmsize
	objnm "$OUT/lib/crc/crc16_export.o" exp16.nm
	objnm "$OUT/lib/crc/crc_ccitt_export.o" expccitt.nm

	if grep -qE '^[0-9a-f]+ [0-9a-f]+ T crc16$' "$TMP/rust16.nmsize"; then
		pass "crc16 is defined by lib/crc/crc16_rust.o"
		check_size crc16 "$(symsize "$TMP/rust16.nmsize" crc16)" "Rust object"
	else
		bad "crc16 is not defined by lib/crc/crc16_rust.o"
	fi
	if grep -qE '^[0-9a-f]+ [0-9a-f]+ T crc_ccitt$' "$TMP/rustccitt.nmsize"; then
		pass "crc_ccitt is defined by lib/crc/crc_ccitt_rust.o"
		check_size crc_ccitt "$(symsize "$TMP/rustccitt.nmsize" crc_ccitt)" "Rust object"
	else
		bad "crc_ccitt is not defined by lib/crc/crc_ccitt_rust.o"
	fi
	if grep -qE '^[0-9a-f]+ [0-9a-f]+ [DdRr] crc_ccitt_table$' "$TMP/rustccitt.nmsize"; then
		pass "crc_ccitt_table is defined by lib/crc/crc_ccitt_rust.o"
		check_size crc_ccitt_table "$(symsize "$TMP/rustccitt.nmsize" crc_ccitt_table)" "Rust object"
	else
		bad "crc_ccitt_table is not defined by lib/crc/crc_ccitt_rust.o"
	fi

	for pair in "exp16.nm crc16" "expccitt.nm crc_ccitt" "expccitt.nm crc_ccitt_table"; do
		set -- $pair
		if grep -qE "^ +U $2\$" "$TMP/$1"; then
			pass "export shim for $2 only references it (no definition)"
		else
			bad "export shim for $2 does not reference it"
		fi
	done
else
	objnm "$OUT/lib/crc/crc16.o" ref16.nmsize
	objnm "$OUT/lib/crc/crc-ccitt.o" refccitt.nmsize
	check_size crc16 "$(symsize "$TMP/ref16.nmsize" crc16)" "C object"
	check_size crc_ccitt "$(symsize "$TMP/refccitt.nmsize" crc_ccitt)" "C object"
	check_size crc_ccitt_table "$(symsize "$TMP/refccitt.nmsize" crc_ccitt_table)" "C object"
fi

section "debug-info provenance"
addr=$(awk '$3=="crc16" && $2 ~ /[Tt]/{print $1; exit}' "$TMP/vmlinux.nm")
if [ -z "$addr" ]; then
	bad "could not locate crc16 in vmlinux"
elif ! grep -q '^CONFIG_DEBUG_INFO=y' "$OUT/.config"; then
	skip "no DWARF in this configuration (CONFIG_DEBUG_INFO_NONE); provenance is established by object ownership and symbol sizes above"
else
	note "crc16 address: 0x$addr"
	src=$(addr2line -e "$VMLINUX" -f -i "0x$addr" 2>/dev/null | tr '\n' ' ')
	note "origin:  $src"
	case "$src" in
	*'??:?'*) skip "addr2line found no line info for 0x$addr" ;;
	*crc16_rust.rs*) pass "crc16 maps to Rust source (crc16_rust.rs)" ;;
	*crc16.c*) pass "crc16 maps to C source (crc16.c)" ;;
	*) bad "crc16 does not map to an expected source: $src" ;;
	esac
fi

section "export / ksymtab"
for sym in crc16 crc_ccitt crc_ccitt_table; do
	check_ksymtab "$sym"
done
check_symvers crc16 EXPORT_SYMBOL
check_symvers crc_ccitt EXPORT_SYMBOL
check_symvers crc_ccitt_table EXPORT_SYMBOL

section "disassembly fingerprint"
if command -v llvm-objdump >/dev/null; then
	llvm-objdump -d --no-show-raw-insn "$VMLINUX" 2>/dev/null |
		awk '/<crc16>:/{p=1} p{print; n++} n>6{exit}' |
		sed 's/^/      /' >>"$LOG"
	llvm-objdump -d --no-show-raw-insn "$VMLINUX" 2>/dev/null |
		awk '/<crc_ccitt>:/{p=1} p{print; n++} n>6{exit}' |
		sed 's/^/      /' >>"$LOG"
	pass "disassembly fingerprints captured above"
else
	skip "llvm-objdump unavailable"
fi

say ""
if [ "$fail" -ne 0 ]; then
	say "RESULT: FAIL"
	exit 1
fi
say "RESULT: PASS"

// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: ksymtab entry for the Rust CRC-16 implementation.
//
// This file contains NO algorithm and NO fallback implementation. It exists
// only to emit __ksymtab_crc16 with the same visibility as the upstream
// EXPORT_SYMBOL(crc16), so that modules (including non-GPL modules) keep
// resolving the symbol exactly as before.
//
// The kernel's Rust export machinery (rust/exports.c) exports symbols with
// EXPORT_SYMBOL_RUST_GPL for the `kernel` crate only; using it here would
// silently change the ABI to GPL-only. See docs/ferret/DECISIONS.md D-004.
//
// The symbol itself is defined in crc16_rust.rs: if that object is missing,
// this file fails to link.

#include <linux/crc16.h>
#include <linux/export.h>

EXPORT_SYMBOL(crc16);

// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: ksymtab entries for the Rust CRC-CCITT implementation.
//
// This file contains NO algorithm and NO fallback implementation. It exists
// only to emit __ksymtab_crc_ccitt and __ksymtab_crc_ccitt_table with the same
// visibility as the upstream EXPORT_SYMBOL()s, so that modules keep resolving
// both symbols exactly as before (including the exported data table used by the
// inline crc_ccitt_byte() in <linux/crc-ccitt.h>).
//
// The symbols themselves are defined in crc_ccitt_rust.rs: if that object is
// missing, this file fails to link.

#include <linux/crc-ccitt.h>
#include <linux/export.h>

EXPORT_SYMBOL(crc_ccitt_table);
EXPORT_SYMBOL(crc_ccitt);

// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: ksymtab entries for the Rust kernel bitmap.
//
// This file contains NO algorithm and NO fallback implementation. It exists
// only to emit the __ksymtab_* entries with the same visibility as the
// upstream EXPORT_SYMBOL() calls in lib/bitmap.c, so that modules keep
// resolving the symbols exactly as before.
//
// The kernel's Rust export machinery (rust/exports.c) exports symbols with
// EXPORT_SYMBOL_RUST_GPL for the `kernel` crate only; using it here would
// silently change the ABI of the non-GPL symbols to GPL-only. See
// docs/ferret/DECISIONS.md D-004.
//
// The symbols themselves are defined in bitmap_rust.rs: if that object is
// missing, this file fails to link.

#include <linux/bitmap.h>
#include <linux/export.h>

EXPORT_SYMBOL(__bitmap_and);
EXPORT_SYMBOL(__bitmap_andnot);
EXPORT_SYMBOL(__bitmap_clear);
EXPORT_SYMBOL(__bitmap_complement);
EXPORT_SYMBOL(__bitmap_equal);
EXPORT_SYMBOL(__bitmap_intersects);
EXPORT_SYMBOL(__bitmap_or);
EXPORT_SYMBOL(__bitmap_replace);
EXPORT_SYMBOL(__bitmap_set);
EXPORT_SYMBOL(__bitmap_shift_left);
EXPORT_SYMBOL(__bitmap_shift_right);
EXPORT_SYMBOL(__bitmap_subset);
EXPORT_SYMBOL(__bitmap_weight);
EXPORT_SYMBOL(__bitmap_weight_and);
EXPORT_SYMBOL(__bitmap_weight_andnot);
EXPORT_SYMBOL(__bitmap_xor);
EXPORT_SYMBOL(bitmap_alloc);
EXPORT_SYMBOL(bitmap_alloc_node);
EXPORT_SYMBOL(bitmap_bitremap);
EXPORT_SYMBOL(bitmap_cut);
EXPORT_SYMBOL(bitmap_find_next_zero_area_off);
EXPORT_SYMBOL(bitmap_free);
EXPORT_SYMBOL(bitmap_from_arr32);
EXPORT_SYMBOL(bitmap_remap);
EXPORT_SYMBOL(bitmap_to_arr32);
EXPORT_SYMBOL(bitmap_zalloc);
EXPORT_SYMBOL(bitmap_zalloc_node);

/* These two are EXPORT_SYMBOL_GPL() in the C original. */
EXPORT_SYMBOL_GPL(devm_bitmap_alloc);
EXPORT_SYMBOL_GPL(devm_bitmap_zalloc);

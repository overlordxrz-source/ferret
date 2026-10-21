// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: ksymtab entries for the Rust reference counting helpers.
//
// This file contains NO algorithm and NO fallback implementation. It exists
// only to emit the __ksymtab_* entries with the same visibility as the
// upstream EXPORT_SYMBOL() calls in lib/refcount.c, so that modules keep
// resolving the symbols exactly as before.
//
// The kernel's Rust export machinery (rust/exports.c) exports symbols with
// EXPORT_SYMBOL_RUST_GPL for the `kernel` crate only; using it here would
// silently change the ABI of the non-GPL symbols to GPL-only. See
// docs/ferret/DECISIONS.md D-004.
//
// The symbols themselves are defined in refcount_rust.rs: if that object is
// missing, this file fails to link.

#include <linux/refcount.h>
#include <linux/export.h>

EXPORT_SYMBOL(refcount_warn_saturate);
EXPORT_SYMBOL(refcount_dec_if_one);
EXPORT_SYMBOL(refcount_dec_not_one);
EXPORT_SYMBOL(refcount_dec_and_mutex_lock);
EXPORT_SYMBOL(refcount_dec_and_lock);
EXPORT_SYMBOL(refcount_dec_and_lock_irqsave);

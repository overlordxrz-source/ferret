// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: ksymtab entries for the Rust generic kernel FIFO.
//
// This file contains NO algorithm and NO fallback implementation. It exists
// only to emit the __ksymtab_* entries with the same visibility as the
// upstream EXPORT_SYMBOL() calls in lib/kfifo.c, so that modules (including
// non-GPL modules) keep resolving the symbols exactly as before.
//
// The kernel's Rust export machinery (rust/exports.c) exports symbols with
// EXPORT_SYMBOL_RUST_GPL for the `kernel` crate only; using it here would
// silently change the ABI to GPL-only. See docs/ferret/DECISIONS.md D-004.
//
// The symbols themselves are defined in kfifo_rust.rs: if that object is
// missing, this file fails to link.

#include <linux/export.h>
#include <linux/kfifo.h>

EXPORT_SYMBOL(__kfifo_alloc);
EXPORT_SYMBOL(__kfifo_free);
EXPORT_SYMBOL(__kfifo_init);
EXPORT_SYMBOL(__kfifo_in);
EXPORT_SYMBOL(__kfifo_out);
EXPORT_SYMBOL(__kfifo_out_peek);
EXPORT_SYMBOL(__kfifo_out_linear);
EXPORT_SYMBOL(__kfifo_from_user);
EXPORT_SYMBOL(__kfifo_to_user);
EXPORT_SYMBOL(__kfifo_dma_in_prepare);
EXPORT_SYMBOL(__kfifo_dma_out_prepare);
EXPORT_SYMBOL(__kfifo_max_r);
EXPORT_SYMBOL(__kfifo_len_r);
EXPORT_SYMBOL(__kfifo_in_r);
EXPORT_SYMBOL(__kfifo_out_r);
EXPORT_SYMBOL(__kfifo_out_peek_r);
EXPORT_SYMBOL(__kfifo_out_linear_r);
EXPORT_SYMBOL(__kfifo_skip_r);
EXPORT_SYMBOL(__kfifo_from_user_r);
EXPORT_SYMBOL(__kfifo_to_user_r);
EXPORT_SYMBOL(__kfifo_dma_in_prepare_r);
EXPORT_SYMBOL(__kfifo_dma_in_finish_r);
EXPORT_SYMBOL(__kfifo_dma_out_prepare_r);

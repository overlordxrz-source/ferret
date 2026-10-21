// SPDX-License-Identifier: GPL-2.0

#include <linux/slab.h>

void * __must_check __realloc_size(2)
rust_helper_krealloc_node_align(const void *objp, size_t new_size, unsigned long align,
				gfp_t flags, int node)
{
	return krealloc_node_align(objp, new_size, align, flags, node);
}

void * __must_check __realloc_size(2)
rust_helper_kvrealloc_node_align(const void *p, size_t size, unsigned long align,
				 gfp_t flags, int node)
{
	return kvrealloc_node_align(p, size, align, flags, node);
}

/*
 * Ferret: expose kmalloc_array() to Rust.
 *
 * The generic kernel FIFO (lib/kfifo.c) allocates its backing buffer with
 * kmalloc_array(esize, size, gfp_mask), where the allocation flags are a
 * run-time value. The kernel crate's `Flags` type only exposes the fixed
 * flag sets, so the allocation cannot be expressed with the `kernel` crate
 * alone. This helper performs the allocation and nothing else; the FIFO
 * logic stays in Rust (lib/kfifo_rust.rs).
 */
void * __must_check rust_helper_kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	return kmalloc_array(n, size, flags);
}

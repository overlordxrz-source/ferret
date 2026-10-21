// SPDX-License-Identifier: GPL-2.0

#include <linux/dma-direction.h>

dma_addr_t rust_helper_sg_dma_address(struct scatterlist *sg)
{
	return sg_dma_address(sg);
}

unsigned int rust_helper_sg_dma_len(struct scatterlist *sg)
{
	return sg_dma_len(sg);
}

struct scatterlist *rust_helper_sg_next(struct scatterlist *sg)
{
	return sg_next(sg);
}

void rust_helper_dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
				   enum dma_data_direction dir, unsigned long attrs)
{
	return dma_unmap_sgtable(dev, sgt, dir, attrs);
}

/*
 * Ferret: expose sg_set_buf() to Rust.
 *
 * The generic kernel FIFO (lib/kfifo.c) fills a caller-provided scatterlist
 * with sg_set_buf() plus a DMA address/length. sg_set_buf() is a static
 * inline in include/linux/scatterlist.h and has no binding, so it is
 * exposed here. The FIFO logic itself stays in Rust.
 */
void rust_helper_sg_set_buf(struct scatterlist *sg, const void *buf,
			    unsigned int buflen)
{
	sg_set_buf(sg, buf, buflen);
}

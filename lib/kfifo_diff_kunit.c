// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: differential KUnit test for the Rust generic kernel FIFO.
//
// Compares the Rust implementation (lib/kfifo_rust.rs) with the original C
// implementation, compiled a second time under test-only symbol names
// (lib/kfifo_ref.o, built from the unmodified lib/kfifo.c with every exported
// symbol renamed; see lib/Makefile).
//
// The two implementations are driven through identical operation sequences on
// separate FIFOs, and the results, the FIFO state and the buffer contents are
// compared after every step.
//
// This suite is only built for CONFIG_KFIFO_DIFF_KUNIT_TEST=y and is never
// part of a production kernel.

#include <kunit/test.h>

#include <linux/kfifo.h>
#include <linux/prandom.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

/* The C reference, compiled from the unmodified lib/kfifo.c. */
extern int kfifo_c_ref_alloc(struct __kfifo *fifo, unsigned int size,
			      size_t esize, gfp_t gfp_mask);
extern void kfifo_c_ref_free(struct __kfifo *fifo);
extern int kfifo_c_ref_init(struct __kfifo *fifo, void *buffer,
			     unsigned int size, size_t esize);
extern unsigned int kfifo_c_ref_in(struct __kfifo *fifo, const void *buf,
				   unsigned int len);
extern unsigned int kfifo_c_ref_out(struct __kfifo *fifo, void *buf,
				    unsigned int len);
extern unsigned int kfifo_c_ref_out_peek(struct __kfifo *fifo, void *buf,
					 unsigned int len);
extern unsigned int kfifo_c_ref_out_linear(struct __kfifo *fifo,
					   unsigned int *tail, unsigned int n);
extern unsigned int kfifo_c_ref_max_r(unsigned int len, size_t recsize);
extern unsigned int kfifo_c_ref_len_r(struct __kfifo *fifo, size_t recsize);
extern unsigned int kfifo_c_ref_in_r(struct __kfifo *fifo, const void *buf,
				     unsigned int len, size_t recsize);
extern unsigned int kfifo_c_ref_out_r(struct __kfifo *fifo, void *buf,
				      unsigned int len, size_t recsize);
extern unsigned int kfifo_c_ref_out_peek_r(struct __kfifo *fifo, void *buf,
					   unsigned int len, size_t recsize);
extern unsigned int kfifo_c_ref_out_linear_r(struct __kfifo *fifo,
					     unsigned int *tail, unsigned int n,
					     size_t recsize);
extern void kfifo_c_ref_skip_r(struct __kfifo *fifo, size_t recsize);
extern unsigned int kfifo_c_ref_dma_in_prepare(struct __kfifo *fifo,
						struct scatterlist *sgl,
						int nents, unsigned int len,
						dma_addr_t dma);
extern unsigned int kfifo_c_ref_dma_out_prepare(struct __kfifo *fifo,
						 struct scatterlist *sgl,
						 int nents, unsigned int len,
						 dma_addr_t dma);

#define KFIFO_DIFF_BUF_SIZE	256

/* A pair of FIFOs: one driven through the Rust API, one through the C API. */
struct kfifo_diff_pair {
	struct __kfifo rust;
	struct __kfifo c;
	u8 rust_buf[KFIFO_DIFF_BUF_SIZE];
	u8 c_buf[KFIFO_DIFF_BUF_SIZE];
};

static void kfifo_diff_pair_init(struct kfifo_diff_pair *p, unsigned int size,
				 size_t esize)
{
	memset(&p->rust, 0, sizeof(p->rust));
	memset(&p->c, 0, sizeof(p->c));
	memset(p->rust_buf, 0, sizeof(p->rust_buf));
	memset(p->c_buf, 0, sizeof(p->c_buf));

	__kfifo_init(&p->rust, p->rust_buf, size, esize);
	kfifo_c_ref_init(&p->c, p->c_buf, size, esize);
}

/* Compare the state of both FIFOs and their buffers. */
static void kfifo_diff_pair_compare(struct kunit *test,
				    struct kfifo_diff_pair *p)
{
	KUNIT_EXPECT_EQ(test, p->rust.in, p->c.in);
	KUNIT_EXPECT_EQ(test, p->rust.out, p->c.out);
	KUNIT_EXPECT_EQ(test, p->rust.mask, p->c.mask);
	KUNIT_EXPECT_EQ(test, p->rust.esize, p->c.esize);
	KUNIT_EXPECT_EQ(test, memcmp(p->rust_buf, p->c_buf, KFIFO_DIFF_BUF_SIZE), 0);
}

/* The initialisation paths must agree, including the error cases. */
static void kfifo_diff_init(struct kunit *test)
{
	static const unsigned int sizes[] = { 0, 1, 2, 3, 4, 7, 8, 16, 64, 255, 256 };
	static const size_t esizes[] = { 1, 2, 4 };
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (j = 0; j < ARRAY_SIZE(esizes); j++) {
			struct kfifo_diff_pair p;

			kfifo_diff_pair_init(&p, sizes[i], esizes[j]);
			kfifo_diff_pair_compare(test, &p);
		}
	}
}

/* Allocating a FIFO must agree, including the failure paths. */
static void kfifo_diff_alloc(struct kunit *test)
{
	static const unsigned int sizes[] = { 0, 1, 2, 3, 8, 64 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		struct kfifo_diff_pair p;
		int rust_ret, c_ret;

		memset(&p, 0, sizeof(p));

		rust_ret = __kfifo_alloc(&p.rust, sizes[i], 1, GFP_KERNEL);
		c_ret = kfifo_c_ref_alloc(&p.c, sizes[i], 1, GFP_KERNEL);

		KUNIT_EXPECT_EQ(test, rust_ret, c_ret);
		KUNIT_EXPECT_EQ(test, p.rust.in, p.c.in);
		KUNIT_EXPECT_EQ(test, p.rust.out, p.c.out);
		KUNIT_EXPECT_EQ(test, p.rust.mask, p.c.mask);
		KUNIT_EXPECT_EQ(test, p.rust.esize, p.c.esize);
		KUNIT_EXPECT_EQ(test, p.rust.data == NULL, p.c.data == NULL);

		if (!rust_ret) {
			__kfifo_free(&p.rust);
			kfifo_c_ref_free(&p.c);
		}
	}
}

/* in/out/peek must agree over a long randomised sequence. */
static void kfifo_diff_random_sequences(struct kunit *test)
{
	struct rnd_state rng;
	struct kfifo_diff_pair p;
	u8 buf[128];
	unsigned int i;

	prandom_seed_state(&rng, 0x666572726574); /* "ferret" */

	kfifo_diff_pair_init(&p, 64, 1);

	for (i = 0; i < 2000; i++) {
		unsigned int op = prandom_u32_state(&rng) % 4;
		unsigned int len = prandom_u32_state(&rng) % 65;
		unsigned int rust_ret, c_ret;
		u8 rust_out[128] = { 0 };
		u8 c_out[128] = { 0 };

		switch (op) {
		case 0:
			prandom_bytes_state(&rng, buf, len);
			rust_ret = __kfifo_in(&p.rust, buf, len);
			c_ret = kfifo_c_ref_in(&p.c, buf, len);
			break;
		case 1:
			rust_ret = __kfifo_out(&p.rust, rust_out, len);
			c_ret = kfifo_c_ref_out(&p.c, c_out, len);
			break;
		case 2:
			rust_ret = __kfifo_out_peek(&p.rust, rust_out, len);
			c_ret = kfifo_c_ref_out_peek(&p.c, c_out, len);
			break;
		default: {
			unsigned int rust_tail = 0, c_tail = 0;

			rust_ret = __kfifo_out_linear(&p.rust, &rust_tail, len);
			c_ret = kfifo_c_ref_out_linear(&p.c, &c_tail, len);
			KUNIT_EXPECT_EQ(test, rust_tail, c_tail);
			break;
		}
		}

		KUNIT_EXPECT_EQ(test, rust_ret, c_ret);
		KUNIT_EXPECT_EQ(test, memcmp(rust_out, c_out, len), 0);
		kfifo_diff_pair_compare(test, &p);
	}
}

/* The wrap-around case: fill, drain and refill until the indices wrap. */
static void kfifo_diff_wrap_around(struct kunit *test)
{
	struct kfifo_diff_pair p;
	u8 buf[32];
	u8 rust_out[32] = { 0 };
	u8 c_out[32] = { 0 };
	unsigned int round;

	kfifo_diff_pair_init(&p, 32, 1);

	for (round = 0; round < 16; round++) {
		unsigned int i;

		for (i = 0; i < sizeof(buf); i++)
			buf[i] = (u8)(round * 7 + i);

		KUNIT_ASSERT_EQ(test, __kfifo_in(&p.rust, buf, sizeof(buf)),
				kfifo_c_ref_in(&p.c, buf, sizeof(buf)));
		KUNIT_ASSERT_EQ(test, __kfifo_out(&p.rust, rust_out, sizeof(rust_out)),
				kfifo_c_ref_out(&p.c, c_out, sizeof(c_out)));
		KUNIT_EXPECT_EQ(test, memcmp(rust_out, c_out, sizeof(rust_out)), 0);
		KUNIT_EXPECT_EQ(test, memcmp(rust_out, buf, sizeof(buf)), 0);
		kfifo_diff_pair_compare(test, &p);
	}
}

/* The record ("_r") API must agree, including the length field handling. */
static void kfifo_diff_records(struct kunit *test)
{
	static const size_t recsizes[] = { 1, 2 };
	unsigned int j;

	for (j = 0; j < ARRAY_SIZE(recsizes); j++) {
		struct kfifo_diff_pair p;
		size_t recsize = recsizes[j];
		u8 buf[16];
		u8 rust_out[16] = { 0 };
		u8 c_out[16] = { 0 };
		unsigned int i;

		kfifo_diff_pair_init(&p, 64, 1);

		for (i = 0; i < 8; i++) {
			unsigned int len = (i % 8) + 1;

			memset(buf, (int)(i + 1), sizeof(buf));

			KUNIT_ASSERT_EQ(test,
					__kfifo_in_r(&p.rust, buf, len, recsize),
					kfifo_c_ref_in_r(&p.c, buf, len, recsize));
			KUNIT_EXPECT_EQ(test,
					__kfifo_len_r(&p.rust, recsize),
					kfifo_c_ref_len_r(&p.c, recsize));

			/* Peek must not consume the record. */
			KUNIT_ASSERT_EQ(test,
					__kfifo_out_peek_r(&p.rust, rust_out, len, recsize),
					kfifo_c_ref_out_peek_r(&p.c, c_out, len, recsize));
			KUNIT_EXPECT_EQ(test, memcmp(rust_out, c_out, len), 0);

			/* The linear offset must agree as well. */
			{
				unsigned int rust_tail = 0, c_tail = 0;

				KUNIT_ASSERT_EQ(test,
						__kfifo_out_linear_r(&p.rust, &rust_tail, len, recsize),
						kfifo_c_ref_out_linear_r(&p.c, &c_tail, len, recsize));
				KUNIT_EXPECT_EQ(test, rust_tail, c_tail);
			}

			kfifo_diff_pair_compare(test, &p);

			/* Consume the record. */
			memset(rust_out, 0, sizeof(rust_out));
			memset(c_out, 0, sizeof(c_out));
			KUNIT_ASSERT_EQ(test,
					__kfifo_out_r(&p.rust, rust_out, len, recsize),
					kfifo_c_ref_out_r(&p.c, c_out, len, recsize));
			KUNIT_EXPECT_EQ(test, memcmp(rust_out, c_out, len), 0);
			kfifo_diff_pair_compare(test, &p);
		}

		/* And skipping a record must agree. */
		KUNIT_ASSERT_EQ(test, __kfifo_in_r(&p.rust, buf, 4, recsize),
				kfifo_c_ref_in_r(&p.c, buf, 4, recsize));
		__kfifo_skip_r(&p.rust, recsize);
		kfifo_c_ref_skip_r(&p.c, recsize);
		kfifo_diff_pair_compare(test, &p);
	}
}

/* __kfifo_max_r() must agree for every record size. */
static void kfifo_diff_max_r(struct kunit *test)
{
	static const unsigned int lens[] = { 0, 1, 10, 255, 256, 1000, 65535, 65536 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lens); i++) {
		KUNIT_EXPECT_EQ(test, __kfifo_max_r(lens[i], 1),
				kfifo_c_ref_max_r(lens[i], 1));
		KUNIT_EXPECT_EQ(test, __kfifo_max_r(lens[i], 2),
				kfifo_c_ref_max_r(lens[i], 2));
	}
}

/* The scatterlist setup must agree, entry by entry. */
static void kfifo_diff_dma_prepare(struct kunit *test)
{
	struct kfifo_diff_pair p;
	struct scatterlist rust_sgl[4];
	struct scatterlist c_sgl[4];
	unsigned int rust_n, c_n;
	unsigned int i;

	kfifo_diff_pair_init(&p, 32, 1);

	memset(rust_sgl, 0, sizeof(rust_sgl));
	memset(c_sgl, 0, sizeof(c_sgl));

	__kfifo_in(&p.rust, p.rust_buf, 16);
	kfifo_c_ref_in(&p.c, p.c_buf, 16);

	rust_n = __kfifo_dma_out_prepare(&p.rust, rust_sgl, ARRAY_SIZE(rust_sgl),
					 16, 0);
	c_n = kfifo_c_ref_dma_out_prepare(&p.c, c_sgl, ARRAY_SIZE(c_sgl), 16, 0);

	KUNIT_EXPECT_EQ(test, rust_n, c_n);

	for (i = 0; i < rust_n && i < c_n; i++) {
		KUNIT_EXPECT_EQ(test, rust_sgl[i].length, c_sgl[i].length);
		KUNIT_EXPECT_EQ(test, rust_sgl[i].dma_address, c_sgl[i].dma_address);
		KUNIT_EXPECT_EQ(test, rust_sgl[i].dma_length, c_sgl[i].dma_length);
	}

	/* The same, with a DMA address. */
	memset(rust_sgl, 0, sizeof(rust_sgl));
	memset(c_sgl, 0, sizeof(c_sgl));

	rust_n = __kfifo_dma_in_prepare(&p.rust, rust_sgl, ARRAY_SIZE(rust_sgl),
					16, 0x1000);
	c_n = kfifo_c_ref_dma_in_prepare(&p.c, c_sgl, ARRAY_SIZE(c_sgl), 16, 0x1000);

	KUNIT_EXPECT_EQ(test, rust_n, c_n);

	for (i = 0; i < rust_n && i < c_n; i++) {
		KUNIT_EXPECT_EQ(test, rust_sgl[i].length, c_sgl[i].length);
		KUNIT_EXPECT_EQ(test, rust_sgl[i].dma_address, c_sgl[i].dma_address);
		KUNIT_EXPECT_EQ(test, rust_sgl[i].dma_length, c_sgl[i].dma_length);
	}
}

static struct kunit_case kfifo_diff_test_cases[] = {
	KUNIT_CASE(kfifo_diff_init),
	KUNIT_CASE(kfifo_diff_alloc),
	KUNIT_CASE(kfifo_diff_random_sequences),
	KUNIT_CASE(kfifo_diff_wrap_around),
	KUNIT_CASE(kfifo_diff_records),
	KUNIT_CASE(kfifo_diff_max_r),
	KUNIT_CASE(kfifo_diff_dma_prepare),
	{},
};

static struct kunit_suite kfifo_diff_test_module = {
	.name = "kfifo_diff",
	.test_cases = kfifo_diff_test_cases,
};

kunit_test_suites(&kfifo_diff_test_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ferret <ferret@example.invalid>");
MODULE_DESCRIPTION("Differential KUnit test for the Rust generic kernel FIFO");

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

#include <linux/errno.h>
#include <linux/kfifo.h>
#include <linux/prandom.h>
#include <linux/ktime.h>
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
extern int kfifo_c_ref_from_user(struct __kfifo *fifo, const void __user *from,
				  unsigned long len, unsigned int *copied);
extern int kfifo_c_ref_to_user(struct __kfifo *fifo, void __user *to,
				unsigned long len, unsigned int *copied);
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
	/* The return value of the Rust implementation. */
	int rust_ret;
	/*
	 * Set when the C reference has undefined behaviour for this input,
	 * so that it is not called at all.
	 */
	bool c_undefined;
};

/*
 * The C reference is the unmodified upstream code, and the upstream code has
 * undefined behaviour for some of the inputs this suite uses:
 *
 *   - the upstream `roundup_pow_of_two()` and `rounddown_pow_of_two()` shift
 *     by an out-of-range amount when their argument is zero
 *     (include/linux/log2.h): `1UL << fls_long(n - 1)` shifts by the word
 *     width for n == 0, and `1UL << (fls_long(n) - 1)` shifts by -1. UBSAN
 *     reports both;
 *   - the C `__kfifo_init()` divides by the element size and rounds the
 *     quotient down, so it calls `rounddown_pow_of_two(0)` whenever the
 *     quotient is zero, and it divides by zero when the element size is
 *     zero;
 *   - the C `__kfifo_alloc()` rounds the size up, so it calls
 *     `roundup_pow_of_two(0)` for a size of zero.
 *
 * The Rust implementation defines all of these cases, so the inputs that are
 * undefined in C are checked against the specification instead of against
 * the C reference: comparing against undefined behaviour compares noise.
 * The upstream KUnit suite never uses such an input (it always uses a
 * fixed size of 32 elements), which is why the undefined behaviour stayed
 * latent upstream. See issue #25.
 */
static bool kfifo_diff_c_undefined_init(unsigned int size, size_t esize)
{
	return esize == 0 || size < esize;
}

static bool kfifo_diff_c_undefined_alloc(unsigned int size)
{
	return size == 0;
}

static void kfifo_diff_pair_init(struct kfifo_diff_pair *p, unsigned int size,
				 size_t esize)
{
	memset(&p->rust, 0, sizeof(p->rust));
	memset(&p->c, 0, sizeof(p->c));
	memset(p->rust_buf, 0, sizeof(p->rust_buf));
	memset(p->c_buf, 0, sizeof(p->c_buf));

	p->rust_ret = __kfifo_init(&p->rust, p->rust_buf, size, esize);

	p->c_undefined = kfifo_diff_c_undefined_init(size, esize);
	if (p->c_undefined)
		return;

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

			if (p.c_undefined) {
				/*
				 * The C reference is undefined for this
				 * input, so the Rust implementation is
				 * checked against the specification: a
				 * quotient below two elements is an error.
				 */
				KUNIT_EXPECT_EQ(test, p.rust_ret, -EINVAL);
				KUNIT_EXPECT_EQ(test, p.rust.mask, 0u);
				continue;
			}

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

		if (kfifo_diff_c_undefined_alloc(sizes[i])) {
			/*
			 * The C reference is undefined for a size of zero
			 * (it rounds the size up with an out-of-range
			 * shift), so the Rust implementation is checked
			 * against the specification: a size below two
			 * elements is an error.
			 */
			KUNIT_EXPECT_EQ(test, rust_ret, -EINVAL);
			KUNIT_EXPECT_EQ(test, p.rust.mask, 0u);
			KUNIT_EXPECT_TRUE(test, p.rust.data == NULL);
			continue;
		}

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

/*
 * The user-copy paths must fail cleanly on a pointer that is not a userspace
 * address, in both implementations.
 *
 * CONFIG_HARDENED_USERCOPY changes the kernel's reaction to a copy from a
 * non-user address from "report the error" to "panic", so the test is only
 * meaningful with it disabled.
 */
#if !IS_ENABLED(CONFIG_HARDENED_USERCOPY)
static void kfifo_diff_user_copy_fault(struct kunit *test)
{
	struct kfifo_diff_pair p;
	unsigned int rust_copied = 0xdeadbeef, c_copied = 0xdeadbeef;
	int rust_ret, c_ret;

	kfifo_diff_pair_init(&p, 64, 1);

	__kfifo_in(&p.rust, p.rust_buf, 8);
	kfifo_c_ref_in(&p.c, p.c_buf, 8);

	/* A kernel address is not a valid userspace address. */
	rust_ret = __kfifo_from_user(&p.rust, p.rust_buf, 8, &rust_copied);
	c_ret = kfifo_c_ref_from_user(&p.c, p.c_buf, 8, &c_copied);

	KUNIT_EXPECT_EQ(test, rust_ret, c_ret);
	KUNIT_EXPECT_EQ(test, rust_ret, -EFAULT);
	KUNIT_EXPECT_EQ(test, rust_copied, c_copied);
	KUNIT_EXPECT_EQ(test, rust_copied, 0);
	kfifo_diff_pair_compare(test, &p);

	/* The same in the output direction. */
	rust_copied = 0xdeadbeef;
	c_copied = 0xdeadbeef;

	rust_ret = __kfifo_to_user(&p.rust, p.rust_buf, 8, &rust_copied);
	c_ret = kfifo_c_ref_to_user(&p.c, p.c_buf, 8, &c_copied);

	KUNIT_EXPECT_EQ(test, rust_ret, c_ret);
	KUNIT_EXPECT_EQ(test, rust_ret, -EFAULT);
	KUNIT_EXPECT_EQ(test, rust_copied, c_copied);
	KUNIT_EXPECT_EQ(test, rust_copied, 0);
	kfifo_diff_pair_compare(test, &p);
}
#else
static void kfifo_diff_user_copy_fault(struct kunit *test)
{
	/* CONFIG_HARDENED_USERCOPY panics on a copy from a non-user address, so
	 * the fault path cannot be exercised from the kernel. */
	KUNIT_SKIP(test, "CONFIG_HARDENED_USERCOPY is enabled");
}
#endif

/*
 * A DMA preparation that has to wrap around the end of the buffer must
 * produce the same two scatterlist entries in both implementations.
 */
static void kfifo_diff_dma_wrap(struct kunit *test)
{
	struct kfifo_diff_pair p;
	struct scatterlist rust_sgl[4];
	struct scatterlist c_sgl[4];
	unsigned int rust_n, c_n, i;

	kfifo_diff_pair_init(&p, 32, 1);

	/* Fill, drain it and refill, so that the read offset is close to the
	 * end of the buffer and the transfer has to be split in two. */
	__kfifo_in(&p.rust, p.rust_buf, 20);
	kfifo_c_ref_in(&p.c, p.c_buf, 20);
	__kfifo_out(&p.rust, p.rust_buf, 20);
	kfifo_c_ref_out(&p.c, p.c_buf, 20);
	__kfifo_in(&p.rust, p.rust_buf, 24);
	kfifo_c_ref_in(&p.c, p.c_buf, 24);

	memset(rust_sgl, 0, sizeof(rust_sgl));
	memset(c_sgl, 0, sizeof(c_sgl));

	rust_n = __kfifo_dma_out_prepare(&p.rust, rust_sgl, ARRAY_SIZE(rust_sgl),
					 24, 0);
	c_n = kfifo_c_ref_dma_out_prepare(&p.c, c_sgl, ARRAY_SIZE(c_sgl), 24, 0);

	KUNIT_EXPECT_EQ(test, rust_n, c_n);
	KUNIT_EXPECT_EQ(test, rust_n, 2);

	for (i = 0; i < rust_n && i < c_n; i++) {
		KUNIT_EXPECT_EQ(test, rust_sgl[i].length, c_sgl[i].length);
		KUNIT_EXPECT_EQ(test, rust_sgl[i].dma_address, c_sgl[i].dma_address);
		KUNIT_EXPECT_EQ(test, rust_sgl[i].dma_length, c_sgl[i].dma_length);
	}
}

#define KFIFO_BENCH_ITERS	20000
#define KFIFO_BENCH_LEN		64

/* Time a fixed in/out sequence on one implementation. */
static u64 kfifo_bench_run(bool rust)
{
	struct __kfifo fifo;
	u8 buf[KFIFO_BENCH_LEN];
	u64 start, end;
	int i;

	memset(&fifo, 0, sizeof(fifo));
	memset(buf, 0, sizeof(buf));

	if (rust)
		__kfifo_init(&fifo, buf, sizeof(buf), 1);
	else
		kfifo_c_ref_init(&fifo, buf, sizeof(buf), 1);

	start = ktime_get();
	for (i = 0; i < KFIFO_BENCH_ITERS; i++) {
		if (rust) {
			__kfifo_in(&fifo, buf, KFIFO_BENCH_LEN);
			__kfifo_out(&fifo, buf, KFIFO_BENCH_LEN);
		} else {
			kfifo_c_ref_in(&fifo, buf, KFIFO_BENCH_LEN);
			kfifo_c_ref_out(&fifo, buf, KFIFO_BENCH_LEN);
		}
	}
	end = ktime_get();

	return end - start;
}

/*
 * Throughput comparison of the two implementations.
 *
 * The absolute numbers are meaningless here: the test kernel runs under QEMU
 * TCG, an instruction-level emulator. The ratio between the two
 * implementations, measured in the same run on the same machine, is the only
 * meaningful part, and even that is noisy.
 */
static void kfifo_diff_benchmark(struct kunit *test)
{
	u64 rust_best = U64_MAX, c_best = U64_MAX;
	u64 rust_ns, c_ns;
	int round;

	/* Three rounds each, keeping the fastest: the emulation and the host
	 * are noisy, and the best run is the least disturbed one. */
	for (round = 0; round < 3; round++) {
		rust_ns = kfifo_bench_run(true);
		c_ns = kfifo_bench_run(false);
		if (rust_ns < rust_best)
			rust_best = rust_ns;
		if (c_ns < c_best)
			c_best = c_ns;
	}

	pr_info("kfifo benchmark: %d iterations of in(%d)+out(%d), best of 3\n",
		KFIFO_BENCH_ITERS, KFIFO_BENCH_LEN, KFIFO_BENCH_LEN);
	pr_info("kfifo benchmark: rust %llu ns, c %llu ns\n", rust_best, c_best);
	pr_info("kfifo benchmark: rust/c ratio %llu per mille\n",
		div64_u64(c_best ? rust_best * 1000 : 0, c_best ? c_best : 1));
}

static struct kunit_case kfifo_diff_test_cases[] = {
	KUNIT_CASE(kfifo_diff_init),
	KUNIT_CASE(kfifo_diff_alloc),
	KUNIT_CASE(kfifo_diff_random_sequences),
	KUNIT_CASE(kfifo_diff_wrap_around),
	KUNIT_CASE(kfifo_diff_records),
	KUNIT_CASE(kfifo_diff_max_r),
	KUNIT_CASE(kfifo_diff_dma_prepare),
	KUNIT_CASE(kfifo_diff_dma_wrap),
	KUNIT_CASE(kfifo_diff_user_copy_fault),
	KUNIT_CASE(kfifo_diff_benchmark),
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

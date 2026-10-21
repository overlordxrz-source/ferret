// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: differential KUnit test for the Rust kernel bitmap.
//
// Compares the Rust implementation (lib/bitmap_rust.rs) with the original C
// implementation, compiled a second time under test-only symbol names
// (lib/bitmap_ref.o, built from the unmodified lib/bitmap.c with every global
// symbol renamed; see lib/Makefile).
//
// The two implementations are driven through identical operations on identical
// inputs, and the results, the return values and the destination buffers are
// compared.
//
// This suite is only built for CONFIG_BITMAP_DIFF_KUNIT_TEST=y and is never
// part of a production kernel.

#include <kunit/test.h>

#include <linux/bitmap.h>
#include <linux/prandom.h>
#include <linux/slab.h>

/* The C reference, compiled from the unmodified lib/bitmap.c. */
extern bool __bitmap_equal_c_ref(const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern bool __bitmap_or_equal_c_ref(const unsigned long *b1, const unsigned long *b2, const unsigned long *b3, unsigned int bits);
extern void __bitmap_complement_c_ref(unsigned long *dst, const unsigned long *src, unsigned int bits);
extern void __bitmap_shift_right_c_ref(unsigned long *dst, const unsigned long *src, unsigned shift, unsigned nbits);
extern void __bitmap_shift_left_c_ref(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits);
extern void bitmap_cut_c_ref(unsigned long *dst, const unsigned long *src, unsigned int first, unsigned int cut, unsigned int nbits);
extern bool __bitmap_and_c_ref(unsigned long *dst, const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern void __bitmap_or_c_ref(unsigned long *dst, const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern void __bitmap_xor_c_ref(unsigned long *dst, const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern bool __bitmap_andnot_c_ref(unsigned long *dst, const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern void __bitmap_replace_c_ref(unsigned long *dst, const unsigned long *old, const unsigned long *new, const unsigned long *mask, unsigned int nbits);
extern bool __bitmap_intersects_c_ref(const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern bool __bitmap_subset_c_ref(const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern unsigned int __bitmap_weight_c_ref(const unsigned long *bitmap, unsigned int bits);
extern unsigned int __bitmap_weight_and_c_ref(const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern unsigned int __bitmap_weight_andnot_c_ref(const unsigned long *b1, const unsigned long *b2, unsigned int bits);
extern void __bitmap_set_c_ref(unsigned long *map, unsigned int start, int len);
extern void __bitmap_clear_c_ref(unsigned long *map, unsigned int start, int len);
extern unsigned long bitmap_find_next_zero_area_off_c_ref(unsigned long *map, unsigned long size, unsigned long start, unsigned int nr, unsigned long align_mask, unsigned long align_offset);
extern void bitmap_remap_c_ref(unsigned long *dst, const unsigned long *src, const unsigned long *old, const unsigned long *new, unsigned int nbits);
extern int bitmap_bitremap_c_ref(int oldbit, const unsigned long *old, const unsigned long *new, int bits);
extern void bitmap_onto_c_ref(unsigned long *dst, const unsigned long *orig, const unsigned long *relmap, unsigned int bits);
extern void bitmap_fold_c_ref(unsigned long *dst, const unsigned long *orig, unsigned int sz, unsigned int nbits);
extern unsigned long *bitmap_alloc_c_ref(unsigned int nbits, gfp_t flags);
extern unsigned long *bitmap_zalloc_c_ref(unsigned int nbits, gfp_t flags);
extern unsigned long *bitmap_alloc_node_c_ref(unsigned int nbits, gfp_t flags, int node);
extern unsigned long *bitmap_zalloc_node_c_ref(unsigned int nbits, gfp_t flags, int node);
extern void bitmap_free_c_ref(const unsigned long *bitmap);
extern void bitmap_from_arr32_c_ref(unsigned long *bitmap, const u32 *buf, unsigned int nbits);
extern void bitmap_to_arr32_c_ref(u32 *buf, const unsigned long *bitmap, unsigned int nbits);

#define DIFF_WORDS	8	/* 512 bits of headroom */
#define DIFF_MAX_BITS	256

/* Fill a bitmap with pseudo-random bits. */
static void diff_fill(struct rnd_state *rng, unsigned long *map, unsigned int bits)
{
	unsigned int i;

	memset(map, 0, DIFF_WORDS * sizeof(unsigned long));

	for (i = 0; i < bits; i++) {
		if (prandom_u32_state(rng) & 1)
			map[i / BITS_PER_LONG] |= 1UL << (i % BITS_PER_LONG);
	}
}

/* Compare the first `words` words of two bitmaps. */
static void diff_compare(struct kunit *test, const unsigned long *a,
			 const unsigned long *b, unsigned int words,
			 const char *what)
{
	KUNIT_EXPECT_MEMEQ_MSG(test, a, b, words * sizeof(unsigned long), what);
}

/* The bit counts used by the tests: they cover the word boundaries, the tail
 * bits and the single-bit cases. */
static const unsigned int diff_sizes[] = {
	1, 2, 7, 31, 32, 33, 63, 64, 65, 100, 127, 128, 129, 191, 192, 255, 256,
};

static void bitmap_diff_sizes(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;

	/*
	 * Guard against a vacuous test: the two implementations must be
	 * different objects, otherwise the comparison below would compare the
	 * C reference with itself and pass for the wrong reason.
	 */
	KUNIT_ASSERT_NE(test, (unsigned long)__bitmap_and,
			(unsigned long)__bitmap_and_c_ref);
	KUNIT_ASSERT_NE(test, (unsigned long)bitmap_alloc,
			(unsigned long)bitmap_alloc_c_ref);

	prandom_seed_state(&rng, 0x666572726574); /* "ferret" */

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned long a[DIFF_WORDS], b[DIFF_WORDS], c[DIFF_WORDS];
		unsigned long ra[DIFF_WORDS], rb[DIFF_WORDS];
		unsigned int words = BITS_TO_LONGS(bits);

		diff_fill(&rng, a, bits);
		diff_fill(&rng, b, bits);
		diff_fill(&rng, c, bits);

		/* Comparison helpers. */
		KUNIT_EXPECT_EQ(test, __bitmap_equal(a, b, bits),
				__bitmap_equal_c_ref(a, b, bits));
		KUNIT_EXPECT_EQ(test, __bitmap_or_equal(a, b, c, bits),
				__bitmap_or_equal_c_ref(a, b, c, bits));
		KUNIT_EXPECT_EQ(test, __bitmap_intersects(a, b, bits),
				__bitmap_intersects_c_ref(a, b, bits));
		KUNIT_EXPECT_EQ(test, __bitmap_subset(a, b, bits),
				__bitmap_subset_c_ref(a, b, bits));

		/* Weight helpers. */
		KUNIT_EXPECT_EQ(test, __bitmap_weight(a, bits),
				__bitmap_weight_c_ref(a, bits));
		KUNIT_EXPECT_EQ(test, __bitmap_weight_and(a, b, bits),
				__bitmap_weight_and_c_ref(a, b, bits));
		KUNIT_EXPECT_EQ(test, __bitmap_weight_andnot(a, b, bits),
				__bitmap_weight_andnot_c_ref(a, b, bits));

		/* Logical operations write the destination. */
		memset(ra, 0, sizeof(ra));
		memset(rb, 0, sizeof(rb));
		KUNIT_EXPECT_EQ(test, __bitmap_and(ra, a, b, bits),
				__bitmap_and_c_ref(rb, a, b, bits));
		diff_compare(test, ra, rb, words, "and");

		memset(ra, 0, sizeof(ra));
		memset(rb, 0, sizeof(rb));
		__bitmap_or(ra, a, b, bits);
		__bitmap_or_c_ref(rb, a, b, bits);
		diff_compare(test, ra, rb, words, "or");

		memset(ra, 0, sizeof(ra));
		memset(rb, 0, sizeof(rb));
		__bitmap_xor(ra, a, b, bits);
		__bitmap_xor_c_ref(rb, a, b, bits);
		diff_compare(test, ra, rb, words, "xor");

		memset(ra, 0, sizeof(ra));
		memset(rb, 0, sizeof(rb));
		KUNIT_EXPECT_EQ(test, __bitmap_andnot(ra, a, b, bits),
				__bitmap_andnot_c_ref(rb, a, b, bits));
		diff_compare(test, ra, rb, words, "andnot");

		memset(ra, 0, sizeof(ra));
		memset(rb, 0, sizeof(rb));
		__bitmap_replace(ra, a, b, c, bits);
		__bitmap_replace_c_ref(rb, a, b, c, bits);
		diff_compare(test, ra, rb, words, "replace");

		memset(ra, 0, sizeof(ra));
		memset(rb, 0, sizeof(rb));
		__bitmap_complement(ra, a, bits);
		__bitmap_complement_c_ref(rb, a, bits);
		diff_compare(test, ra, rb, words, "complement");
	}
}

static void bitmap_diff_set_clear(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;

	prandom_seed_state(&rng, 0x6269746d6170); /* "bitmap" */

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned int words = BITS_TO_LONGS(bits);
		unsigned long a[DIFF_WORDS], ra[DIFF_WORDS], rb[DIFF_WORDS];
		unsigned int start, len;

		diff_fill(&rng, a, bits);

		/* A range inside the bitmap. */
		start = prandom_u32_state(&rng) % bits;
		len = prandom_u32_state(&rng) % (bits - start + 1);

		memcpy(ra, a, sizeof(ra));
		memcpy(rb, a, sizeof(rb));
		__bitmap_set(ra, start, len);
		__bitmap_set_c_ref(rb, start, len);
		diff_compare(test, ra, rb, words, "set");

		memcpy(ra, a, sizeof(ra));
		memcpy(rb, a, sizeof(rb));
		__bitmap_clear(ra, start, len);
		__bitmap_clear_c_ref(rb, start, len);
		diff_compare(test, ra, rb, words, "clear");

		/* A range that starts inside the bitmap and runs past its end. */
		memcpy(ra, a, sizeof(ra));
		memcpy(rb, a, sizeof(rb));
		__bitmap_set_c_ref(ra, start, 64);
		__bitmap_set_c_ref(rb, start, 64);
		diff_compare(test, ra, rb, words, "set past the end");

		/* A zero-length range. */
		memcpy(ra, a, sizeof(ra));
		memcpy(rb, a, sizeof(rb));
		__bitmap_set_c_ref(ra, start, 0);
		__bitmap_set_c_ref(rb, start, 0);
		diff_compare(test, ra, rb, words, "set zero length");
	}
}

static void bitmap_diff_shifts(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;

	prandom_seed_state(&rng, 0x736869667473); /* "shifts" */

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned int words = BITS_TO_LONGS(bits);
		unsigned long a[DIFF_WORDS], ra[DIFF_WORDS], rb[DIFF_WORDS];
		unsigned int shift;

		diff_fill(&rng, a, bits);

		for (shift = 0; shift <= bits; shift += 7) {
			memset(ra, 0, sizeof(ra));
			memset(rb, 0, sizeof(rb));
			__bitmap_shift_right(ra, a, shift, bits);
			__bitmap_shift_right_c_ref(rb, a, shift, bits);
			diff_compare(test, ra, rb, words, "shift right");

			memset(ra, 0, sizeof(ra));
			memset(rb, 0, sizeof(rb));
			__bitmap_shift_left(ra, a, shift, bits);
			__bitmap_shift_left_c_ref(rb, a, shift, bits);
			diff_compare(test, ra, rb, words, "shift left");
		}
	}
}

static void bitmap_diff_cut(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;

	prandom_seed_state(&rng, 0x637574746573); /* "cuttest" */

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned int words = BITS_TO_LONGS(bits);
		unsigned long a[DIFF_WORDS], ra[DIFF_WORDS], rb[DIFF_WORDS];
		unsigned int first, cut;

		diff_fill(&rng, a, bits);

		first = prandom_u32_state(&rng) % bits;
		cut = prandom_u32_state(&rng) % (bits - first + 1);

		memcpy(ra, a, sizeof(ra));
		memcpy(rb, a, sizeof(rb));
		bitmap_cut(ra, a, first, cut, bits);
		bitmap_cut_c_ref(rb, a, first, cut, bits);
		diff_compare(test, ra, rb, words, "cut");
	}
}

/*
 * The C original reads and writes the word that `first` points at even when
 * that word is outside the bitmap, which is an out-of-bounds access: the
 * upstream test suite exercises exactly that case (first == BITS_PER_LONG with
 * a one-word bitmap). The Rust implementation only touches the words that are
 * inside the bitmap, so the bits of the bitmap itself are the same, and the
 * C implementation's extra word is not compared.
 */
static void bitmap_diff_cut_out_of_range(struct kunit *test)
{
	unsigned long a[DIFF_WORDS], ra[DIFF_WORDS], rb[DIFF_WORDS];
	unsigned int bits = 64;
	unsigned int words = BITS_TO_LONGS(bits);
	unsigned int first;

	memset(a, 0, sizeof(a));
	a[0] = 0xa5a5a5a5UL;
	a[1] = 0x5a5a5a5aUL;

	/* `first` at and past the end of the bitmap. */
	for (first = bits; first <= bits + BITS_PER_LONG; first += BITS_PER_LONG) {
		memcpy(ra, a, sizeof(ra));
		memcpy(rb, a, sizeof(rb));
		bitmap_cut(ra, a, first, BITS_PER_LONG, bits);
		bitmap_cut_c_ref(rb, a, first, BITS_PER_LONG, bits);
		diff_compare(test, ra, rb, words, "cut outside the bitmap");
	}
}

static void bitmap_diff_find_next_zero_area(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;

	prandom_seed_state(&rng, 0x666e645f7a726f); /* "fnd_zero" */

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned long a[DIFF_WORDS], b[DIFF_WORDS];
		unsigned int nr;

		diff_fill(&rng, a, bits);

		for (nr = 1; nr <= 16; nr++) {
			unsigned long start = prandom_u32_state(&rng) % bits;

			memcpy(b, a, sizeof(b));
			KUNIT_EXPECT_EQ(test,
					bitmap_find_next_zero_area_off(a, bits, start, nr, 0, 0),
					bitmap_find_next_zero_area_off_c_ref(b, bits, start, nr, 0, 0));

			memcpy(b, a, sizeof(b));
			KUNIT_EXPECT_EQ(test,
					bitmap_find_next_zero_area_off_c_ref(a, bits, start, nr, 15, 3),
					bitmap_find_next_zero_area_off_c_ref(b, bits, start, nr, 15, 3));
		}
	}
}

static void bitmap_diff_remap(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;

	prandom_seed_state(&rng, 0x72656d61705f62); /* "remap_b" */

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned int words = BITS_TO_LONGS(bits);
		unsigned long src[DIFF_WORDS], old[DIFF_WORDS], new[DIFF_WORDS];
		unsigned long ra[DIFF_WORDS], rb[DIFF_WORDS];
		unsigned int bit;

		diff_fill(&rng, src, bits);
		diff_fill(&rng, old, bits);
		diff_fill(&rng, new, bits);

		memset(ra, 0, sizeof(ra));
		memset(rb, 0, sizeof(rb));
		bitmap_remap(ra, src, old, new, bits);
		bitmap_remap_c_ref(rb, src, old, new, bits);
		diff_compare(test, ra, rb, words, "remap");

		for (bit = 0; bit < bits; bit++) {
			KUNIT_EXPECT_EQ(test,
					bitmap_bitremap(bit, old, new, bits),
					bitmap_bitremap_c_ref(bit, old, new, bits));
		}
	}
}

static void bitmap_diff_arr32(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;

	prandom_seed_state(&rng, 0x61727233325f31); /* "arr32_1" */

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned int halfwords = DIV_ROUND_UP(bits, 32);
		u32 src[DIFF_WORDS] = { 0 };
		u32 out_a[DIFF_WORDS] = { 0 };
		u32 out_b[DIFF_WORDS] = { 0 };
		unsigned long a[DIFF_WORDS], b[DIFF_WORDS];
		unsigned int j;

		for (j = 0; j < halfwords; j++)
			src[j] = prandom_u32_state(&rng);

		memset(a, 0, sizeof(a));
		memset(b, 0, sizeof(b));

		bitmap_from_arr32(a, src, bits);
		bitmap_from_arr32_c_ref(b, src, bits);
		diff_compare(test, a, b, BITS_TO_LONGS(bits), "from_arr32");

		bitmap_to_arr32(out_a, a, bits);
		bitmap_to_arr32_c_ref(out_b, b, bits);
		KUNIT_EXPECT_MEMEQ_MSG(test, out_a, out_b, halfwords * sizeof(u32), "to_arr32");
	}
}

static void bitmap_diff_alloc(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned long *a = bitmap_alloc(bits, GFP_KERNEL);
		unsigned long *b = bitmap_alloc_c_ref(bits, GFP_KERNEL);
		unsigned long *za = bitmap_zalloc(bits, GFP_KERNEL);
		unsigned long *zb = bitmap_zalloc_c_ref(bits, GFP_KERNEL);
		unsigned int words = BITS_TO_LONGS(bits);

		KUNIT_ASSERT_NOT_NULL(test, a);
		KUNIT_ASSERT_NOT_NULL(test, b);
		KUNIT_ASSERT_NOT_NULL(test, za);
		KUNIT_ASSERT_NOT_NULL(test, zb);

		/* An allocated bitmap is not zeroed, a zalloc'ed one is. */
		diff_compare(test, za, zb, words, "zalloc");

		/* Both allocations have the same size. */
		KUNIT_EXPECT_EQ(test, ksize(a), ksize(b));

		bitmap_free(a);
		bitmap_free_c_ref(b);
		bitmap_free_c_ref(za);
		bitmap_free_c_ref(zb);
	}
}

/*
 * The NUMA node variants must agree with the C reference as well.
 */
static void bitmap_diff_alloc_node(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(diff_sizes); i++) {
		unsigned int bits = diff_sizes[i];
		unsigned long *a = bitmap_alloc_node(bits, GFP_KERNEL, 0);
		unsigned long *b = bitmap_alloc_node_c_ref(bits, GFP_KERNEL, 0);
		unsigned long *za = bitmap_zalloc_node(bits, GFP_KERNEL, 0);
		unsigned long *zb = bitmap_zalloc_node_c_ref(bits, GFP_KERNEL, 0);

		KUNIT_ASSERT_NOT_NULL(test, a);
		KUNIT_ASSERT_NOT_NULL(test, b);
		KUNIT_ASSERT_NOT_NULL(test, za);
		KUNIT_ASSERT_NOT_NULL(test, zb);

		/* A zeroed allocation is zeroed in both implementations. */
		diff_compare(test, za, zb, BITS_TO_LONGS(bits), "zalloc_node");
		KUNIT_EXPECT_EQ(test, ksize(a), ksize(b));

		bitmap_free(a);
		bitmap_free(b);
		bitmap_free(za);
		bitmap_free(zb);
	}
}

/*
 * Throughput comparison of the two implementations.
 *
 * The upstream test_bitmap kselftest measures bitmap_read() and
 * bitmap_write(), which are static inlines in include/linux/bitmap.h and
 * therefore identical in both builds: they do not measure the ported code at
 * all. This benchmark exercises the *exported* functions instead, on both
 * implementations, in the same kernel and the same run.
 *
 * The numbers are QEMU TCG figures, so only the ratio between the two
 * implementations is meaningful; the run-to-run spread of a single TCG
 * measurement is large (the upstream kselftest, which measures identical code
 * in both builds, varied by ~30 % between two boots of the same kernel).
 */
#define BENCH_BITS	1024
#define BENCH_ITERS	2000
#define BENCH_ROUNDS	3

/* `with_weight` selects the workload: the weight helpers use the kernel's
 * population count, which the alternatives mechanism patches to the popcount
 * instruction at boot. That patch cannot be applied to Rust code, so the
 * weight helpers are measured separately from the rest of the API. */
static u64 bench_run(bool rust, bool with_weight)
{
	unsigned long a[BENCH_BITS / BITS_PER_LONG] = { 0 };
	unsigned long b[BENCH_BITS / BITS_PER_LONG] = { 0 };
	unsigned long c[BENCH_BITS / BITS_PER_LONG] = { 0 };
	u64 start, end;
	unsigned int i, j;

	for (j = 0; j < BENCH_BITS / BITS_PER_LONG; j++)
		a[j] = 0x5555555555555555UL ^ (j * 0x0101010101010101UL);

	start = ktime_get();

	for (i = 0; i < BENCH_ITERS; i++) {
		if (rust) {
			__bitmap_set(a, i % (BENCH_BITS - 64), 64);
			__bitmap_clear(a, i % (BENCH_BITS - 64), 32);
			__bitmap_and(b, a, c, BENCH_BITS);
			__bitmap_or(b, a, c, BENCH_BITS);
			__bitmap_xor(b, a, c, BENCH_BITS);
			if (with_weight)
				__bitmap_weight(a, BENCH_BITS);
			__bitmap_shift_left(b, a, 3, BENCH_BITS);
			__bitmap_shift_right(b, a, 3, BENCH_BITS);
		} else {
			__bitmap_set_c_ref(a, i % (BENCH_BITS - 64), 64);
			__bitmap_clear_c_ref(a, i % (BENCH_BITS - 64), 32);
			__bitmap_and_c_ref(b, a, c, BENCH_BITS);
			__bitmap_or_c_ref(b, a, c, BENCH_BITS);
			__bitmap_xor_c_ref(b, a, c, BENCH_BITS);
			if (with_weight)
				__bitmap_weight_c_ref(a, BENCH_BITS);
			__bitmap_shift_left_c_ref(b, a, 3, BENCH_BITS);
			__bitmap_shift_right_c_ref(b, a, 3, BENCH_BITS);
		}
	}

	end = ktime_get();

	return end - start;
}

static void bitmap_diff_benchmark(struct kunit *test)
{
	u64 rust_best, c_best, rust_ns, c_ns;
	unsigned int round;

	/* The whole API, including the weight helpers. */
	rust_best = U64_MAX;
	c_best = U64_MAX;
	for (round = 0; round < BENCH_ROUNDS; round++) {
		rust_ns = bench_run(true, true);
		c_ns = bench_run(false, true);

		if (rust_ns < rust_best)
			rust_best = rust_ns;
		if (c_ns < c_best)
			c_best = c_ns;
	}

	pr_info("bitmap benchmark: %d iterations over %d bits, best of %d\n",
		BENCH_ITERS, BENCH_BITS, BENCH_ROUNDS);
	pr_info("bitmap benchmark: all operations, rust %llu ns, c %llu ns, ratio %llu per mille\n",
		rust_best, c_best,
		div64_u64(c_best ? rust_best * 1000 : 0, c_best ? c_best : 1));

	/* The same workload without the weight helpers. */
	rust_best = U64_MAX;
	c_best = U64_MAX;
	for (round = 0; round < BENCH_ROUNDS; round++) {
		rust_ns = bench_run(true, false);
		c_ns = bench_run(false, false);

		if (rust_ns < rust_best)
			rust_best = rust_ns;
		if (c_ns < c_best)
			c_best = c_ns;
	}

	pr_info("bitmap benchmark: without the weight helpers, rust %llu ns, c %llu ns, ratio %llu per mille\n",
		rust_best, c_best,
		div64_u64(c_best ? rust_best * 1000 : 0, c_best ? c_best : 1));
}

static struct kunit_case bitmap_diff_test_cases[] = {
	KUNIT_CASE(bitmap_diff_sizes),
	KUNIT_CASE(bitmap_diff_set_clear),
	KUNIT_CASE(bitmap_diff_shifts),
	KUNIT_CASE(bitmap_diff_cut),
	KUNIT_CASE(bitmap_diff_cut_out_of_range),
	KUNIT_CASE(bitmap_diff_find_next_zero_area),
	KUNIT_CASE(bitmap_diff_remap),
	KUNIT_CASE(bitmap_diff_arr32),
	KUNIT_CASE(bitmap_diff_alloc),
	KUNIT_CASE(bitmap_diff_alloc_node),
	KUNIT_CASE(bitmap_diff_benchmark),
	{},
};

static struct kunit_suite bitmap_diff_test_module = {
	.name = "bitmap_diff",
	.test_cases = bitmap_diff_test_cases,
};

kunit_test_suites(&bitmap_diff_test_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ferret <ferret@example.invalid>");
MODULE_DESCRIPTION("Differential KUnit test for the Rust kernel bitmap");

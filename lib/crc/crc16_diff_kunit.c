// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: differential KUnit test for the Rust CRC-16 implementation.
//
// Compares the Rust implementation (crc16(), from lib/crc/crc16_rust.rs) with
// the original C implementation compiled under the test-only symbol
// crc16_c_ref (lib/crc/crc16_ref.o, built from the unmodified lib/crc/crc16.c
// with -Dcrc16=crc16_c_ref).
//
// This suite is only built for CONFIG_CRC16_DIFF_KUNIT_TEST=y and is never part
// of a production kernel.

#include <kunit/test.h>
#include <linux/crc16.h>
#include <linux/prandom.h>
#include <linux/types.h>

extern u16 crc16_c_ref(u16 crc, const u8 *p, size_t len);

static void crc16_diff_edge_cases(struct kunit *test)
{
	/* Length 0 must not dereference the pointer. */
	KUNIT_EXPECT_EQ(test, crc16(0, NULL, 0), crc16_c_ref(0, NULL, 0));
	KUNIT_EXPECT_EQ(test, crc16(0xffff, NULL, 0), crc16_c_ref(0xffff, NULL, 0));

	/* All 65536 initial values over a single byte (covers the whole table
	 * index range through (crc & 0xff) ^ byte). */
	for (u32 crc = 0; crc <= 0xffff; crc++) {
		u8 b = 0x5a;

		KUNIT_ASSERT_EQ(test, crc16((u16)crc, &b, 1),
				crc16_c_ref((u16)crc, &b, 1));
	}

	/* Boundary lengths and unaligned starts. */
	{
		u8 buf[1024];

		for (size_t i = 0; i < sizeof(buf); i++)
			buf[i] = (u8)(i * 31 + 7);

		for (size_t len = 0; len <= 64; len++) {
			for (size_t off = 0; off < 16; off++) {
				u16 init = (u16)(len * 257 + off);

				KUNIT_ASSERT_EQ(test,
					crc16(init, buf + off, len),
					crc16_c_ref(init, buf + off, len));
			}
		}
	}
}

static void crc16_diff_random_streams(struct kunit *test)
{
	struct rnd_state rng;
	u8 buf[1024];

	prandom_seed_state(&rng, 0x666572726574); /* "ferret" */

	for (int iter = 0; iter < 2000; iter++) {
		u16 init = (u16)prandom_u32_state(&rng);
		size_t len = prandom_u32_state(&rng) % (sizeof(buf) + 1);
		size_t off = prandom_u32_state(&rng) % (sizeof(buf) - len + 1);

		prandom_bytes_state(&rng, buf, sizeof(buf));

		KUNIT_ASSERT_EQ_MSG(test, crc16(init, buf + off, len),
				    crc16_c_ref(init, buf + off, len),
				    "iter=%d len=%zu off=%zu init=0x%04x",
				    iter, len, off, init);
	}
}

static struct kunit_case crc16_diff_cases[] = {
	KUNIT_CASE(crc16_diff_edge_cases),
	KUNIT_CASE(crc16_diff_random_streams),
	{}
};

static struct kunit_suite crc16_diff_suite = {
	.name = "crc16_diff",
	.test_cases = crc16_diff_cases,
};
kunit_test_suite(crc16_diff_suite);

MODULE_DESCRIPTION("Ferret differential test: Rust CRC-16 vs original C");
MODULE_LICENSE("GPL");

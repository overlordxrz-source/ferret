// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret: differential KUnit test for the Rust reference counting helpers.
//
// Compares the Rust implementation (lib/refcount_rust.rs) with the original C
// implementation, compiled a second time under test-only symbol names
// (lib/refcount_ref.o, built from the unmodified lib/refcount.c with every
// global symbol renamed; see lib/Makefile).
//
// The two implementations are driven through identical operations on identical
// inputs, and the return values, the resulting counts and the state of the
// locks are compared.
//
// This suite is only built for CONFIG_REFCOUNT_DIFF_KUNIT_TEST=y and is never
// part of a production kernel.

#include <kunit/test.h>

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>

/* The C reference, compiled from the unmodified lib/refcount.c. */
extern void refcount_warn_saturate_c_ref(refcount_t *r, enum refcount_saturation_type t);
extern bool refcount_dec_if_one_c_ref(refcount_t *r);
extern bool refcount_dec_not_one_c_ref(refcount_t *r);
extern bool refcount_dec_and_mutex_lock_c_ref(refcount_t *r, struct mutex *lock);
extern bool refcount_dec_and_lock_c_ref(refcount_t *r, spinlock_t *lock);
extern bool refcount_dec_and_lock_irqsave_c_ref(refcount_t *r, spinlock_t *lock, unsigned long *flags);

/*
 * The counts used by the tests. They cover the empty count, the single
 * reference, the multi-reference case, the largest valid count, the
 * saturated count and the "bad" (negative) counts that the primitive
 * tolerates.
 */
static const int diff_counts[] = {
	0, 1, 2, 3, 4, 17, 1000, INT_MAX, REFCOUNT_SATURATED, -1, INT_MIN,
};

/* The saturation events, as the enum of the C header. */
static const enum refcount_saturation_type diff_saturations[] = {
	REFCOUNT_ADD_NOT_ZERO_OVF,
	REFCOUNT_ADD_OVF,
	REFCOUNT_ADD_UAF,
	REFCOUNT_SUB_UAF,
	REFCOUNT_DEC_LEAK,
};

static void refcount_diff_dec_if_one(struct kunit *test)
{
	unsigned int i;

	/*
	 * Guard against a vacuous test: the two implementations must be
	 * different objects, otherwise the comparison below would compare the
	 * C reference with itself and pass for the wrong reason.
	 */
	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_if_one,
			(unsigned long)refcount_dec_if_one_c_ref);

	for (i = 0; i < ARRAY_SIZE(diff_counts); i++) {
		refcount_t rust, cref;
		bool rust_ret, cref_ret;

		refcount_set(&rust, diff_counts[i]);
		refcount_set(&cref, diff_counts[i]);

		rust_ret = refcount_dec_if_one(&rust);
		cref_ret = refcount_dec_if_one_c_ref(&cref);

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
	}
}

static void refcount_diff_dec_not_one(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_not_one,
			(unsigned long)refcount_dec_not_one_c_ref);

	for (i = 0; i < ARRAY_SIZE(diff_counts); i++) {
		refcount_t rust, cref;
		bool rust_ret, cref_ret;

		refcount_set(&rust, diff_counts[i]);
		refcount_set(&cref, diff_counts[i]);

		rust_ret = refcount_dec_not_one(&rust);
		cref_ret = refcount_dec_not_one_c_ref(&cref);

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
	}
}

static void refcount_diff_warn_saturate(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_warn_saturate,
			(unsigned long)refcount_warn_saturate_c_ref);

	for (i = 0; i < ARRAY_SIZE(diff_saturations); i++) {
		refcount_t rust, cref;

		refcount_set(&rust, 5);
		refcount_set(&cref, 5);

		refcount_warn_saturate(&rust, diff_saturations[i]);
		refcount_warn_saturate_c_ref(&cref, diff_saturations[i]);

		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
		KUNIT_EXPECT_EQ(test, refcount_read(&rust),
				(unsigned int)REFCOUNT_SATURATED);
	}

	/* An unknown event saturates the count as well. */
	{
		refcount_t rust, cref;

		refcount_set(&rust, 5);
		refcount_set(&cref, 5);

		refcount_warn_saturate(&rust, 99);
		refcount_warn_saturate_c_ref(&cref, 99);

		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
		KUNIT_EXPECT_EQ(test, refcount_read(&rust),
				(unsigned int)REFCOUNT_SATURATED);
	}
}

static void refcount_diff_dec_and_mutex_lock(struct kunit *test)
{
	DEFINE_MUTEX(rust_lock);
	DEFINE_MUTEX(cref_lock);
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_and_mutex_lock,
			(unsigned long)refcount_dec_and_mutex_lock_c_ref);

	for (i = 0; i < ARRAY_SIZE(diff_counts); i++) {
		refcount_t rust, cref;
		bool rust_ret, cref_ret;

		refcount_set(&rust, diff_counts[i]);
		refcount_set(&cref, diff_counts[i]);

		rust_ret = refcount_dec_and_mutex_lock(&rust, &rust_lock);
		cref_ret = refcount_dec_and_mutex_lock_c_ref(&cref, &cref_lock);

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));

		/*
		 * On success the lock is held by the caller, on failure it is
		 * not held. Both implementations must agree.
		 */
		KUNIT_EXPECT_EQ(test, mutex_is_locked(&rust_lock),
				mutex_is_locked(&cref_lock));

		if (mutex_is_locked(&rust_lock))
			mutex_unlock(&rust_lock);
		if (mutex_is_locked(&cref_lock))
			mutex_unlock(&cref_lock);
	}
}

static void refcount_diff_dec_and_lock(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_and_lock,
			(unsigned long)refcount_dec_and_lock_c_ref);

	for (i = 0; i < ARRAY_SIZE(diff_counts); i++) {
		spinlock_t rust_lock, cref_lock;
		refcount_t rust, cref;
		bool rust_ret, cref_ret;

		spin_lock_init(&rust_lock);
		spin_lock_init(&cref_lock);

		refcount_set(&rust, diff_counts[i]);
		refcount_set(&cref, diff_counts[i]);

		rust_ret = refcount_dec_and_lock(&rust, &rust_lock);
		cref_ret = refcount_dec_and_lock_c_ref(&cref, &cref_lock);

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
		KUNIT_EXPECT_EQ(test, spin_is_locked(&rust_lock),
				spin_is_locked(&cref_lock));

		if (spin_is_locked(&rust_lock))
			spin_unlock(&rust_lock);
		if (spin_is_locked(&cref_lock))
			spin_unlock(&cref_lock);
	}
}

static void refcount_diff_dec_and_lock_irqsave(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_and_lock_irqsave,
			(unsigned long)refcount_dec_and_lock_irqsave_c_ref);

	for (i = 0; i < ARRAY_SIZE(diff_counts); i++) {
		spinlock_t rust_lock, cref_lock;
		unsigned long rust_flags = 0, cref_flags = 0;
		refcount_t rust, cref;
		bool rust_ret, cref_ret, rust_irq, cref_irq;
		bool irq_before = irqs_disabled();

		spin_lock_init(&rust_lock);
		spin_lock_init(&cref_lock);

		refcount_set(&rust, diff_counts[i]);
		refcount_set(&cref, diff_counts[i]);

		rust_ret = refcount_dec_and_lock_irqsave(&rust, &rust_lock,
							 &rust_flags);
		rust_irq = irqs_disabled();
		cref_ret = refcount_dec_and_lock_irqsave_c_ref(&cref, &cref_lock,
							       &cref_flags);
		cref_irq = irqs_disabled();

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));

		/*
		 * The interrupts are disabled while the lock is held, and
		 * restored when the helper fails. Both implementations must
		 * agree.
		 */
		KUNIT_EXPECT_EQ(test, rust_irq, cref_irq);
		KUNIT_EXPECT_EQ(test, spin_is_locked(&rust_lock),
				spin_is_locked(&cref_lock));

		if (spin_is_locked(&rust_lock))
			spin_unlock_irqrestore(&rust_lock, rust_flags);
		if (spin_is_locked(&cref_lock))
			spin_unlock_irqrestore(&cref_lock, cref_flags);

		/* The interrupt state must be the one we started with. */
		KUNIT_EXPECT_EQ(test, irqs_disabled(), irq_before);
	}
}

/*
 * A sequence of decrements, driven identically on both implementations. The
 * point is the interaction of the helpers: the "dec if one" and the "dec not
 * one" helpers must leave the count in the same state, whichever order they
 * are called in.
 */
static void refcount_diff_sequence(struct kunit *test)
{
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(diff_counts); i++) {
		refcount_t rust, cref;

		refcount_set(&rust, diff_counts[i]);
		refcount_set(&cref, diff_counts[i]);

		for (j = 0; j < 8; j++) {
			bool rust_ret, cref_ret;

			rust_ret = refcount_dec_if_one(&rust);
			cref_ret = refcount_dec_if_one_c_ref(&cref);
			KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
			KUNIT_EXPECT_EQ(test, refcount_read(&rust),
					refcount_read(&cref));

			rust_ret = refcount_dec_not_one(&rust);
			cref_ret = refcount_dec_not_one_c_ref(&cref);
			KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
			KUNIT_EXPECT_EQ(test, refcount_read(&rust),
					refcount_read(&cref));
		}
	}
}

/*
 * The concurrency test. The reference counts are shared by several tasks, so
 * the outcome of a single operation depends on the interleaving. What is
 * checked here are the invariants that must hold whatever the interleaving
 * is:
 *
 * * the count never drops below one, because `refcount_dec_not_one()` refuses
 *   to decrement a count of one, so a run of a known number of decrements
 *   always ends at exactly one;
 * * exactly one task can perform the one-to-zero transition of
 *   `refcount_dec_if_one()`, because it is a single compare-and-exchange.
 *
 * Every task has its own argument structure, so that the tasks share nothing
 * but the count itself. The tasks also have to *start* before the test stops
 * them: `kthread_stop()` on a task that has not run yet kills it before it
 * calls the worker function, which would silently drop one of the tasks.
 */
#define DIFF_THREADS	4
#define DIFF_ITERATIONS	250

/* The phase of the worker: decrement, or drop the last reference. */
enum diff_phase {
	DIFF_PHASE_DEC,
	DIFF_PHASE_DROP,
};

struct diff_thread_args {
	refcount_t *count;
	unsigned int iterations;
	enum diff_phase phase;
	/* The number of tasks that have started. */
	atomic_t *started;
	/* The number of successful "dec if one" transitions. */
	atomic_t dropped;
	/* The number of successful decrements. */
	atomic_t decrements;
	/* The number of refused decrements (the count was one). */
	atomic_t refused;
	/* Set to use the C reference instead of the Rust implementation. */
	bool use_c_ref;
};

static int diff_worker(void *arg)
{
	struct diff_thread_args *args = arg;
	unsigned int i;

	/* The task is running now. */
	atomic_inc(args->started);

	if (args->phase == DIFF_PHASE_DROP) {
		bool dropped;

		if (args->use_c_ref)
			dropped = refcount_dec_if_one_c_ref(args->count);
		else
			dropped = refcount_dec_if_one(args->count);

		if (dropped)
			atomic_inc(&args->dropped);

		return 0;
	}

	for (i = 0; i < args->iterations; i++) {
		bool decremented;

		if (args->use_c_ref)
			decremented = refcount_dec_not_one_c_ref(args->count);
		else
			decremented = refcount_dec_not_one(args->count);

		if (decremented)
			atomic_inc(&args->decrements);
		else
			atomic_inc(&args->refused);
	}

	return 0;
}

/* Run one phase with one implementation and check the results. */
static void diff_run_phase(struct kunit *test, enum diff_phase phase,
			   unsigned int iterations, bool use_c_ref)
{
	struct diff_thread_args args[DIFF_THREADS];
	struct task_struct *threads[DIFF_THREADS];
	atomic_t started = ATOMIC_INIT(0);
	refcount_t shared;
	unsigned int i;
	unsigned int decrements = 0, refused = 0, dropped = 0;
	unsigned int spins = 0;
	int count;

	/*
	 * The count starts one above the number of decrements, so that the
	 * last reference is still there when the decrement phase is over. The
	 * drop phase starts with a single reference, which the tasks race
	 * for.
	 */
	if (phase == DIFF_PHASE_DEC)
		count = DIFF_THREADS * iterations + 1;
	else
		count = 1;

	refcount_set(&shared, count);

	for (i = 0; i < DIFF_THREADS; i++) {
		args[i] = (struct diff_thread_args) {
			.count = &shared,
			.iterations = iterations,
			.phase = phase,
			.started = &started,
			.dropped = ATOMIC_INIT(0),
			.decrements = ATOMIC_INIT(0),
			.refused = ATOMIC_INIT(0),
			.use_c_ref = use_c_ref,
		};
	}

	for (i = 0; i < DIFF_THREADS; i++) {
		threads[i] = kthread_run(diff_worker, &args[i],
					 use_c_ref ? "ferret_refcount_c_ref" :
						     "ferret_refcount_rust");
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, threads[i]);
	}

	/* Wait for the tasks to be running before stopping them. */
	while (atomic_read(&started) < DIFF_THREADS && spins++ < 10000)
		udelay(100);
	KUNIT_ASSERT_EQ(test, atomic_read(&started), DIFF_THREADS);

	for (i = 0; i < DIFF_THREADS; i++)
		kthread_stop(threads[i]);

	for (i = 0; i < DIFF_THREADS; i++) {
		decrements += atomic_read(&args[i].decrements);
		refused += atomic_read(&args[i].refused);
		dropped += atomic_read(&args[i].dropped);
	}

	pr_info("ferret refcount: %s phase %d: count %u decrements %u refused %u dropped %u\n",
		use_c_ref ? "c_ref" : "rust", phase, refcount_read(&shared),
		decrements, refused, dropped);

	switch (phase) {
	case DIFF_PHASE_DEC:
		/*
		 * The decrements never go below one, so the count is exactly
		 * the one that was left, and every decrement was successful.
		 */
		KUNIT_EXPECT_EQ(test, refcount_read(&shared), 1u);
		KUNIT_EXPECT_EQ(test, decrements, DIFF_THREADS * iterations);
		KUNIT_EXPECT_EQ(test, refused, 0u);
		break;
	case DIFF_PHASE_DROP:
		/* Exactly one task dropped the last reference. */
		KUNIT_EXPECT_EQ(test, dropped, 1u);
		KUNIT_EXPECT_EQ(test, refcount_read(&shared), 0u);
		break;
	}
}

static void refcount_diff_concurrent(struct kunit *test)
{
	/*
	 * A thousand decrements spread over four tasks, then the race for the
	 * last reference. Both implementations have to reach the same state.
	 */
	diff_run_phase(test, DIFF_PHASE_DEC, DIFF_ITERATIONS, false);
	diff_run_phase(test, DIFF_PHASE_DEC, DIFF_ITERATIONS, true);

	diff_run_phase(test, DIFF_PHASE_DROP, 1, false);
	diff_run_phase(test, DIFF_PHASE_DROP, 1, true);
}

static struct kunit_case refcount_diff_test_cases[] = {
	KUNIT_CASE(refcount_diff_dec_if_one),
	KUNIT_CASE(refcount_diff_dec_not_one),
	KUNIT_CASE(refcount_diff_warn_saturate),
	KUNIT_CASE(refcount_diff_dec_and_mutex_lock),
	KUNIT_CASE(refcount_diff_dec_and_lock),
	KUNIT_CASE(refcount_diff_dec_and_lock_irqsave),
	KUNIT_CASE(refcount_diff_sequence),
	KUNIT_CASE(refcount_diff_concurrent),
	{},
};

static struct kunit_suite refcount_diff_test_module = {
	.name = "refcount_diff",
	.test_cases = refcount_diff_test_cases,
};

kunit_test_suites(&refcount_diff_test_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ferret <ferret@example.invalid>");
MODULE_DESCRIPTION("Differential KUnit test for the Rust reference counting helpers");

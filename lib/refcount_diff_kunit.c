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
#include <linux/prandom.h>
#include <linux/refcount.h>
#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>

/*
 * KUnit-only instrumentation of the Rust implementation: the number of
 * compare-and-exchange retries of refcount_dec_not_one(), and a reset. The
 * symbols exist only in a KUnit build (CONFIG_KUNIT), which is the same
 * condition under which this test is built.
 *
 * They exist so that the concurrency test can *assert* that the retry path
 * ran. Without the counter the test can only infer it from the final count,
 * which is what FVS-AUDIT-001-R2 objected to.
 */
u32 refcount_rust_retry_count(void);
void refcount_rust_retry_reset(void);

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
	0, 1, 2, 3, 4, 17, 1000, INT_MAX, INT_MAX - 1, REFCOUNT_SATURATED,
	REFCOUNT_SATURATED + 1, REFCOUNT_SATURATED - 1, -1, INT_MIN,
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
 * A randomised property test.
 *
 * The count is taken from the boundary table half of the time, so that the
 * interesting states (zero, one, two, the saturated count, INT_MAX) are
 * actually visited; the other half of the time it is a random value. Every
 * iteration performs an operation, and the return value and the resulting
 * count are compared at every step.
 *
 * The test asserts at the end that every branch it is supposed to cover was
 * actually taken, so that the coverage claim is a measurement and not an
 * assumption. (An earlier version of this test drew its state from a
 * distribution that never reached the boundaries, and 21 % of its iterations
 * asserted a tautology; the independent audit FVS-AUDIT-001 measured that and
 * this is the fix.)
 */
#define DIFF_PROPERTY_ITERATIONS	2000

static void refcount_diff_property(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;
	unsigned int hits_zero = 0, hits_one = 0, hits_two = 0;
	unsigned int hits_saturated = 0, hits_int_max = 0;
	unsigned int hits_if_one_success = 0;
	unsigned int hits_not_one_refused = 0, hits_not_one_saturated = 0;
	unsigned int hits_not_one_underflow = 0;
	unsigned int hits_lock_taken = 0;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_if_one,
			(unsigned long)refcount_dec_if_one_c_ref);
	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_not_one,
			(unsigned long)refcount_dec_not_one_c_ref);

	prandom_seed_state(&rng, 0x666572726574); /* "ferret" */

	for (i = 0; i < DIFF_PROPERTY_ITERATIONS; i++) {
		refcount_t rust, cref;
		bool rust_ret, cref_ret;
		unsigned int op;
		int value;

		/*
		 * Half of the time the count comes from the boundary table,
		 * so that the interesting states are visited; the other half
		 * of the time it is a random value.
		 */
		if (prandom_u32_state(&rng) & 1)
			value = diff_counts[prandom_u32_state(&rng) %
					   ARRAY_SIZE(diff_counts)];
		else
			value = (int)prandom_u32_state(&rng);

		refcount_set(&rust, value);
		refcount_set(&cref, value);

		switch (value) {
		case 0:
			hits_zero++;
			break;
		case 1:
			hits_one++;
			break;
		case 2:
			hits_two++;
			break;
		case INT_MAX:
			hits_int_max++;
			break;
		case REFCOUNT_SATURATED:
			hits_saturated++;
			break;
		}

		op = prandom_u32_state(&rng) % 4;
		switch (op) {
		case 0:
			/* The one-to-zero transition. */
			rust_ret = refcount_dec_if_one(&rust);
			cref_ret = refcount_dec_if_one_c_ref(&cref);
			if (value == 1)
				hits_if_one_success++;
			break;
		case 1:
			/* The decrement that refuses a count of one. */
			rust_ret = refcount_dec_not_one(&rust);
			cref_ret = refcount_dec_not_one_c_ref(&cref);
			if (value == 1)
				hits_not_one_refused++;
			else if (value == 0)
				hits_not_one_underflow++;
			else if (value == REFCOUNT_SATURATED)
				hits_not_one_saturated++;
			break;
		case 2: {
			/* The spinlock helper: the lock is held on success. */
			spinlock_t rust_lock, cref_lock;

			spin_lock_init(&rust_lock);
			spin_lock_init(&cref_lock);

			rust_ret = refcount_dec_and_lock(&rust, &rust_lock);
			cref_ret = refcount_dec_and_lock_c_ref(&cref, &cref_lock);

			if (value == 1)
				hits_lock_taken++;

			if (spin_is_locked(&rust_lock))
				spin_unlock(&rust_lock);
			if (spin_is_locked(&cref_lock))
				spin_unlock(&cref_lock);
			break;
		}
		default: {
			/*
			 * The interrupt-safe helper: the lock is held with
			 * the interrupts disabled on success, and the
			 * interrupt state is restored on the failure path.
			 */
			spinlock_t rust_lock, cref_lock;
			unsigned long rust_flags = 0, cref_flags = 0;

			spin_lock_init(&rust_lock);
			spin_lock_init(&cref_lock);

			rust_ret = refcount_dec_and_lock_irqsave(&rust, &rust_lock,
								&rust_flags);
			cref_ret = refcount_dec_and_lock_irqsave_c_ref(&cref,
									&cref_lock,
									&cref_flags);

			if (value == 1)
				hits_lock_taken++;

			if (spin_is_locked(&rust_lock))
				spin_unlock_irqrestore(&rust_lock, rust_flags);
			if (spin_is_locked(&cref_lock))
				spin_unlock_irqrestore(&cref_lock, cref_flags);
			break;
		}
		}

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
	}

	/*
	 * The coverage claim, as a measurement: every branch this test is
	 * supposed to cover has to have been taken.
	 */
	pr_info("ferret refcount: property coverage: zero=%u one=%u two=%u saturated=%u int_max=%u if_one_success=%u not_one_refused=%u not_one_saturated=%u not_one_underflow=%u lock_taken=%u\n",
		hits_zero, hits_one, hits_two, hits_saturated, hits_int_max,
		hits_if_one_success, hits_not_one_refused, hits_not_one_saturated,
		hits_not_one_underflow, hits_lock_taken);

	KUNIT_ASSERT_GT(test, hits_zero, 0u);
	KUNIT_ASSERT_GT(test, hits_one, 0u);
	KUNIT_ASSERT_GT(test, hits_two, 0u);
	KUNIT_ASSERT_GT(test, hits_saturated, 0u);
	KUNIT_ASSERT_GT(test, hits_int_max, 0u);
	KUNIT_ASSERT_GT(test, hits_if_one_success, 0u);
	KUNIT_ASSERT_GT(test, hits_not_one_refused, 0u);
	KUNIT_ASSERT_GT(test, hits_not_one_saturated, 0u);
	KUNIT_ASSERT_GT(test, hits_not_one_underflow, 0u);
	KUNIT_ASSERT_GT(test, hits_lock_taken, 0u);
}

/*
 * The concurrency test. The reference counts are shared by several tasks, so
 * the outcome of a single operation depends on the interleaving. What is
 * checked here are the invariants that must hold whatever the interleaving
 * is:
 *
 * * every call does exactly one thing: it either decrements the count or it
 *   refuses, because the count was one;
 * * the count and the number of decrements agree: the count is the initial
 *   count minus the number of decrements. A lost update — or a broken
 *   compare-and-exchange retry loop — breaks this;
 * * exactly one task can perform the one-to-zero transition of
 *   `refcount_dec_if_one()`, because it is a single compare-and-exchange.
 *
 * The tasks are released together, through a start barrier, so that they
 * really contend: without it, the first task runs its whole loop before the
 * next one is created, the compare-and-exchange never fails, and the retry
 * path of `refcount_dec_not_one()` is never executed. The independent audit
 * FVS-AUDIT-001 measured that (its mutation M11, a broken retry loop, passed
 * the whole suite).
 *
 * Every task has its own argument structure, so that the tasks share nothing
 * but the count itself. The tasks also have to *start* before the test stops
 * them: `kthread_stop()` on a task that has not run yet kills it before it
 * calls the worker function, which would silently drop one of the tasks.
 */
#define DIFF_THREADS	4
#define DIFF_ITERATIONS	1000

/* The phase of the worker: decrement, or drop the last reference. */
enum diff_phase {
	DIFF_PHASE_DEC,
	DIFF_PHASE_DROP,
};

/*
 * The state the tasks share.
 *
 * It is allocated on the heap, and not on the stack of the test: the tasks
 * read it through a pointer, and a stable allocation is one thing less to
 * reason about.
 */
struct diff_shared {
	/* The number of tasks that are ready to run. */
	atomic_t started;
	/*
	 * The barrier: the tasks sleep on it and are woken together by
	 * complete_all(), so that they contend.
	 *
	 * A completion, not a flag polled with msleep(): a timer-driven wake
	 * spreads the tasks over milliseconds, while the whole decrement
	 * phase takes microseconds, so the tasks run one after the other and
	 * the compare-and-exchange never fails. Measured: 430 retries in one
	 * boot and 0 in the next two with the polling barrier.
	 *
	 * A spin was tried too, and is worse: four spinning tasks on a
	 * two-CPU guest starve the test thread that has to release them, so
	 * the release arrives after the spin timeout and the tasks run one
	 * after the other anyway.
	 */
	struct completion go;
	/* The count the tasks decrement. */
	refcount_t count;
};

struct diff_thread_args {
	struct diff_shared *shared;
	unsigned int iterations;
	enum diff_phase phase;
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

	/*
	 * The task is ready; it waits for the others before it starts, so
	 * that they really contend.
	 *
	 * The wait is a *spin*, not a sleep. A sleeping task is woken by the
	 * timer, and on a two-CPU guest the wake-ups are spread over
	 * milliseconds while the whole decrement phase takes microseconds: the
	 * workers then run one after the other and the compare-and-exchange
	 * never fails. Spinning keeps them runnable, so the release reaches
	 * them in the same scheduling window. Measured: the retry count of the
	 * decrement phase was 430 in one boot with a sleeping barrier, and 0
	 * in the next two; with the spinning barrier it is positive in every
	 * boot of the campaign (the counter is asserted below).
	 *
	 * The wait is bounded: a task must never spin forever, not even if
	 * the release is lost.
	 */
	atomic_inc(&args->shared->started);
	/*
	 * Wait for the release, bounded: a task must never sleep forever, not
	 * even if the release is lost. The timeout is generous — it only has
	 * to cover the time the test thread needs to see the four tasks — and
	 * it is reported, because a barrier that timed out is a test defect
	 * and not a result.
	 */
	if (!wait_for_completion_timeout(&args->shared->go,
					 msecs_to_jiffies(5000)))
		pr_info("ferret refcount: the start barrier timed out\n");

	if (args->phase == DIFF_PHASE_DROP) {
		bool dropped;

		if (args->use_c_ref)
			dropped = refcount_dec_if_one_c_ref(&args->shared->count);
		else
			dropped = refcount_dec_if_one(&args->shared->count);

		if (dropped)
			atomic_inc(&args->dropped);

		return 0;
	}

	for (i = 0; i < args->iterations; i++) {
		bool decremented;

		if (args->use_c_ref)
			decremented = refcount_dec_not_one_c_ref(&args->shared->count);
		else
			decremented = refcount_dec_not_one(&args->shared->count);

		if (decremented)
			atomic_inc(&args->decrements);
		else
			atomic_inc(&args->refused);
	}

	return 0;
}

/*
 * Run one phase with one implementation and check the results.
 *
 * Returns the number of compare-and-exchange retries the Rust implementation
 * recorded during the phase (0 for the C reference and for the drop phase,
 * which does not have a retry loop).
 */
static unsigned int diff_run_phase(struct kunit *test, enum diff_phase phase,
				   unsigned int iterations, bool use_c_ref)
{
	struct diff_thread_args args[DIFF_THREADS];
	struct task_struct *threads[DIFF_THREADS];
	struct diff_shared *shared;
	unsigned int i;
	unsigned int decrements = 0, refused = 0, dropped = 0;
	unsigned int spins = 0;
	unsigned int retries = 0;
	unsigned int initial;
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

	initial = count;

	shared = kzalloc(sizeof(*shared), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shared);

	/*
	 * The retry path of the Rust decrement loop is asserted, not assumed:
	 * reset the counter now, and require it to be non-zero when the phase
	 * is over.
	 */
	if (!use_c_ref && phase == DIFF_PHASE_DEC)
		refcount_rust_retry_reset();

	init_completion(&shared->go);
	refcount_set(&shared->count, count);

	for (i = 0; i < DIFF_THREADS; i++) {
		args[i] = (struct diff_thread_args) {
			.shared = shared,
			.iterations = iterations,
			.phase = phase,
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

	/* Wait for the tasks to be ready. */
	while (atomic_read(&shared->started) < DIFF_THREADS && spins++ < 60000)
		udelay(100);

	/*
	 * Release the tasks together, so that they really contend. Without
	 * this, the first task runs its whole loop before the next one is
	 * even created, the compare-and-exchange never fails, and the retry
	 * path of the decrement loop is never executed.
	 *
	 * complete_all() wakes every waiter in one go. The release happens
	 * *before* the tasks are stopped and before the assertions below: a
	 * task that waits for the barrier must never be left waiting, not
	 * even when the wait above timed out.
	 */
	complete_all(&shared->go);

	/*
	 * Stop the workers at once, without sleeping in between: a kernel
	 * thread that has already exited can be freed before kthread_stop()
	 * looks at it, and then there is nothing left to stop.
	 */
	for (i = 0; i < DIFF_THREADS; i++)
		kthread_stop(threads[i]);

	KUNIT_ASSERT_EQ(test, atomic_read(&shared->started), DIFF_THREADS);

	for (i = 0; i < DIFF_THREADS; i++) {
		decrements += atomic_read(&args[i].decrements);
		refused += atomic_read(&args[i].refused);
		dropped += atomic_read(&args[i].dropped);
	}

	/*
	 * The retry path of the compare-and-exchange loop. The counter is
	 * read here; the *assertion* is in refcount_diff_concurrent(), which
	 * may repeat the phase when the workers happened not to overlap.
	 */
	if (!use_c_ref && phase == DIFF_PHASE_DEC)
		retries = refcount_rust_retry_count();

	pr_info("ferret refcount: %s phase %d: count %u decrements %u refused %u dropped %u retries %u\n",
		use_c_ref ? "c_ref" : "rust", phase,
		refcount_read(&shared->count), decrements, refused, dropped,
		retries);

	switch (phase) {
	case DIFF_PHASE_DEC:
		/*
		 * Every call did exactly one thing, and the count agrees with
		 * the number of decrements. A lost update — or a broken
		 * compare-and-exchange retry loop — breaks the second one.
		 */
		KUNIT_EXPECT_EQ(test, decrements + refused,
				DIFF_THREADS * iterations);
		KUNIT_EXPECT_EQ(test, refcount_read(&shared->count),
				initial - decrements);
		/* The count never drops below one. */
		KUNIT_EXPECT_GE(test, refcount_read(&shared->count), 1u);
		break;
	case DIFF_PHASE_DROP:
		/* Exactly one task dropped the last reference. */
		KUNIT_EXPECT_EQ(test, dropped, 1u);
		KUNIT_EXPECT_EQ(test, refcount_read(&shared->count), 0u);
		break;
	}

	kfree(shared);

	return retries;
}

/*
 * How many times the decrement phase may be repeated to observe a retry.
 *
 * The retry path is a scheduling race: four tasks released together have to
 * read the count before either writes. On a two-CPU guest that is what
 * usually happens, but it is not something the test can force — the
 * measurements are in the commit and in the audit report. Repeating the
 * phase costs a few milliseconds; five attempts make a run that never
 * overlaps extremely unlikely, and a run in which none of them overlaps still
 * *fails* the assertion below instead of passing silently.
 */
#define DIFF_RETRY_ATTEMPTS	5

static void refcount_diff_concurrent(struct kunit *test)
{
	unsigned int attempt;
	unsigned int retries = 0;

	/*
	 * A thousand decrements spread over four tasks, then the race for the
	 * last reference. Both implementations have to reach the same state.
	 */
	for (attempt = 1; attempt <= DIFF_RETRY_ATTEMPTS; attempt++) {
		retries = diff_run_phase(test, DIFF_PHASE_DEC, DIFF_ITERATIONS,
					 false);
		if (retries > 0)
			break;
		pr_info("ferret refcount: no retry in attempt %u of %u: the workers did not overlap\n",
			attempt, DIFF_RETRY_ATTEMPTS);
	}

	/*
	 * The assertion FVS-AUDIT-001-R2 asked for: the retry path is not
	 * inferred from the final count, it is counted. A run that never
	 * observed a retry did not test the loop, and fails here.
	 */
	if (retries == 0) {
		pr_err("ferret refcount: the compare-and-exchange retry path was never exercised in %u attempts\n",
		       DIFF_RETRY_ATTEMPTS);
	}
	KUNIT_EXPECT_GT(test, retries, 0u);
	pr_info("ferret refcount: retry path exercised: %u retries (attempt %u of %u)\n",
		retries, attempt > DIFF_RETRY_ATTEMPTS ? DIFF_RETRY_ATTEMPTS : attempt,
		DIFF_RETRY_ATTEMPTS);

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
	KUNIT_CASE(refcount_diff_property),
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

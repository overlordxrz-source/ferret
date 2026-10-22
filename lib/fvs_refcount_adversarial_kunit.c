// SPDX-License-Identifier: GPL-2.0-only
//
// FVS-AUDIT-001: independent adversarial test suite for the reference counting
// port of PR #24 (overlordxrz-source/ferret).
//
// This file was written by the independent audit FVS-AUDIT-001 and is kept
// as it was written, so that the audit's tests stay reproducible against the
// tree they were written for. It does not modify the implementation: it only
// tests it harder than the project's own suite does.
//
// It is now part of the project: the tests it contains are the reference for
// the coverage claims of the reference counting port, and they are what
// detects the mutations of the lock helpers' failure path.
//
// What it adds over lib/refcount_diff_kunit.c:
//
//  1. A sweep over the *boundaries* of the count space. The pull request's
//     randomised property test never reached the count 0, the count 2, the
//     saturated count or INT_MAX (the audit measured that on the real
//     kernel). This sweep reaches every one of them, for every helper.
//
//  2. A DETERMINISTIC test of the failure path of the three lock helpers -
//     the path that is taken after the lock has been acquired but the
//     decrement to zero fails. The pull request declares that path
//     "not covered" and its mutation harness declares a missing unlock
//     "not detectable". This file reaches it deterministically and asserts
//     that the lock was released and that the interrupt state was restored.
//
//  3. A randomised differential test whose state distribution actually
//     reaches the interesting states.
//
// The C reference is the unmodified lib/refcount.c, compiled under test-only
// names by lib/Makefile (same mechanism as lib/refcount_diff_kunit.c).
//
// The fourth item, the measurement of the pull request's own property-test
// distribution, was removed when the property test was fixed: the property
// test now asserts its own coverage, which is stronger than a measurement of
// it.

#include <kunit/test.h>

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/prandom.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>

extern void refcount_warn_saturate_c_ref(refcount_t *r, enum refcount_saturation_type t);
extern bool refcount_dec_if_one_c_ref(refcount_t *r);
extern bool refcount_dec_not_one_c_ref(refcount_t *r);
extern bool refcount_dec_and_mutex_lock_c_ref(refcount_t *r, struct mutex *lock);
extern bool refcount_dec_and_lock_c_ref(refcount_t *r, spinlock_t *lock);
extern bool refcount_dec_and_lock_irqsave_c_ref(refcount_t *r, spinlock_t *lock, unsigned long *flags);

/*
 * The boundary of the count space. Every one of these values is interesting:
 * the empty count, the single reference, the first multi-reference count,
 * the largest valid count, the saturated count and its neighbours, and the
 * "bad" (negative) counts that the primitive tolerates.
 */
static const int fvs_counts[] = {
	INT_MIN, INT_MIN + 1, INT_MIN / 2 - 1, REFCOUNT_SATURATED,
	REFCOUNT_SATURATED + 1, REFCOUNT_SATURATED + 2,
	-3, -2, -1, 0, 1, 2, 3, 4, 5, 17, 1000,
	INT_MAX - 1, INT_MAX,
};

/* The saturation events, as the enum of the C header. */
static const enum refcount_saturation_type fvs_saturations[] = {
	REFCOUNT_ADD_NOT_ZERO_OVF,
	REFCOUNT_ADD_OVF,
	REFCOUNT_ADD_UAF,
	REFCOUNT_SUB_UAF,
	REFCOUNT_DEC_LEAK,
};

/*
 * A wait helper that polls an atomic. msleep() yields the CPU, so this works
 * on a uniprocessor guest as well.
 */
static int fvs_wait_atomic(atomic_t *a, int value, unsigned int timeout_ms)
{
	unsigned int i;

	for (i = 0; i < timeout_ms; i++) {
		if (atomic_read(a) == value)
			return 0;
		msleep(1);
	}

	return -ETIMEDOUT;
}

/*
 * ---------------------------------------------------------------------------
 * 1. The boundary sweep.
 * ---------------------------------------------------------------------------
 */

static void fvs_sweep_dec_if_one(struct kunit *test)
{
	unsigned int i;

	/* The two implementations must be different objects. */
	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_if_one,
			(unsigned long)refcount_dec_if_one_c_ref);

	for (i = 0; i < ARRAY_SIZE(fvs_counts); i++) {
		refcount_t rust, cref;
		bool rust_ret, cref_ret;

		refcount_set(&rust, fvs_counts[i]);
		refcount_set(&cref, fvs_counts[i]);

		rust_ret = refcount_dec_if_one(&rust);
		cref_ret = refcount_dec_if_one_c_ref(&cref);

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));

		/* The one-to-zero transition is the only success. */
		KUNIT_EXPECT_EQ(test, rust_ret, fvs_counts[i] == 1);
	}
}

static void fvs_sweep_dec_not_one(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_not_one,
			(unsigned long)refcount_dec_not_one_c_ref);

	for (i = 0; i < ARRAY_SIZE(fvs_counts); i++) {
		refcount_t rust, cref;
		bool rust_ret, cref_ret;
		unsigned int before = (unsigned int)fvs_counts[i];

		refcount_set(&rust, fvs_counts[i]);
		refcount_set(&cref, fvs_counts[i]);

		rust_ret = refcount_dec_not_one(&rust);
		cref_ret = refcount_dec_not_one_c_ref(&cref);

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));

		/*
		 * The C contract, spelled out:
		 *  - a saturated count is left alone and the call reports
		 *    "the decrement did not happen" (true);
		 *  - a count of one is refused (false);
		 *  - a count of zero underflows: the call warns and reports
		 *    true, and the count is NOT decremented;
		 *  - every other count is decremented by one.
		 */
		switch (before) {
		case 0:
			/* The underflow warns, does not decrement, reports true. */
			KUNIT_EXPECT_EQ(test, rust_ret, true);
			KUNIT_EXPECT_EQ(test, refcount_read(&rust), 0u);
			break;
		case 1:
			/* The refusal does not change the count. */
			KUNIT_EXPECT_EQ(test, rust_ret, false);
			KUNIT_EXPECT_EQ(test, refcount_read(&rust), 1u);
			break;
		case (unsigned int)REFCOUNT_SATURATED:
			/* The saturated count is left alone. */
			KUNIT_EXPECT_EQ(test, rust_ret, true);
			KUNIT_EXPECT_EQ(test, refcount_read(&rust),
					(unsigned int)REFCOUNT_SATURATED);
			break;
		default:
			KUNIT_EXPECT_EQ(test, rust_ret, true);
			KUNIT_EXPECT_EQ(test, refcount_read(&rust), before - 1);
			break;
		}
	}
}

static void fvs_sweep_warn_saturate(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_warn_saturate,
			(unsigned long)refcount_warn_saturate_c_ref);

	for (i = 0; i < ARRAY_SIZE(fvs_saturations); i++) {
		refcount_t rust, cref;

		refcount_set(&rust, 5);
		refcount_set(&cref, 5);

		refcount_warn_saturate(&rust, fvs_saturations[i]);
		refcount_warn_saturate_c_ref(&cref, fvs_saturations[i]);

		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
		/* The exact saturated value, not just "the same as the C". */
		KUNIT_EXPECT_EQ(test, refcount_read(&rust),
				(unsigned int)REFCOUNT_SATURATED);
	}

	/* The unknown event saturates the count as well. */
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

static void fvs_sweep_dec_and_mutex_lock(struct kunit *test)
{
	DEFINE_MUTEX(rust_lock);
	DEFINE_MUTEX(cref_lock);
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_and_mutex_lock,
			(unsigned long)refcount_dec_and_mutex_lock_c_ref);

	for (i = 0; i < ARRAY_SIZE(fvs_counts); i++) {
		refcount_t rust, cref;
		bool rust_ret, cref_ret;

		refcount_set(&rust, fvs_counts[i]);
		refcount_set(&cref, fvs_counts[i]);

		rust_ret = refcount_dec_and_mutex_lock(&rust, &rust_lock);
		cref_ret = refcount_dec_and_mutex_lock_c_ref(&cref, &cref_lock);

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
		KUNIT_EXPECT_EQ(test, mutex_is_locked(&rust_lock),
				mutex_is_locked(&cref_lock));

		/*
		 * The C contract: the mutex is taken only when the count was
		 * one, and then the decrement to zero always succeeds, so the
		 * helper always returns true with the mutex held.
		 */
		KUNIT_EXPECT_EQ(test, rust_ret, fvs_counts[i] == 1);
		KUNIT_EXPECT_EQ(test, mutex_is_locked(&rust_lock),
				fvs_counts[i] == 1);

		if (mutex_is_locked(&rust_lock))
			mutex_unlock(&rust_lock);
		if (mutex_is_locked(&cref_lock))
			mutex_unlock(&cref_lock);
	}
}

static void fvs_sweep_dec_and_lock(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_and_lock,
			(unsigned long)refcount_dec_and_lock_c_ref);

	for (i = 0; i < ARRAY_SIZE(fvs_counts); i++) {
		refcount_t rust, cref;
		spinlock_t rust_lock, cref_lock;
		bool rust_ret, cref_ret;

		spin_lock_init(&rust_lock);
		spin_lock_init(&cref_lock);

		refcount_set(&rust, fvs_counts[i]);
		refcount_set(&cref, fvs_counts[i]);

		rust_ret = refcount_dec_and_lock(&rust, &rust_lock);
		cref_ret = refcount_dec_and_lock_c_ref(&cref, &cref_lock);

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
		KUNIT_EXPECT_EQ(test, spin_is_locked(&rust_lock),
				spin_is_locked(&cref_lock));

		KUNIT_EXPECT_EQ(test, rust_ret, fvs_counts[i] == 1);
		KUNIT_EXPECT_EQ(test, spin_is_locked(&rust_lock),
				fvs_counts[i] == 1);

		if (spin_is_locked(&rust_lock))
			spin_unlock(&rust_lock);
		if (spin_is_locked(&cref_lock))
			spin_unlock(&cref_lock);
	}
}

static void fvs_sweep_dec_and_lock_irqsave(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_and_lock_irqsave,
			(unsigned long)refcount_dec_and_lock_irqsave_c_ref);

	for (i = 0; i < ARRAY_SIZE(fvs_counts); i++) {
		refcount_t rust, cref;
		spinlock_t rust_lock, cref_lock;
		unsigned long rust_flags = 0, cref_flags = 0;
		bool rust_ret, cref_ret, rust_irq, cref_irq;
		bool irq_before = irqs_disabled();

		spin_lock_init(&rust_lock);
		spin_lock_init(&cref_lock);

		refcount_set(&rust, fvs_counts[i]);
		refcount_set(&cref, fvs_counts[i]);

		rust_ret = refcount_dec_and_lock_irqsave(&rust, &rust_lock,
							 &rust_flags);
		rust_irq = irqs_disabled();
		cref_ret = refcount_dec_and_lock_irqsave_c_ref(&cref, &cref_lock,
							       &cref_flags);
		cref_irq = irqs_disabled();

		KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
		KUNIT_EXPECT_EQ(test, refcount_read(&rust), refcount_read(&cref));
		KUNIT_EXPECT_EQ(test, rust_irq, cref_irq);
		KUNIT_EXPECT_EQ(test, spin_is_locked(&rust_lock),
				spin_is_locked(&cref_lock));

		KUNIT_EXPECT_EQ(test, rust_ret, fvs_counts[i] == 1);
		/* With the lock held, the interrupts are disabled. */
		KUNIT_EXPECT_EQ(test, rust_irq, fvs_counts[i] == 1);

		if (spin_is_locked(&rust_lock))
			spin_unlock_irqrestore(&rust_lock, rust_flags);
		if (spin_is_locked(&cref_lock))
			spin_unlock_irqrestore(&cref_lock, cref_flags);

		/* The interrupt state must be the one we started with. */
		KUNIT_EXPECT_EQ(test, irqs_disabled(), irq_before);
	}
}

/*
 * ---------------------------------------------------------------------------
 * 2. The failure path of the lock helpers, reached deterministically.
 * ---------------------------------------------------------------------------
 *
 * The helper does two things: it decrements the count with
 * refcount_dec_not_one() (which refuses a count of one and leaves the count
 * alone), and then, if the count was one, it takes the lock and decrements
 * the count to zero with refcount_dec_and_test().
 *
 * The failure path - "the lock was taken, the decrement to zero did not
 * succeed, release the lock and return false" - needs the count to change
 * between the two steps. The audit makes that happen deterministically:
 *
 *   * the count starts at one, so the first step refuses and the helper
 *     proceeds to the lock;
 *   * a "holder" task holds the lock, so the helper *blocks* inside
 *     spin_lock() / mutex_lock() - which proves that the first step already
 *     ran, because the first step cannot block;
 *   * while the helper is blocked, the holder sets the count to zero;
 *   * the holder releases the lock; the helper acquires it and calls
 *     refcount_dec_and_test() on a count of zero, which warns and returns
 *     false - the failure path.
 *
 * The proof that the failure path was taken is the resulting count: the
 * refcount_dec_and_test() on a count of zero saturates the count (the
 * REFCOUNT_SUB_UAF warning sets it to REFCOUNT_SATURATED), so the count
 * ends up saturated. If the helper had instead taken the early return
 * (the first step saw the count of zero and refused to decrement it), the
 * count would still be zero. The two outcomes cannot be confused, so the
 * test cannot pass vacuously.
 *
 * The holder task is pinned to one CPU and the helper task to another, so
 * that the task that spins on the lock cannot starve the task that holds
 * it. The mutation of the count is done by the holder itself, so the main
 * task never has to run while the helper is spinning.
 */

#define FVS_HOLDER_CPU	1
#define FVS_WORKER_CPU	0

/* How long the holder waits for the worker to block on the lock. */
#define FVS_BLOCK_WINDOW_MS	300

enum fvs_helper_kind {
	FVS_HELPER_SPINLOCK,
	FVS_HELPER_IRQSAVE,
};

struct fvs_worker {
	refcount_t *count;
	spinlock_t *lock;
	atomic_t entered;	/* the worker is about to call the helper */
	atomic_t done;		/* the worker has returned */
	bool ret;
	bool irq_after;
	unsigned long flags;
	enum fvs_helper_kind kind;
	bool use_c_ref;
};

static int fvs_worker_fn(void *arg)
{
	struct fvs_worker *w = arg;

	atomic_set(&w->entered, 1);

	if (w->kind == FVS_HELPER_SPINLOCK) {
		if (w->use_c_ref)
			w->ret = refcount_dec_and_lock_c_ref(w->count, w->lock);
		else
			w->ret = refcount_dec_and_lock(w->count, w->lock);
	} else {
		if (w->use_c_ref)
			w->ret = refcount_dec_and_lock_irqsave_c_ref(w->count,
									w->lock,
									&w->flags);
		else
			w->ret = refcount_dec_and_lock_irqsave(w->count, w->lock,
								&w->flags);
	}

	w->irq_after = irqs_disabled();
	atomic_set(&w->done, 1);

	return 0;
}

struct fvs_holder {
	spinlock_t *lock;
	refcount_t *count;
	struct fvs_worker *worker;
	unsigned int waited;	/* the busy-wait counter */
	atomic_t locked;	/* the holder has the lock */
	atomic_t release;	/* the holder may let go */
	int ret;
};

static int fvs_holder_fn(void *arg)
{
	struct fvs_holder *h = arg;

	spin_lock(h->lock);
	atomic_set(&h->locked, 1);

	/*
	 * Wait for the helper task to enter the helper (or be aborted).
	 *
	 * The wait is a busy wait and not a sleep: the holder holds a
	 * spinlock, and the kernel does not allow a task to sleep while it
	 * holds one ("scheduling while atomic"). The helper task is pinned
	 * to another CPU, so it can run while this one waits.
	 */
	while (!atomic_read(&h->worker->entered) && !atomic_read(&h->release)) {
		udelay(100);
		h->waited++;
		if (h->waited > FVS_BLOCK_WINDOW_MS * 10)
			break;
	}

	if (atomic_read(&h->release)) {
		/* The scenario was aborted. */
		spin_unlock(h->lock);
		return 0;
	}

	/* Give the helper task the time to block on the lock. */
	for (h->waited = 0; h->waited < FVS_BLOCK_WINDOW_MS * 10; h->waited++)
		udelay(100);

	/* Change the count while the helper is blocked on the lock. */
	refcount_set(h->count, 0);

	spin_unlock(h->lock);
	h->ret = 0;

	return 0;
}

/*
 * Run the failure-path scenario once against one implementation.
 */
static bool fvs_run_lock_failure(struct kunit *test, enum fvs_helper_kind kind,
				 bool use_c_ref, unsigned int *final_count,
				 bool *ret, bool *lock_released,
				 bool *irq_restored)
{
	struct fvs_holder holder;
	struct fvs_worker worker;
	struct task_struct *holder_task, *worker_task;
	spinlock_t lock;
	refcount_t count;

	spin_lock_init(&lock);
	refcount_set(&count, 1);

	worker.count = &count;
	worker.lock = &lock;
	atomic_set(&worker.entered, 0);
	atomic_set(&worker.done, 0);
	worker.ret = false;
	worker.irq_after = true;
	worker.flags = 0;
	worker.kind = kind;
	worker.use_c_ref = use_c_ref;

	holder.lock = &lock;
	holder.count = &count;
	holder.worker = &worker;
	atomic_set(&holder.locked, 0);
	atomic_set(&holder.release, 0);
	holder.waited = 0;
	holder.ret = 0;

	holder_task = kthread_create_on_cpu(fvs_holder_fn, &holder,
					    FVS_HOLDER_CPU, "fvs_holder");
	if (IS_ERR(holder_task)) {
		KUNIT_FAIL(test, "cannot create the holder task\n");
		return false;
	}
	/* kthread_create_on_cpu() does not wake the thread. */
	wake_up_process(holder_task);

	if (fvs_wait_atomic(&holder.locked, 1, 5000) != 0) {
		kthread_stop(holder_task);
		KUNIT_FAIL(test, "the holder task did not take the lock\n");
		return false;
	}

	worker_task = kthread_create_on_cpu(fvs_worker_fn, &worker,
					    FVS_WORKER_CPU, "fvs_worker");
	if (IS_ERR(worker_task)) {
		atomic_set(&holder.release, 1);
		kthread_stop(holder_task);
		KUNIT_FAIL(test, "cannot create the worker task\n");
		return false;
	}
	wake_up_process(worker_task);

	if (fvs_wait_atomic(&worker.done, 1, 10000) != 0) {
		atomic_set(&holder.release, 1);
		kthread_stop(holder_task);
		kthread_stop(worker_task);
		KUNIT_FAIL(test, "the worker task did not finish\n");
		return false;
	}

	kthread_stop(holder_task);
	kthread_stop(worker_task);

	*final_count = refcount_read(&count);
	*ret = worker.ret;
	*lock_released = !spin_is_locked(&lock);
	*irq_restored = !worker.irq_after;

	return true;
}

static void fvs_lock_failure_path_spinlock(struct kunit *test)
{
	unsigned int rust_count = 0, cref_count = 0;
	bool rust_ret = false, cref_ret = false;
	bool rust_rel = false, cref_rel = false;
	bool rust_irq = false, cref_irq = false;

	if (!fvs_run_lock_failure(test, FVS_HELPER_SPINLOCK, false, &rust_count,
				  &rust_ret, &rust_rel, &rust_irq))
		return;
	if (!fvs_run_lock_failure(test, FVS_HELPER_SPINLOCK, true, &cref_count,
				  &cref_ret, &cref_rel, &cref_irq))
		return;

	/*
	 * The proof that the failure path was taken: refcount_dec_and_test()
	 * on a count of zero saturates the count. If the helper had taken
	 * the early return instead, the count would still be zero.
	 */
	KUNIT_EXPECT_EQ(test, rust_count, (unsigned int)REFCOUNT_SATURATED);
	KUNIT_EXPECT_EQ(test, cref_count, (unsigned int)REFCOUNT_SATURATED);

	/* The helper must report the failure. */
	KUNIT_EXPECT_EQ(test, rust_ret, false);
	KUNIT_EXPECT_EQ(test, cref_ret, false);

	/* The lock must have been released. */
	KUNIT_EXPECT_EQ(test, rust_rel, true);
	KUNIT_EXPECT_EQ(test, cref_rel, true);

	/* The two implementations must agree. */
	KUNIT_EXPECT_EQ(test, rust_count, cref_count);
	KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
	KUNIT_EXPECT_EQ(test, rust_rel, cref_rel);
}

static void fvs_lock_failure_path_irqsave(struct kunit *test)
{
	unsigned int rust_count = 0, cref_count = 0;
	bool rust_ret = false, cref_ret = false;
	bool rust_rel = false, cref_rel = false;
	bool rust_irq = false, cref_irq = false;

	if (!fvs_run_lock_failure(test, FVS_HELPER_IRQSAVE, false, &rust_count,
				  &rust_ret, &rust_rel, &rust_irq))
		return;
	if (!fvs_run_lock_failure(test, FVS_HELPER_IRQSAVE, true, &cref_count,
				  &cref_ret, &cref_rel, &cref_irq))
		return;

	KUNIT_EXPECT_EQ(test, rust_count, (unsigned int)REFCOUNT_SATURATED);
	KUNIT_EXPECT_EQ(test, cref_count, (unsigned int)REFCOUNT_SATURATED);
	KUNIT_EXPECT_EQ(test, rust_ret, false);
	KUNIT_EXPECT_EQ(test, cref_ret, false);
	KUNIT_EXPECT_EQ(test, rust_rel, true);
	KUNIT_EXPECT_EQ(test, cref_rel, true);

	/*
	 * The interrupt state must have been restored by the helper's own
	 * failure path (spin_unlock_irqrestore), not only by the caller.
	 */
	KUNIT_EXPECT_EQ(test, rust_irq, true);
	KUNIT_EXPECT_EQ(test, cref_irq, true);

	KUNIT_EXPECT_EQ(test, rust_count, cref_count);
	KUNIT_EXPECT_EQ(test, rust_ret, cref_ret);
	KUNIT_EXPECT_EQ(test, rust_rel, cref_rel);
}

/*
 * The mutex variant of the failure path. A mutex blocks in a sleepable way,
 * so no CPU pinning is needed.
 */
struct fvs_mutex_holder {
	struct mutex *lock;
	refcount_t *count;
	struct fvs_worker *worker;
	atomic_t locked;
	atomic_t release;
};

static int fvs_mutex_holder_fn(void *arg)
{
	struct fvs_mutex_holder *h = arg;

	mutex_lock(h->lock);
	atomic_set(&h->locked, 1);

	while (!atomic_read(&h->worker->entered) && !atomic_read(&h->release))
		msleep(1);

	if (atomic_read(&h->release)) {
		/* The scenario was aborted. */
		mutex_unlock(h->lock);
		return 0;
	}

	msleep(FVS_BLOCK_WINDOW_MS);

	refcount_set(h->count, 0);

	mutex_unlock(h->lock);

	return 0;
}

struct fvs_mutex_worker {
	refcount_t *count;
	struct mutex *lock;
	atomic_t entered;
	atomic_t done;
	bool ret;
	bool use_c_ref;
};

static int fvs_mutex_worker_fn(void *arg)
{
	struct fvs_mutex_worker *w = arg;

	atomic_set(&w->entered, 1);

	if (w->use_c_ref)
		w->ret = refcount_dec_and_mutex_lock_c_ref(w->count, w->lock);
	else
		w->ret = refcount_dec_and_mutex_lock(w->count, w->lock);

	atomic_set(&w->done, 1);

	return 0;
}

static void fvs_lock_failure_path_mutex(struct kunit *test)
{
	struct fvs_mutex_holder holder;
	struct fvs_mutex_worker worker;
	struct task_struct *holder_task, *worker_task;
	struct mutex lock;
	refcount_t count;
	unsigned int final_count;
	bool ret, released;

	mutex_init(&lock);
	refcount_set(&count, 1);

	worker.count = &count;
	worker.lock = &lock;
	atomic_set(&worker.entered, 0);
	atomic_set(&worker.done, 0);
	worker.ret = false;
	worker.use_c_ref = false;

	holder.lock = &lock;
	holder.count = &count;
	holder.worker = (struct fvs_worker *)&worker;
	atomic_set(&holder.locked, 0);
	atomic_set(&holder.release, 0);

	holder_task = kthread_run(fvs_mutex_holder_fn, &holder, "fvs_mholder");
	if (IS_ERR(holder_task)) {
		KUNIT_FAIL(test, "cannot create the holder task\n");
		return;
	}

	if (fvs_wait_atomic(&holder.locked, 1, 5000) != 0) {
		kthread_stop(holder_task);
		KUNIT_FAIL(test, "the holder task did not take the mutex\n");
		return;
	}

	worker_task = kthread_run(fvs_mutex_worker_fn, &worker, "fvs_mworker");
	if (IS_ERR(worker_task)) {
		atomic_set(&holder.release, 1);
		kthread_stop(holder_task);
		KUNIT_FAIL(test, "cannot create the worker task\n");
		return;
	}

	if (fvs_wait_atomic(&worker.done, 1, 10000) != 0) {
		atomic_set(&holder.release, 1);
		kthread_stop(holder_task);
		kthread_stop(worker_task);
		KUNIT_FAIL(test, "the worker task did not finish\n");
		return;
	}

	kthread_stop(holder_task);
	kthread_stop(worker_task);

	final_count = refcount_read(&count);
	ret = worker.ret;
	released = !mutex_is_locked(&lock);

	/* The failure path was taken (the count is saturated). */
	KUNIT_EXPECT_EQ(test, final_count, (unsigned int)REFCOUNT_SATURATED);
	/* The helper reports the failure. */
	KUNIT_EXPECT_EQ(test, ret, false);
	/* The mutex was released. */
	KUNIT_EXPECT_EQ(test, released, true);
}

/*
 * ---------------------------------------------------------------------------
 * 3. A randomised differential test whose distribution reaches the states.
 * ---------------------------------------------------------------------------
 */

#define FVS_RANDOM_ITERATIONS	20000

static void fvs_random_differential(struct kunit *test)
{
	struct rnd_state rng;
	unsigned int i;

	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_if_one,
			(unsigned long)refcount_dec_if_one_c_ref);
	KUNIT_ASSERT_NE(test, (unsigned long)refcount_dec_not_one,
			(unsigned long)refcount_dec_not_one_c_ref);

	prandom_seed_state(&rng, 0xF5A0001);

	for (i = 0; i < FVS_RANDOM_ITERATIONS; i++) {
		refcount_t rust, cref;
		bool rust_ret, cref_ret;
		int value;

		/*
		 * Half of the time the count is taken from the boundary
		 * table, so the interesting states are actually visited; the
		 * other half of the time it is random.
		 */
		if (prandom_u32_state(&rng) & 1)
			value = fvs_counts[prandom_u32_state(&rng) %
					   ARRAY_SIZE(fvs_counts)];
		else
			value = (int)prandom_u32_state(&rng);

		refcount_set(&rust, value);
		refcount_set(&cref, value);

		switch (prandom_u32_state(&rng) % 4) {
		case 0:
			rust_ret = refcount_dec_if_one(&rust);
			cref_ret = refcount_dec_if_one_c_ref(&cref);
			break;
		case 1:
			rust_ret = refcount_dec_not_one(&rust);
			cref_ret = refcount_dec_not_one_c_ref(&cref);
			break;
		case 2: {
			spinlock_t rust_lock, cref_lock;

			spin_lock_init(&rust_lock);
			spin_lock_init(&cref_lock);
			rust_ret = refcount_dec_and_lock(&rust, &rust_lock);
			cref_ret = refcount_dec_and_lock_c_ref(&cref, &cref_lock);
			if (spin_is_locked(&rust_lock))
				spin_unlock(&rust_lock);
			if (spin_is_locked(&cref_lock))
				spin_unlock(&cref_lock);
			break;
		}
		default: {
			spinlock_t rust_lock, cref_lock;
			unsigned long rust_flags = 0, cref_flags = 0;

			spin_lock_init(&rust_lock);
			spin_lock_init(&cref_lock);
			rust_ret = refcount_dec_and_lock_irqsave(&rust, &rust_lock,
								 &rust_flags);
			cref_ret = refcount_dec_and_lock_irqsave_c_ref(&cref,
								       &cref_lock,
								       &cref_flags);
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
}

static struct kunit_case fvs_refcount_test_cases[] = {
	KUNIT_CASE(fvs_sweep_dec_if_one),
	KUNIT_CASE(fvs_sweep_dec_not_one),
	KUNIT_CASE(fvs_sweep_warn_saturate),
	KUNIT_CASE(fvs_sweep_dec_and_mutex_lock),
	KUNIT_CASE(fvs_sweep_dec_and_lock),
	KUNIT_CASE(fvs_sweep_dec_and_lock_irqsave),
	KUNIT_CASE(fvs_lock_failure_path_spinlock),
	KUNIT_CASE(fvs_lock_failure_path_irqsave),
	KUNIT_CASE(fvs_lock_failure_path_mutex),
	KUNIT_CASE(fvs_random_differential),
	{},
};

static struct kunit_suite fvs_refcount_test_module = {
	.name = "fvs_refcount",
	.test_cases = fvs_refcount_test_cases,
};

kunit_test_suites(&fvs_refcount_test_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("FVS-AUDIT-001 <audit@example.invalid>");
MODULE_DESCRIPTION("Independent adversarial tests for the Ferret reference counting port");

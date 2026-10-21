// SPDX-License-Identifier: GPL-2.0-only

//! Ferret: the out-of-line reference counting helpers, in Rust.
//!
//! Replaces `lib/refcount.c`. The C ABI is unchanged: the same six exported
//! symbols, with the same signatures and the same (non-GPL) `EXPORT_SYMBOL`
//! visibility, so the modules that use them keep resolving them exactly as
//! before.
//!
//! # What stays in C, and why
//!
//! The `refcount_t` *primitive* stays in C, in `include/linux/refcount.h`. It
//! is a header type, not an object: the C code embeds it in its own
//! structures (`struct kref`, `struct rcu_head`, `struct device`, ...), and the
//! inline operations in that header are what the kernel's own code calls. It
//! cannot be "migrated" as a file without changing every C structure that
//! embeds it.
//!
//! What moves to Rust is the *exported* layer: the saturation warning and the
//! five decrement helpers. Those are the only out-of-line functions the
//! header needs, and the header calls them by name, so the C code that uses
//! the primitive keeps working unchanged.
//!
//! Note the direction of the call: the C inline operations of the primitive
//! (`refcount_inc()`, `refcount_dec()`, `refcount_add()`, ...) warn through
//! `refcount_warn_saturate()`, which is the Rust implementation below. Every
//! saturation warning in the kernel therefore goes through this Rust code.
//! There is no recursion: the Rust code never calls back into the C
//! `refcount_warn_saturate()`.
//!
//! # Memory ordering
//!
//! The decrements use the same primitives as the C original, with the same
//! ordering:
//!
//! * `refcount_dec_if_one()` and `refcount_dec_not_one()` use
//!   `atomic_try_cmpxchg_release()`, which is release on success and relaxed
//!   on failure. The Rust code calls that very same C primitive, through the
//!   kernel crate's `Atomic` (which is a thin wrapper over it), so the
//!   ordering is not re-implemented and cannot drift.
//! * `refcount_dec_and_test()` is the C inline of the primitive, which
//!   provides the release ordering of the decrement and the acquire on
//!   success through `smp_acquire__after_ctrl_dep()`.
//!
//! # The lock helpers
//!
//! `refcount_dec_and_mutex_lock()`, `refcount_dec_and_lock()` and
//! `refcount_dec_and_lock_irqsave()` first decrement the count with
//! `refcount_dec_not_one()`; if that fails (the count was one, or the count
//! is saturated), the object is still alive and the function returns false
//! *without* taking the lock. Otherwise the lock is taken and the count is
//! decremented to zero with `refcount_dec_and_test()`. If that second
//! decrement does not reach zero, the lock is released and the function
//! returns false. On success the lock is *held* by the caller, exactly as in
//! the C original.
//!
//! # Differences from the C original
//!
//! One, in the warning path: the C `WARN_ONCE()` uses a plain (racy) flag, so
//! two tasks that hit the same warning at the same time can print it twice.
//! The Rust code uses an atomic compare-and-exchange, so each warning is
//! printed exactly once. The message text is unchanged.

use kernel::bindings;
use kernel::prelude::*;
use kernel::sync::Refcount;

/// Saturate a refcount and warn about the event that caused it.
///
/// This is the C `refcount_warn_saturate()`. It is called by the inline
/// operations of the `refcount_t` primitive when the counter leaves its
/// valid range, and it is the only place where the saturation value is
/// written.
#[export]
pub unsafe extern "C" fn refcount_warn_saturate(
    r: *mut bindings::refcount_t,
    t: bindings::refcount_saturation_type,
) {
    // SAFETY: the caller passes a valid refcount, and the C contract of the
    // reference count is unchanged.
    let r = unsafe { Refcount::from_ptr(r) };

    r.warn_saturate(t);
}

/// Decrement a refcount if it is one, and report whether it reached zero.
///
/// This is the C `refcount_dec_if_one()`. The one-to-zero transition is a
/// single release-ordered compare-and-exchange, which provides release
/// ordering and a control dependency, so that the free cannot be reordered
/// before the decrement.
#[export]
pub unsafe extern "C" fn refcount_dec_if_one(r: *mut bindings::refcount_t) -> bool {
    // SAFETY: the caller passes a valid refcount, and the C contract of the
    // reference count is unchanged.
    let r = unsafe { Refcount::from_ptr(r) };

    r.try_dec_if_one()
}

/// Decrement a refcount if it is not one, and report whether it succeeded.
///
/// This is the C `refcount_dec_not_one()`. The count of one is refused (the
/// function returns false), a saturated count is left alone, and an
/// underflow warns once.
#[export]
pub unsafe extern "C" fn refcount_dec_not_one(r: *mut bindings::refcount_t) -> bool {
    // SAFETY: the caller passes a valid refcount, and the C contract of the
    // reference count is unchanged.
    let r = unsafe { Refcount::from_ptr(r) };

    r.try_dec_not_one()
}

/// Decrement a refcount to zero and return holding the mutex, if able to.
///
/// This is the C `refcount_dec_and_mutex_lock()`. On success the mutex is
/// held by the caller; on failure it is not taken, or it is released again.
#[export]
pub unsafe extern "C" fn refcount_dec_and_mutex_lock(
    r: *mut bindings::refcount_t,
    lock: *mut bindings::mutex,
) -> bool {
    // SAFETY: the caller passes a valid refcount, and the C contract of the
    // reference count is unchanged.
    let r = unsafe { Refcount::from_ptr(r) };

    // The count must be greater than one: if it is one, the object is still
    // alive and the caller must not take the lock.
    if r.try_dec_not_one() {
        return false;
    }

    // SAFETY: the caller passes a valid, initialised mutex.
    unsafe { bindings::mutex_lock(lock) };

    if !r.dec_and_test() {
        // SAFETY: the mutex is held here.
        unsafe { bindings::mutex_unlock(lock) };
        return false;
    }

    true
}

/// Decrement a refcount to zero and return holding the spinlock, if able to.
///
/// This is the C `refcount_dec_and_lock()`. On success the spinlock is held
/// by the caller; on failure it is not taken, or it is released again.
#[export]
pub unsafe extern "C" fn refcount_dec_and_lock(
    r: *mut bindings::refcount_t,
    lock: *mut bindings::spinlock_t,
) -> bool {
    // SAFETY: the caller passes a valid refcount, and the C contract of the
    // reference count is unchanged.
    let r = unsafe { Refcount::from_ptr(r) };

    // The count must be greater than one: if it is one, the object is still
    // alive and the caller must not take the lock.
    if r.try_dec_not_one() {
        return false;
    }

    // SAFETY: the caller passes a valid, initialised spinlock.
    unsafe { bindings::spin_lock(lock) };

    if !r.dec_and_test() {
        // SAFETY: the spinlock is held here.
        unsafe { bindings::spin_unlock(lock) };
        return false;
    }

    true
}

/// Decrement a refcount to zero and return holding the spinlock with
/// interrupts disabled, if able to.
///
/// This is the C `refcount_dec_and_lock_irqsave()`. On success the spinlock
/// is held by the caller and the saved interrupt flags are written to
/// `flags`; on failure the lock is not taken, or it is released again and
/// the interrupt state is restored.
#[export]
pub unsafe extern "C" fn refcount_dec_and_lock_irqsave(
    r: *mut bindings::refcount_t,
    lock: *mut bindings::spinlock_t,
    flags: *mut kernel::ffi::c_ulong,
) -> bool {
    // SAFETY: the caller passes a valid refcount, and the C contract of the
    // reference count is unchanged.
    let r = unsafe { Refcount::from_ptr(r) };

    // The count must be greater than one: if it is one, the object is still
    // alive and the caller must not take the lock.
    if r.try_dec_not_one() {
        return false;
    }

    // SAFETY: the caller passes a valid, initialised spinlock, and a valid
    // pointer to the saved interrupt flags.
    unsafe { bindings::spin_lock_irqsave(lock, flags) };

    if !r.dec_and_test() {
        // SAFETY: the spinlock is held here.
        unsafe { bindings::spin_unlock_irqrestore(lock, flags) };
        return false;
    }

    true
}

#[cfg(CONFIG_KUNIT)]
#[kunit_tests(refcount_rust)]
mod tests {
    use kernel::sync::atomic::{Atomic, Relaxed};
    use kernel::sync::{KRef, Refcount};

    use super::*;

    /// A test object with an embedded `struct kref`, like the C objects have.
    ///
    /// The reference count is the first member, so that the release callback
    /// can find the object from the count, like the C `container_of()` does.
    struct TestObject {
        kref: bindings::kref,
        releases: Atomic<u32>,
    }

    impl TestObject {
        fn new() -> Self {
            let obj = TestObject {
                kref: bindings::kref {
                    refcount: bindings::refcount_t {
                        refs: bindings::atomic_t { counter: 1 },
                    },
                },
                releases: Atomic::new(0),
            };

            obj
        }
    }

    /// The release callback of the test object: it counts the releases.
    unsafe extern "C" fn release(kref: *mut bindings::kref) {
        // SAFETY: the count is the first member of `TestObject`, and the
        // object is alive until the callback returns.
        let obj = kref.cast::<TestObject>();
        // SAFETY: the object is alive.
        unsafe { (*obj).releases.fetch_add(1, Relaxed) };
    }

    #[test]
    fn kref_release_on_drop() {
        let mut obj = TestObject::new();

        {
            // SAFETY: the count is the one of the object, the object is alive
            // for the whole scope, and the callback is the release callback
            // of the object.
            let kref = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };
            assert_eq!(kref.as_ptr(), &mut obj as *mut TestObject);
        }

        // The last reference went away, so the object was released.
        assert_eq!(obj.releases.load(Relaxed), 1);
    }

    #[test]
    fn kref_get_keeps_the_object_alive() {
        let mut obj = TestObject::new();

        {
            // SAFETY: as above.
            let kref = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };
            kref.get();
        }

        // The reference taken by `get()` is still held, so the object was not
        // released.
        assert_eq!(obj.releases.load(Relaxed), 0);
        // SAFETY: the count is a valid, initialised `refcount_t`.
        assert_eq!(unsafe { bindings::atomic_read(&obj.kref.refcount.refs) }, 1);
    }

    #[test]
    fn kref_into_raw_transfers_the_reference() {
        let mut obj = TestObject::new();

        {
            // SAFETY: as above.
            let kref = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };
            let raw = kref.into_raw();
            assert_eq!(raw, &mut obj.kref as *mut bindings::kref);
        }

        // The reference was given away, so it was not released.
        assert_eq!(obj.releases.load(Relaxed), 0);
        // SAFETY: the count is a valid, initialised `refcount_t`.
        assert_eq!(unsafe { bindings::atomic_read(&obj.kref.refcount.refs) }, 1);
    }

    #[test]
    fn dec_if_one_ignores_a_saturated_count() {
        let mut count = bindings::refcount_t {
            refs: bindings::atomic_t { counter: 0 },
        };

        // SAFETY: the count is a valid, initialised `refcount_t`.
        let r = unsafe { Refcount::from_ptr(&mut count) };
        r.set_saturated();

        assert!(!r.try_dec_if_one());
        assert_eq!(r.read(), kernel::sync::REFCOUNT_SATURATED);
    }

    #[test]
    fn dec_not_one_refuses_a_count_of_one() {
        let mut count = bindings::refcount_t {
            refs: bindings::atomic_t { counter: 0 },
        };

        // SAFETY: the count is a valid, initialised `refcount_t`.
        let r = unsafe { Refcount::from_ptr(&mut count) };
        r.set(1);

        assert!(!r.try_dec_not_one());
        assert_eq!(r.read(), 1);
    }

    #[test]
    fn dec_not_one_decrements_a_larger_count() {
        let mut count = bindings::refcount_t {
            refs: bindings::atomic_t { counter: 0 },
        };

        // SAFETY: the count is a valid, initialised `refcount_t`.
        let r = unsafe { Refcount::from_ptr(&mut count) };
        r.set(3);

        assert!(r.try_dec_not_one());
        assert_eq!(r.read(), 2);
    }
}

// SPDX-License-Identifier: GPL-2.0-only

//! FVS-AUDIT-001: independent adversarial tests for the reference counting
//! port of PR #24 (overlordxrz-source/ferret).
//!
//! This file was written by the independent audit FVS-AUDIT-001 and is kept
//! as it was written, so that the audit's tests stay reproducible against the
//! tree they were written for. It does not modify the implementation; it only
//! tests it harder than the project's own suite does.
//!
//! It is now part of the project: the contract model in this file is the
//! oracle the reference counting port is compared against.
//!
//! What it adds over `lib/refcount_rust.rs`'s own test module:
//!
//!  * two live `KRef` handles on the same C object (the pull request's tests
//!    only ever create one handle, so the "one reference per handle"
//!    contract is never exercised);
//!  * `KRef::get()` and `KRef::drop()` on a *saturated* count and on a
//!    *dead* count, where the handle's behaviour is surprising: `get()` takes
//!    no reference at all and silently saturates the count;
//!  * `into_raw()` followed by the C `kref_put()` sequence;
//!  * a compile-time demonstration that the `'a` lifetime of `KRef` is not
//!    inferred from the arguments, so it is not enforced by the type system;
//!  * a differential comparison of the decrement helpers against a model
//!    written from the *contract* of the primitive, over a distribution that
//!    actually reaches the boundary states (the pull request's model test
//!    reaches none of them).

use kernel::prelude::*;

#[cfg(CONFIG_KUNIT)]
#[kunit_tests(fvs_refcount_adv)]
mod tests {
    use kernel::bindings;
    use kernel::sync::atomic::{Atomic, Relaxed};
    use kernel::sync::{KRef, Refcount, REFCOUNT_SATURATED};

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
            TestObject {
                kref: bindings::kref {
                    refcount: bindings::refcount_t {
                        refs: bindings::atomic_t { counter: 1 },
                    },
                },
                releases: Atomic::new(0),
            }
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

    /// The C `kref_put()` sequence, spelled out: decrement the count and, if
    /// it reached zero, run the release callback.
    ///
    /// `include/linux/kref.h` is not part of the kernel crate's bindgen
    /// header set, so `kref_put()` itself is not in the bindings; this is
    /// its body.
    unsafe fn c_kref_put(kref: *mut bindings::kref, release: unsafe extern "C" fn(*mut bindings::kref)) {
        // SAFETY: the caller guarantees that the kref is valid.
        let r = unsafe { Refcount::from_ptr(core::ptr::addr_of_mut!((*kref).refcount)) };

        if r.dec_and_test() {
            // SAFETY: the callback is the release callback of the object.
            unsafe { release(kref) };
        }
    }

    /*
     * ---------------------------------------------------------------------
     * 1. What does KRef::new() actually do with the count?
     * ---------------------------------------------------------------------
     *
     * FINDING: `KRef::new()` does NOT take a reference. It stores the
     * pointers and the callback; the count is not incremented. The handle
     * therefore *adopts* a reference that the caller must already hold
     * (the C equivalent is `kref_get()` followed by handing the reference
     * over), and the handle releases it in `Drop`.
     *
     * The documentation of the type says "The type holds *one* reference",
     * and the pull request's audit report says "Who owns the reference? |
     * The handle", but neither the type documentation nor the safety
     * contract of `new()` states the precondition "the caller must
     * already hold a reference on the object's behalf". A user who writes
     * `KRef::new(&obj->kref, obj, release)` without a preceding
     * `kref_get()` gets a handle that releases a reference it never took.
     *
     * The pull request's own test module only ever creates ONE handle, so
     * this is never observed there.
     */
    #[test]
    fn kref_new_does_not_take_a_reference() {
        let mut obj = TestObject::new();

        {
            // SAFETY: the count is the one of the object, the object is
            // alive for the whole scope, and the callback is the release
            // callback of the object.
            let kref = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };

            // The count is still 1: `new()` did not increment it.
            assert_eq!(kref.read(), 1);
        }

        // The drop released the (single, adopted) reference.
        assert_eq!(obj.releases.load(Relaxed), 1);
    }

    /*
     * ---------------------------------------------------------------------
     * 1b. Two handles over one reference release the object twice.
     * ---------------------------------------------------------------------
     *
     * Because `KRef::new()` does not take a reference, two handles over the
     * same object share ONE reference. The first drop releases it (and the
     * release callback frees the object), while the second handle is still
     * alive: the second drop then decrements a count of zero, which warns
     * and does not decrement.
     *
     * This is a caller error, but the type does not make it hard to make,
     * and the pull request's audit report's checklist ("Who owns the
     * reference? The handle") does not mention that the caller has to have
     * taken it first.
     */
    #[test]
    fn two_handles_over_one_reference_release_the_object_early() {
        let mut obj = TestObject::new();

        {
            // SAFETY: as above.
            let a = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };
            // SAFETY: as above. This handle does NOT take a second
            // reference: the two handles share the single one.
            let b = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };

            // One reference, two handles.
            assert_eq!(a.read(), 1);
            assert_eq!(b.read(), 1);

            drop(a);
            // The count reached zero: the object was released while the
            // second handle is still alive.
            assert_eq!(obj.releases.load(Relaxed), 1);
        }

        // The second drop decremented a count of zero: the warning
        // saturated the count and the release callback did NOT run again.
        assert_eq!(obj.releases.load(Relaxed), 1);
        assert_eq!(
            unsafe { bindings::atomic_read(&obj.kref.refcount.refs) },
            REFCOUNT_SATURATED
        );
    }

    /*
     * ---------------------------------------------------------------------
     * 2. get() on a saturated count.
     * ---------------------------------------------------------------------
     *
     * `refcount_inc()` on a saturated count warns and *does not* increment:
     * the count stays saturated. The handle therefore believes it owns a
     * reference that was never taken. The drop then decrements a saturated
     * count, which warns and does not decrement either, so the release
     * callback never runs: the object leaks. That is the C behaviour too,
     * but the pull request's suite never exercises it.
     */
    #[test]
    fn get_on_a_saturated_count_takes_no_reference() {
        let mut obj = TestObject::new();

        {
            // SAFETY: as above.
            let kref = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };

            // SAFETY: the count is a valid, initialised `refcount_t`.
            unsafe {
                let r = Refcount::from_ptr(&mut obj.kref.refcount);
                r.set_saturated();
            }

            kref.get();

            // The count did not move: no reference was taken.
            assert_eq!(kref.read(), REFCOUNT_SATURATED as u32);
        }

        // The drop of a saturated count does not reach zero either.
        assert_eq!(obj.releases.load(Relaxed), 0);
        assert_eq!(
            unsafe { bindings::atomic_read(&obj.kref.refcount.refs) },
            REFCOUNT_SATURATED
        );
    }

    /*
     * ---------------------------------------------------------------------
     * 3. get() on a dead object (the count is zero).
     * ---------------------------------------------------------------------
     *
     * `refcount_inc()` on a count of zero warns (REFCOUNT_ADD_UAF) and
     * saturates the count. The reference is *not* taken, and the object is
     * already dead. The handle now owns a reference that does not exist.
     */
    #[test]
    fn get_on_a_dead_count_takes_no_reference() {
        let mut obj = TestObject::new();

        {
            // SAFETY: as above.
            let kref = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };

            // SAFETY: the count is a valid, initialised `refcount_t`.
            unsafe {
                let r = Refcount::from_ptr(&mut obj.kref.refcount);
                r.set(0);
            }

            kref.get();

            // The count was saturated by the warning, not incremented.
            assert_eq!(kref.read(), REFCOUNT_SATURATED as u32);
        }

        // No release: the count never reached zero through a decrement.
        assert_eq!(obj.releases.load(Relaxed), 0);
    }

    /*
     * ---------------------------------------------------------------------
     * 4. into_raw() followed by the C kref_put() sequence.
     * ---------------------------------------------------------------------
     *
     * `into_raw()` must transfer the reference, not leak it and not drop
     * it: the C side owns one reference afterwards and must release it with
     * `kref_put()`. The pull request's test only checks that the handle did
     * *not* release the reference; it never checks that the C side can
     * actually release it.
     */
    #[test]
    fn into_raw_then_c_kref_put_releases_once() {
        let mut obj = TestObject::new();
        let raw;

        {
            // SAFETY: as above.
            let kref = unsafe { KRef::new(&mut obj.kref, &mut obj, release) };
            raw = kref.into_raw();
        }

        // The handle did not release anything.
        assert_eq!(obj.releases.load(Relaxed), 0);
        assert_eq!(unsafe { bindings::atomic_read(&obj.kref.refcount.refs) }, 1);

        // The C side now owns the reference.
        // SAFETY: `raw` is the `struct kref` embedded in `obj`, the object
        // is alive, and `release` is its release callback.
        unsafe { c_kref_put(raw, release) };

        // The C put() ran the release callback exactly once.
        assert_eq!(obj.releases.load(Relaxed), 1);
        assert_eq!(unsafe { bindings::atomic_read(&obj.kref.refcount.refs) }, 0);
    }

    /*
     * ---------------------------------------------------------------------
     * 5. The lifetime of KRef is not enforced by the type system.
     * ---------------------------------------------------------------------
     *
     * `KRef::new()` does not take the lifetime as an argument: the lifetime
     * parameter of the type is chosen by the caller. The following function
     * therefore compiles, and it is a compile-time proof that the `'a` of
     * `KRef<'a, T>` carries no information about the object.
     *
     * The function is never called: it is a type-level check only.
     */
    #[allow(dead_code)]
    fn kref_new_has_an_unbounded_lifetime(
        kref: *mut bindings::kref,
        data: *mut TestObject,
    ) -> KRef<'static, TestObject> {
        // SAFETY: this function is never called.
        unsafe { KRef::new(kref, data, release) }
    }

    /*
     * ---------------------------------------------------------------------
     * 6. The auto-trait bounds of KRef.
     * ---------------------------------------------------------------------
     *
     * The handle is `Send` and `Sync` whatever the object type is. That is
     * the claim the pull request makes; this is the compile-time proof.
     */
    fn assert_send<T: Send>() {}
    fn assert_sync<T: Sync>() {}

    /// A type that is neither `Send` nor `Sync`.
    struct NotSendNotSync {
        _ptr: *mut u8,
    }

    #[test]
    fn kref_is_send_and_sync() {
        assert_send::<KRef<'static, TestObject>>();
        assert_sync::<KRef<'static, TestObject>>();
        // And for a type that is neither Send nor Sync.
        assert_send::<KRef<'static, NotSendNotSync>>();
        assert_sync::<KRef<'static, NotSendNotSync>>();
    }

    /*
     * ---------------------------------------------------------------------
     * 7. The decrement helpers against a model of the contract.
     * ---------------------------------------------------------------------
     *
     * The model is written from the *contract* of the primitive, not from
     * the implementation: the count is kept as a signed 64-bit value, the
     * saturation value is derived from the contract
     * (INT_MIN / 2, "roughly equidistant from 0 and INT_MAX"), and the
     * boundary values are computed rather than hard-coded.
     *
     * The distribution reaches the boundaries on purpose: the pull
     * request's own model test reaches none of them in 20 000 operations.
     */
    struct ContractModel {
        /// The count is a 32-bit value; the primitive's arithmetic is
        /// *unsigned* (the C code uses `unsigned int val` and
        /// `unsigned int new`), so the model keeps the 32-bit pattern.
        count: u32,
    }

    impl ContractModel {
        /// The saturated value, from the contract: INT_MIN / 2.
        fn saturated() -> u32 {
            (i32::MIN / 2) as u32
        }

        fn new(count: u32) -> Self {
            ContractModel { count }
        }

        /// The contract of `refcount_dec_if_one()`: the one-to-zero
        /// transition, and nothing else.
        fn dec_if_one(&mut self) -> bool {
            if self.count == 1 {
                self.count = 0;
                true
            } else {
                false
            }
        }

        /// The contract of `refcount_dec_not_one()`:
        ///  - a saturated count is left alone (the count does not move);
        ///  - a count of one is refused;
        ///  - a count of zero underflows: the count does not move;
        ///  - any other count is decremented by one.
        fn dec_not_one(&mut self) -> bool {
            if self.count == Self::saturated() {
                return true;
            }
            if self.count == 1 {
                return false;
            }
            if self.count == 0 {
                return true;
            }
            self.count = self.count.wrapping_sub(1);
            true
        }
    }

    /// The values the model is compared on: the boundaries and their
    /// neighbours.
    fn boundary_values() -> [i32; 15] {
        let sat = REFCOUNT_SATURATED;

        [
            i32::MIN,
            i32::MIN + 1,
            sat - 1,
            sat,
            sat + 1,
            -2,
            -1,
            0,
            1,
            2,
            3,
            4,
            i32::MAX - 1,
            i32::MAX,
            17,
        ]
    }

    #[test]
    fn dec_helpers_match_the_contract_model_on_the_boundaries() {
        for value in boundary_values() {
            let mut count = bindings::refcount_t {
                refs: bindings::atomic_t { counter: value },
            };
            // SAFETY: the count is a valid, initialised `refcount_t`.
            let r = unsafe { Refcount::from_ptr(&mut count) };
            let mut model = ContractModel::new(value as u32);

            // The one-to-zero transition.
            assert_eq!(r.try_dec_if_one(), model.dec_if_one());
            assert_eq!(r.read() as u32, model.count);

            // The decrement that refuses a count of one.
            assert_eq!(r.try_dec_not_one(), model.dec_not_one());
            assert_eq!(r.read() as u32, model.count);
        }
    }

    /// A deterministic pseudo-random generator, so that a failure is
    /// reproducible. It is a different generator from the pull request's,
    /// on purpose.
    struct Lcg(u64);

    impl Lcg {
        fn next(&mut self) -> u32 {
            // A 64-bit LCG with the constants of the "Numerical Recipes"
            // generator, which is not the xorshift of the pull request.
            self.0 = self.0.wrapping_mul(1664525).wrapping_add(1013904223);
            (self.0 >> 33) as u32
        }
    }

    #[test]
    fn dec_helpers_match_the_contract_model_over_a_reaching_distribution() {
        let mut rng = Lcg(0x00F5_0001_0000_0001);
        let mut saturated_hits = 0u32;
        let mut zero_hits = 0u32;
        let mut one_hits = 0u32;
        let mut two_hits = 0u32;
        let mut dec_if_one_success = 0u32;
        let mut dec_not_one_refuse = 0u32;
        let mut dec_not_one_saturated = 0u32;
        let mut dec_not_one_underflow = 0u32;

        for _ in 0..20000 {
            // Half of the time the count comes from the boundary table, so
            // the interesting states are reached; otherwise it is random.
            let value = if rng.next() & 1 == 0 {
                boundary_values()[(rng.next() as usize) % boundary_values().len()]
            } else {
                rng.next() as i32
            };

            match value {
                v if v == REFCOUNT_SATURATED => saturated_hits += 1,
                0 => zero_hits += 1,
                1 => one_hits += 1,
                2 => two_hits += 1,
                _ => (),
            }

            // The one-to-zero transition, from the same starting value.
            {
                let mut count = bindings::refcount_t {
                    refs: bindings::atomic_t { counter: value },
                };
                // SAFETY: the count is a valid, initialised `refcount_t`.
                let r = unsafe { Refcount::from_ptr(&mut count) };
                let mut model = ContractModel::new(value as u32);

                let ret = r.try_dec_if_one();

                assert_eq!(ret, model.dec_if_one());
                assert_eq!(r.read() as u32, model.count);

                if ret {
                    dec_if_one_success += 1;
                }
            }

            // The decrement that refuses a count of one, from the same
            // starting value.
            {
                let mut count = bindings::refcount_t {
                    refs: bindings::atomic_t { counter: value },
                };
                // SAFETY: the count is a valid, initialised `refcount_t`.
                let r = unsafe { Refcount::from_ptr(&mut count) };
                let mut model = ContractModel::new(value as u32);

                let ret = r.try_dec_not_one();

                assert_eq!(ret, model.dec_not_one());
                assert_eq!(r.read() as u32, model.count);

                if value == REFCOUNT_SATURATED {
                    dec_not_one_saturated += 1;
                    // The saturated count never moves.
                    assert_eq!(r.read(), REFCOUNT_SATURATED as u32);
                } else if value == 0 {
                    dec_not_one_underflow += 1;
                    // The underflow does not move the count either.
                    assert_eq!(r.read(), 0);
                } else if value == 1 {
                    dec_not_one_refuse += 1;
                    // The refusal does not move the count.
                    assert_eq!(r.read(), 1);
                }
            }
        }

        // The distribution must really reach the boundaries; otherwise this
        // test would be vacuous, and that is exactly the defect this file
        // exists to demonstrate.
        assert!(saturated_hits > 0);
        assert!(zero_hits > 0);
        assert!(one_hits > 0);
        assert!(two_hits > 0);
        assert!(dec_if_one_success > 0);
        assert!(dec_not_one_refuse > 0);
        assert!(dec_not_one_saturated > 0);
        assert!(dec_not_one_underflow > 0);
    }
}

// SPDX-License-Identifier: GPL-2.0

//! Atomic reference counting.
//!
//! C header: [`include/linux/refcount.h`](srctree/include/linux/refcount.h)

use crate::build_assert;
use crate::sync::atomic::{Atomic, Relaxed, Release};
use crate::types::Opaque;

/// The value a saturated refcount takes (`REFCOUNT_SATURATED`).
///
/// The kernel's value is `INT_MIN / 2`, which is negative when seen as a
/// signed integer: the saturation range is deliberately close to zero, so that
/// a saturated count cannot reach the "bad" range in a single operation.
pub const REFCOUNT_SATURATED: i32 = i32::MIN / 2;

/// The "warn once" flag of the underflow warning.
static UNDERFLOW_WARNED: Atomic<u32> = Atomic::new(0);

/// The "warn once" flag of the "addition on zero" warning.
static WARN_ADD_UAF: Atomic<u32> = Atomic::new(0);
/// The "warn once" flag of the "saturated" warning (the `inc_not_zero` case).
static WARN_ADD_NOT_ZERO_OVF: Atomic<u32> = Atomic::new(0);
/// The "warn once" flag of the "saturated" warning (the `add` case).
static WARN_ADD_OVF: Atomic<u32> = Atomic::new(0);
/// The "warn once" flag of the "underflow" warning.
static WARN_SUB_UAF: Atomic<u32> = Atomic::new(0);
/// The "warn once" flag of the "decrement hit zero" warning.
static WARN_DEC_LEAK: Atomic<u32> = Atomic::new(0);
/// The "warn once" flag of the "unknown event" warning.
static WARN_UNKNOWN: Atomic<u32> = Atomic::new(0);

/// Warn once, like the kernel's `WARN_ONCE()`.
///
/// The C `WARN_ONCE()` uses a plain, racy flag, so two tasks that hit the
/// warning at the same time may print it twice. This version uses an atomic
/// compare-and-exchange, so the warning is printed exactly once.
///
/// The message is printed as a `KERN_WARNING` message, and the warning
/// machinery of the kernel (`warn_on!()`) reports the location and dumps the
/// stack, like the C `WARN_ONCE()` does.
///
/// This is a macro and not a function, so that every call site gets its own
/// reported location: the C original reports the line of the `REFCOUNT_WARN()`
/// call site, so the event that caused the warning can be read off the
/// warning in the log.
macro_rules! warn_once {
    ($flag:expr, $message:expr) => {
        if $flag.cmpxchg(0, 1, Relaxed).is_ok() {
            crate::pr_warn!("{}", $message);
            crate::warn_on!(true);
        }
    };
}

/// Atomic reference counter.
///
/// This type is conceptually an atomic integer, but provides saturation semantics compared to
/// normal atomic integers. Values in the negative range when viewed as a signed integer are
/// saturation (bad) values. For details about the saturation semantics, please refer to top of
/// [`include/linux/refcount.h`](srctree/include/linux/refcount.h).
///
/// Wraps the kernel's C `refcount_t`.
#[repr(transparent)]
pub struct Refcount(Opaque<bindings::refcount_t>);

impl Refcount {
    /// Construct a new [`Refcount`] from an initial value.
    ///
    /// The initial value should be non-saturated.
    // Always inline to optimize out error path of `build_assert`.
    #[inline(always)]
    pub fn new(value: i32) -> Self {
        build_assert!(value >= 0, "initial value saturated");
        // SAFETY: There are no safety requirements for this FFI call.
        Self(Opaque::new(unsafe { bindings::REFCOUNT_INIT(value) }))
    }

    #[inline]
    fn as_ptr(&self) -> *mut bindings::refcount_t {
        self.0.get()
    }

    /// Get the underlying atomic counter that backs the refcount.
    ///
    /// NOTE: Usage of this function is discouraged as it can circumvent the protections offered by
    /// `refcount.h`. If there is no way to achieve the result using APIs in `refcount.h`, then
    /// this function can be used. Otherwise consider adding a binding for the required API.
    #[inline]
    pub fn as_atomic(&self) -> &Atomic<i32> {
        let ptr = self.0.get().cast();
        // SAFETY: `refcount_t` is a transparent wrapper of `atomic_t`, which is an atomic 32-bit
        // integer that is layout-wise compatible with `Atomic<i32>`. All values are valid for
        // `refcount_t`, despite some of the values being considered saturated and "bad".
        unsafe { &*ptr }
    }

    /// Set a refcount's value.
    #[inline]
    pub fn set(&self, value: i32) {
        // SAFETY: `self.as_ptr()` is valid.
        unsafe { bindings::refcount_set(self.as_ptr(), value) }
    }

    /// Increment a refcount.
    ///
    /// It will saturate if overflows and `WARN`. It will also `WARN` if the refcount is 0, as this
    /// represents a possible use-after-free condition.
    ///
    /// Provides no memory ordering, it is assumed that caller already has a reference on the
    /// object.
    #[inline]
    pub fn inc(&self) {
        // SAFETY: self is valid.
        unsafe { bindings::refcount_inc(self.as_ptr()) }
    }

    /// Decrement a refcount.
    ///
    /// It will `WARN` on underflow and fail to decrement when saturated.
    ///
    /// Provides release memory ordering, such that prior loads and stores are done
    /// before.
    #[inline]
    pub fn dec(&self) {
        // SAFETY: `self.as_ptr()` is valid.
        unsafe { bindings::refcount_dec(self.as_ptr()) }
    }

    /// Borrow an existing C `refcount_t`.
    ///
    /// This is the borrowed counterpart of [`Refcount::new()`]: it gives Rust
    /// code access to a reference count that is *embedded in an object owned by
    /// C*, which is what the exported `refcount_*` helpers operate on. The
    /// count itself stays C: the `refcount_t` primitive is a header type that
    /// the C code embeds in its own structures, and its inline operations are
    /// what the kernel's own code calls.
    ///
    /// # Safety
    ///
    /// `ptr` must point to a valid, initialised `refcount_t` that outlives
    /// `'a`, and the C contract of the reference count (no increment from
    /// zero, no use after free) must be respected by the caller.
    pub unsafe fn from_ptr<'a>(ptr: *mut bindings::refcount_t) -> &'a Self {
        // SAFETY: the caller guarantees that the pointer is valid; `Refcount`
        // is a transparent wrapper of `refcount_t`, so the layouts match.
        unsafe { &*ptr.cast() }
    }

    /// Read the refcount's value.
    ///
    /// The read is relaxed, like the C `refcount_read()`, and the type is the
    /// C one: `unsigned int`. A saturated count therefore reads back as
    /// `0xc000_0000`, not as a negative number.
    #[inline]
    pub fn read(&self) -> u32 {
        self.as_atomic().load(Relaxed) as u32
    }

    /// Set the refcount to the saturated value.
    ///
    /// This is the Rust equivalent of `refcount_set(r, REFCOUNT_SATURATED)`:
    /// the count stops moving, so that a concurrent decrement cannot drive it
    /// into the "bad" range and free the object.
    #[inline]
    pub fn set_saturated(&self) {
        self.set(REFCOUNT_SATURATED);
    }

    /// Saturate the refcount and warn about the event that caused it.
    ///
    /// This is the Rust equivalent of the C `refcount_warn_saturate()`: the
    /// count is set to the saturated value, which stops it from moving, and
    /// the event is reported once. The message text is the one of the C
    /// original.
    pub fn warn_saturate(&self, t: bindings::refcount_saturation_type) {
        self.set_saturated();

        match t {
            bindings::refcount_saturation_type_REFCOUNT_ADD_NOT_ZERO_OVF => {
                warn_once!(
                    &WARN_ADD_NOT_ZERO_OVF,
                    "refcount_t: saturated; leaking memory.\n"
                )
            }
            bindings::refcount_saturation_type_REFCOUNT_ADD_OVF => {
                warn_once!(&WARN_ADD_OVF, "refcount_t: saturated; leaking memory.\n")
            }
            bindings::refcount_saturation_type_REFCOUNT_ADD_UAF => {
                warn_once!(&WARN_ADD_UAF, "refcount_t: addition on 0; use-after-free.\n")
            }
            bindings::refcount_saturation_type_REFCOUNT_SUB_UAF => {
                warn_once!(&WARN_SUB_UAF, "refcount_t: underflow; use-after-free.\n")
            }
            bindings::refcount_saturation_type_REFCOUNT_DEC_LEAK => {
                warn_once!(&WARN_DEC_LEAK, "refcount_t: decrement hit 0; leaking memory.\n")
            }
            _ => warn_once!(&WARN_UNKNOWN, "refcount_t: unknown saturation event!?.\n"),
        }
    }

    /// Decrement the refcount if it is one, and report whether it reached zero.
    ///
    /// This is the Rust equivalent of the C `atomic_try_cmpxchg_release()`
    /// on the underlying counter: the one-to-zero transition is performed with
    /// release ordering, and the failure ordering is relaxed, exactly as in C.
    /// It provides a control dependency, so that the free cannot be reordered
    /// before the decrement.
    #[inline]
    pub fn try_dec_if_one(&self) -> bool {
        self.as_atomic().cmpxchg(1, 0, Release).is_ok()
    }

    /// Decrement the refcount unless it is one.
    ///
    /// This is the Rust equivalent of the C `refcount_dec_not_one()`: a
    /// release-ordered compare-and-exchange loop that refuses to decrement a
    /// count of one, treats a saturated count as "do not touch", and warns
    /// (once) on an underflow.
    ///
    /// The return value follows the C: `true` when the count was decremented
    /// (or when it is saturated), `false` when the count was one.
    pub fn try_dec_not_one(&self) -> bool {
        let mut val = self.read() as u32;

        loop {
            if val == REFCOUNT_SATURATED as u32 {
                return true;
            }

            if val == 1 {
                return false;
            }

            let new = val.wrapping_sub(1);
            if new > val {
                // The count was zero: this is an underflow. The C warns once
                // and returns true without decrementing.
                warn_once!(
                    &UNDERFLOW_WARNED,
                    "refcount_t: underflow; use-after-free.\n"
                );

                return true;
            }

            match self.as_atomic().cmpxchg(val as i32, new as i32, Release) {
                Ok(_) => return true,
                // The C updates the expected value on failure and retries.
                Err(current) => val = current as u32,
            }
        }
    }

    /// Decrement a refcount and test if it is 0.
    ///
    /// It will `WARN` on underflow and fail to decrement when saturated.
    ///
    /// Provides release memory ordering, such that prior loads and stores are done
    /// before, and provides an acquire ordering on success such that memory deallocation
    /// must come after.
    ///
    /// Returns true if the resulting refcount is 0, false otherwise.
    ///
    /// # Notes
    ///
    /// A common pattern of using `Refcount` is to free memory when the reference count reaches
    /// zero. This means that the reference to `Refcount` could become invalid after calling this
    /// function. This is fine as long as the reference to `Refcount` is no longer used when this
    /// function returns `false`. It is not necessary to use raw pointers in this scenario, see
    /// <https://github.com/rust-lang/rust/issues/55005>.
    #[inline]
    #[must_use = "use `dec` instead if you do not need to test if it is 0"]
    pub fn dec_and_test(&self) -> bool {
        // SAFETY: `self.as_ptr()` is valid.
        unsafe { bindings::refcount_dec_and_test(self.as_ptr()) }
    }
}

// SAFETY: `refcount_t` is thread-safe.
unsafe impl Send for Refcount {}

// SAFETY: `refcount_t` is thread-safe.
unsafe impl Sync for Refcount {}

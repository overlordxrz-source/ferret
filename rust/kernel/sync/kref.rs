// SPDX-License-Identifier: GPL-2.0

//! Reference counting for objects that are owned by C.
//!
//! C header: [`include/linux/kref.h`](srctree/include/linux/kref.h)
//!
//! The C `struct kref` is a reference count embedded in an object; the object
//! is freed by a callback when the last reference goes away. This module
//! provides the Rust view of that: a `KRef` borrows the count of a C-owned
//! object, can take additional references, and releases its own reference when
//! it is dropped.
//!
//! The count itself is the C `refcount_t` (see [`crate::sync::Refcount`]); this
//! type does not change it, it only makes it usable from Rust.

use crate::bindings;
use crate::sync::Refcount;
use core::marker::PhantomData;
use core::mem::ManuallyDrop;

/// The callback that releases an object whose last reference went away.
///
/// This is the C `void (*release)(struct kref *)` of `kref_put()`: it receives
/// the embedded reference count, and it is expected to free the object (the
/// caller usually uses `container_of()`).
pub type KRefRelease = unsafe extern "C" fn(*mut bindings::kref);

/// A reference to an object owned by C, counted by a `struct kref`.
///
/// The type owns *one* reference: it is released when the `KRef` is dropped,
/// and the release callback runs when it was the last one. Moving a `KRef`
/// transfers the reference, exactly like moving a C pointer does.
pub struct KRef<'a, T: ?Sized> {
    /// The reference count, embedded in the C object.
    kref: *mut bindings::kref,
    /// The object the count belongs to.
    data: *mut T,
    /// The callback that frees the object when the last reference goes away.
    release: KRefRelease,
    /// The object must outlive the reference, and the reference holds it
    /// exclusively: the `KRef` is the only Rust handle to the object while
    /// it is alive.
    _lifetime: PhantomData<&'a mut T>,
}

impl<'a, T: ?Sized> KRef<'a, T> {
    /// Borrow a reference to a C object with an embedded `struct kref`.
    ///
    /// # Safety
    ///
    /// * `kref` must point to the `struct kref` embedded in the object at
    ///   `data`, and the object must be alive for the lifetime `'a`.
    /// * The count must not be accessed through the C API in a way that
    ///   conflicts with the reference held here.
    /// * `release` must be the callback the C code would pass to `kref_put()`
    ///   for this object; it is called when the last reference goes away.
    pub unsafe fn new(
        kref: *mut bindings::kref,
        data: *mut T,
        release: KRefRelease,
    ) -> Self {
        KRef {
            kref,
            data,
            release,
            _lifetime: PhantomData,
        }
    }

    /// Take an additional reference to the object.
    ///
    /// This is the C `kref_get()`: the count is incremented, with the
    /// saturation rules of the C primitive.
    pub fn get(&self) {
        // SAFETY: the count is valid for as long as the object is alive,
        // which the lifetime of the borrow guarantees.
        let r = unsafe { Refcount::from_ptr(self.count()) };
        r.inc();
    }

    /// The reference count embedded in the object.
    fn count(&self) -> *mut bindings::refcount_t {
        // SAFETY: the caller guarantees that the kref is valid.
        unsafe { core::ptr::addr_of_mut!((*self.kref).refcount) }
    }

    /// The object this reference belongs to.
    ///
    /// The pointer is a raw pointer, so the caller must not use it to create
    /// a data race with another user of the same object.
    pub fn as_ptr(&self) -> *mut T {
        self.data
    }

    /// Give the reference to the C side.
    ///
    /// The caller becomes the owner of one reference and must release it
    /// (with `kref_put()`, or by handing it to something that does).
    pub fn into_raw(self) -> *mut bindings::kref {
        let this = ManuallyDrop::new(self);
        this.kref
    }
}

impl<T: ?Sized> Drop for KRef<'_, T> {
    fn drop(&mut self) {
        // SAFETY: the count is valid while the object is alive, which the
        // borrow guarantees.
        let r = unsafe { Refcount::from_ptr(self.count()) };

        // The C `kref_put()`: the release callback runs when the decrement
        // brought the count to zero.
        if r.dec_and_test() {
            // SAFETY: the callback is the one the C code would pass to
            // `kref_put()` for this object.
            unsafe { (self.release)(self.kref) };
        }
    }
}

// The `KRef` holds a unique borrow of the object (`PhantomData<&'a mut T>`),
// so the derived `Send` and `Sync` bounds are the ones of the object itself:
// `Send` needs `T: Send`, `Sync` needs `T: Sync`. The reference count is
// atomic, so moving the handle between tasks and sharing it are both safe
// with respect to the count.

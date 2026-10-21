// SPDX-License-Identifier: GPL-2.0

//! Reference counting for objects that are owned by C.
//!
//! C header: [`include/linux/kref.h`](srctree/include/linux/kref.h)
//!
//! The C `struct kref` is a reference count embedded in an object; the object
//! is freed by a callback when the last reference goes away. This module
//! provides the Rust view of that: a `KRef` holds a reference to a C-owned
//! object, can take additional references, and releases its own reference
//! when it is dropped.
//!
//! The count itself is the C `refcount_t` (see [`crate::sync::Refcount`]); this
//! type does not change it, it only makes it usable from Rust.
//!
//! # Ownership
//!
//! The object belongs to C. This handle does *not* own it and does not
//! promise anything about who else can reach it:
//!
//! * the C code may hold other references to the same object, and it may
//!   access the object through its own pointers at any time;
//! * other `KRef`s for the same object may exist at the same time, each
//!   holding one reference.
//!
//! The handle therefore only owns *one reference*, and the object is reached
//! through a raw pointer, so the responsibility for the accesses to it stays
//! with the caller. See the safety contract of [`KRef::new()`].

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
/// The type holds *one* reference: it is released when the `KRef` is dropped,
/// and the release callback runs when it was the last one. Moving a `KRef`
/// transfers the reference, exactly like moving a C pointer does.
///
/// The object itself is *not* owned by the handle. See the [module
/// documentation](self) for what that means for the aliasing of the object.
pub struct KRef<'a, T: ?Sized> {
    /// The reference count, embedded in the C object.
    kref: *mut bindings::kref,
    /// The object the count belongs to.
    data: *mut T,
    /// The callback that frees the object when the last reference goes away.
    release: KRefRelease,
    /// The lifetime of the C object.
    ///
    /// This is a plain lifetime marker. It deliberately says nothing about
    /// the aliasing of the object:
    ///
    /// * `PhantomData<&'a mut T>` would promise *exclusive* access, which
    ///   cannot be proven: the C code may hold other references to the same
    ///   object, and other `KRef`s may exist for it at the same time. The
    ///   promise would be false, and the borrow checker would not even
    ///   enforce it, because the object is reached through a raw pointer.
    /// * `PhantomData<&'a T>` would promise that the object is not mutated
    ///   for `'a`, which the C code may do anyway.
    ///
    /// The marker is what the `Send` and `Sync` implementations below are
    /// written against: the handle is not tied to the object type, so the
    /// two are decided explicitly.
    _lifetime: PhantomData<&'a ()>,
}

impl<'a, T: ?Sized> KRef<'a, T> {
    /// Take a reference to a C object with an embedded `struct kref`.
    ///
    /// # Safety
    ///
    /// * `kref` must point to the `struct kref` embedded in the object at
    ///   `data`, and the object must be alive for the lifetime `'a`.
    /// * The count must not be accessed through the C API in a way that
    ///   conflicts with the reference held here.
    /// * `release` must be the callback the C code would pass to `kref_put()`
    ///   for this object; it is called when the last reference goes away.
    ///   It must be safe to call from any task, exactly like the C
    ///   `kref_put()` it replaces, and it must not assume that it has
    ///   exclusive access to the object beyond what the C `kref_put()`
    ///   provides.
    /// * The caller must not create a data race through the pointer returned
    ///   by [`KRef::as_ptr()`]: the object belongs to C, which may access it
    ///   concurrently.
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
        // which the lifetime of the handle guarantees.
        let r = unsafe { Refcount::from_ptr(self.count()) };
        r.inc();
    }

    /// The reference count embedded in the object.
    fn count(&self) -> *mut bindings::refcount_t {
        // SAFETY: the caller guarantees that the kref is valid.
        unsafe { core::ptr::addr_of_mut!((*self.kref).refcount) }
    }

    /// The current value of the reference count.
    ///
    /// The read is relaxed, like the C `refcount_read()`.
    pub fn read(&self) -> i32 {
        // SAFETY: the count is valid for as long as the object is alive,
        // which the lifetime of the handle guarantees.
        let r = unsafe { Refcount::from_ptr(self.count()) };
        r.read()
    }

    /// The object this reference belongs to.
    ///
    /// The pointer is a raw pointer: the object is owned by C, which may
    /// access it concurrently, so the caller must not use it to create a
    /// data race with another user of the same object.
    pub fn as_ptr(&self) -> *mut T {
        self.data
    }

    /// Give the reference to the C side.
    ///
    /// The caller becomes the owner of one reference and must release it
    /// (with `kref_put()`, or by handing it to something that does). The
    /// handle does not release anything: the reference is *transferred*.
    pub fn into_raw(self) -> *mut bindings::kref {
        let this = ManuallyDrop::new(self);
        this.kref
    }
}

impl<T: ?Sized> Drop for KRef<'_, T> {
    fn drop(&mut self) {
        // SAFETY: the count is valid while the object is alive, which the
        // lifetime of the handle guarantees.
        let r = unsafe { Refcount::from_ptr(self.count()) };

        // The C `kref_put()`: the release callback runs when the decrement
        // brought the count to zero.
        if r.dec_and_test() {
            // SAFETY: the callback is the one the C code would pass to
            // `kref_put()` for this object, and it is safe to call from any
            // task.
            unsafe { (self.release)(self.kref) };
        }
    }
}

// SAFETY: the handle owns one reference and nothing else. Moving it to
// another task transfers that reference, and the release callback may then
// run in that task, which is exactly what the C `kref_put()` allows. The
// operations on the count itself are atomic.
unsafe impl<T: ?Sized> Send for KRef<'_, T> {}

// SAFETY: the handle only performs atomic operations on the count and hands
// out a raw pointer to the object. The accesses to the object through that
// pointer are the caller's responsibility, exactly as they are for the C
// `struct kref` this replaces.
unsafe impl<T: ?Sized> Sync for KRef<'_, T> {}

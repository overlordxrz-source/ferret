# Design: virtual filesystem in Rust

Status: design target.

## Scope

* `fs/namei.c` — path resolution (last; hardest).
* `fs/inode.c`, `fs/dcache.c` — inode and dentry lifetime.
* `fs/file.c`, `fs/open.c` — descriptor tables and open/close.
* `fs/read_write.c` — read/write iteration, `iov_iter` interaction.
* `fs/pipe.c` — first concrete candidate: self-contained, testable, no block
  device.
* Page-cache coupling in `mm/filemap.c` (with the MM tier).

## Lifetime model

Reference counting in VFS is the central hazard (`dput`, `iput`, RCU-walk
lookups). Rust models it as:

* `InodeRef` / `DentryRef`: owning handles that call `iput`/`dput` on drop.
* `InodeBorrow<'a>` / `DentryBorrow<'a>`: borrowed handles valid while a
  reference is held by the caller.
* RCU-walk paths get a separate `RcuDentry<'a>` type that cannot be upgraded
  without an explicit `try_get()` returning `Option<DentryRef>`.

This makes "use after dput" a compile error inside migrated code.

## Order of work

1. **`fs/pipe.c`** — ring buffer, poll integration, splice support. Exercises
   file operations, wait queues, and fd lifetime with no on-disk format.
2. **`fs/eventfd.c` / `signalfd`** — small anon-inode filesystems; exercise
   `anon_inode_getfile` and `file_operations` bridging.
3. **Inode/dentry reference helpers** used by small filesystems.
4. **`fs/ramfs` / `fs/tmpfs`** — first real filesystem migration; tmpfs pulls
   in the page cache and shmem, so it is deliberately later than ramfs.
5. **VFS lookup** — only after namei's locking and RCU rules are expressible.

## File-operation bridging

Upstream `file_operations` is a C vtable of function pointers. Rust implements
only the methods it migrates and forwards the rest to C during the transition:

```
transitional FileOps { open: Option<RustOpen>, read_iter: Option<RustReadIter>, ... }
```

The bridging layer must preserve `-errno` return conventions exactly, including
`-ERESTARTSYS` behaviour and partial-copy semantics.

## Verification

* `tools/testing/selftests/filesystems`, `fs` LTP subset.
* xfstests for real filesystems (later, needs a block device in QEMU).
* KUnit tests for pipe ring-buffer edge cases (wrap-around, zero-length,
  `O_NONBLOCK`, concurrent reader/writer).
* Fault injection (`CONFIG_FAULT_INJECTION`) to exercise error paths.

## Risks

* Lock ordering across `inode->i_rwsem`, `dentry->d_lock` and `sb->s_umount`.
* RCU grace-period assumptions cannot be expressed in Rust's type system; they
  remain documented obligations on `unsafe` boundary functions.
* Userspace ABI corner cases (atomicity of `read`/`write`, `O_DIRECT`
  alignment) must be preserved exactly.

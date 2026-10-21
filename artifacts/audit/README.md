# Audit evidence

The raw evidence of the independent-verification gate
([`AUDIT_PROTOCOL.md`](../../../AUDIT_PROTOCOL.md)) for checkpoint 005.

## `mutation-test.txt`

The mutation testing injects a defect into the Rust implementation, in a
disposable git worktree, rebuilds the kernel, boots it and checks that the
suite notices.

The file **accumulates the runs**, so the history is visible. Two entries
need a word of explanation:

* **`INVALID  the mutation did not build`** — this is the first version of
  the "the underflow also decrements the count" mutation. It removed the
  warning together with the decrement, which made the "warn once" flag dead
  code, and the kernel crate builds with `-D warnings`, so the mutation did
  not compile. The mutation was corrected (the warning is kept, the
  decrement is added) and re-run: the suite **detects** it. The `PASS` line
  of the re-run is the last one in the file.
* **`HOLE  the mutation was NOT detected`** — these are the three mutations
  that are declared *not detectable* in the protocol, with the reason: a
  memory-ordering defect is invisible to a functional test, and two of the
  paths are only reachable under a race. (The wording of the message is
  from the first version of the script; the script now says
  `KNOWN LIMIT`.)

Summary of the eight mutations:

| Mutation | Declared | Result |
| --- | --- | --- |
| a count of one is reported as decremented | detectable | detected |
| the underflow also decrements the count | detectable | detected (after the mutation was corrected) |
| the saturated count is decremented | detectable | detected |
| the count is saturated to zero | detectable | detected |
| the mutex is taken when it must not be | detectable | detected |
| the spinlock is not released on the failure path | not detectable | not detected |
| the interrupt state is not restored on the failure path | not detectable | not detected |
| the compare-and-exchange is relaxed | not detectable | not detected |

## `sanitizer-kasan-ubsan.txt`

The KASAN + UBSAN build and boot. The result of that run:

* the audited suites pass inside the sanitized kernel (`refcount_rust` 7/7,
  `refcount_diff` 9/9);
* **no sanitizer report points at the audited component**;
* the boot of the sanitized kernel reports a KUnit failure, and the failure
  is **not** in this component: two `shift-out-of-bounds` reports in
  `include/linux/log2.h` are reached from the `kfifo_diff` suite of the
  earlier checkpoint 003. They are recorded as issue #25.

The message of that run predates the current version of the script, which
separates the two results; the separation is described in
`docs/ferret/design/COMPONENT_005_AUDIT.md` §8.

## KCSAN

**NOT RUN**: the build and the boot of a KCSAN kernel do not fit in the
checkpoint's time budget. It is listed as NOT RUN, not as a pass.

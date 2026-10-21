# Design: scheduler and task management in Rust

Status: design target.

## Scope

* `kernel/sched/core.c` — task lifecycle, context switching entry points.
* `kernel/sched/fair.c` — CFS runqueue operations.
* `kernel/sched/rt.c`, `deadline.c` — later, and only after fair is stable.
* `kernel/fork.c` — task creation/teardown bookkeeping.
* `kernel/exit.c` — exit and reaping paths.

## Core invariants

1. **`task_struct` is shared with assembly and with C code.** Rust cannot own it
   initially. The migration starts with accessor types that borrow
   `*mut task_struct` for a bounded lifetime tied to RCU or to the task lock.
2. **Runqueue operations happen with `rq->lock` held and IRQs disabled.**
   The Rust API must make "call this without the lock" unrepresentable — e.g.
   `fn enqueue(rq: &mut LockedRunQueue<'_>, task: TaskRef<'_>)`.
3. **Preemption state is per-task and must be balanced.** A guard type
   (`PreemptGuard`) that increments on creation and decrements on drop replaces
   `preempt_disable()`/`preempt_enable()` in migrated code. Guard leaks become
   visible in review.
4. **`current` is architecture-specific.** Accessed through one function that
   documents the calling context (per-CPU, preemption disabled).

## Proposed layout

```
kernel/sched/rust/
  task.rs        TaskRef, TaskState, accessors with documented provenance
  rq.rs          RunQueue wrapper, LockedRunQueue
  fair.rs        vruntime arithmetic, load tracking, pick_next
  preempt.rs     PreemptGuard, need_resched
  switch.rs      context-switch glue (assembly boundary unchanged)
```

## Migration order

1. `task_struct` field accessors used by the scheduler only (no ownership).
2. CFS arithmetic: `calc_delta_fair`, `update_curr` accounting, entity
   insertion helpers. These are pure functions over `sched_entity` fields and
   can be differentially tested by replaying schedules.
3. Runqueue enqueue/dequeue with the lock expressed in the type system.
4. `pick_next_task_fair`.
5. Only then consider `context_switch` call sites; the actual register save and
   restore remains assembly, called through a small Rust FFI shim whose
   contract is "returns on the other task's stack".

## Verification

* `CONFIG_SCHED_DEBUG`, `CONFIG_DEBUG_PREEMPT`, lockdep/RCU stall detection.
* Stress: `hackbench`, `perf bench sched`, and a kselftest subset under QEMU.
* Differential tests for CFS arithmetic using recorded (vruntime, weight,
  delta) vectors captured from the C implementation.
* Boot-time assertion that migrated and C paths produce identical vruntime
  updates when a debug config is enabled (transitional, removed at the end).

## Risks

* Scheduler changes are performance-critical and hard to test in TCG; relative
  benchmarks only, and never as the sole acceptance criterion.
* `task_struct` layout is ABI-relevant for tracing/perf; Rust code must not
  introduce padding or reordering.

## Dependency reduction goal

Each scheduler migration must delete a C accessor or critical-section
assumption, not add a Rust-only abstraction that duplicates one.

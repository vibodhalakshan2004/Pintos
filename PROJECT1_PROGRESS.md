# Pintos Project 1: Threads — Progress Notes

Last updated: September 5, 2026

This document records the work completed so far for Johns Hopkins Pintos
Project 1. It describes the implementation currently present in this
repository, the concepts learned, the tests that have passed, and the work
that remains.

## Working environment

- Repository: `~/pintos`
- Source tree: `~/pintos/src`
- Threads project: `~/pintos/src/threads`
- Build directory: `~/pintos/src/threads/build`
- Emulator: QEMU
- Bochs is not used because of local Bochs 2.8 compatibility problems.
- Project requirements are taken from the Johns Hopkins Project 1 PDF.

Build the threads project with:

```bash
cd ~/pintos/src/threads
make
```

Run one test manually with QEMU using:

```bash
cd ~/pintos/src/threads/build
pintos -v -k -T 60 --qemu -- -q run TEST-NAME
```

Run an official `.result` checker with QEMU using:

```bash
cd ~/pintos/src/threads/build
make SIMULATOR=--qemu tests/threads/TEST-NAME.result
```

## Part 1: Alarm Clock

Status: implemented and tested.

### Problem in the original implementation

The original `timer_sleep()` used busy waiting. A sleeping thread repeatedly
checked the timer and called `thread_yield()` until enough ticks had elapsed.
Although it allowed other threads to run, the sleeping thread remained ready
and repeatedly consumed CPU time.

The project requires sleeping threads to block so that they consume no CPU
while waiting.

### Changes to `threads/thread.h`

The following field was added to `struct thread`:

```c
int64_t wake_tick;
```

`wake_tick` stores an absolute timer tick, not a relative duration. For
example, if the current tick is 100 and a thread sleeps for 20 ticks, its
`wake_tick` is 120.

### Changes to `devices/timer.c`

A global list stores sleeping threads:

```c
static struct list sleeping_threads;
```

It is initialized in `timer_init()`:

```c
list_init (&sleeping_threads);
```

The list is kept in increasing `wake_tick` order using `wake_time_less()`.
Therefore, the first element is always the next thread that should wake.

The new `timer_sleep()` behavior is:

1. Assert that interrupts were initially enabled.
2. Return immediately when `ticks <= 0`.
3. Disable interrupts.
4. Calculate the current thread's absolute wake-up tick.
5. Insert the current thread into `sleeping_threads` in wake-time order.
6. Call `thread_block()`.
7. Restore the previous interrupt level after the thread is awakened.

Interrupts are disabled while inserting and blocking because the timer
interrupt also accesses `sleeping_threads`. This prevents a lost-wakeup race
between adding the thread to the list and blocking it.

The timer interrupt now:

1. Increments the global timer tick.
2. Examines the front of `sleeping_threads`.
3. Removes and unblocks every thread with `wake_tick <= ticks`.
4. Stops as soon as the front thread is not ready, because the list is sorted.
5. Calls the original `thread_tick()` for scheduler accounting and time-slice
   enforcement.

### Alarm Clock data flow

```text
Running thread
    |
    | timer_sleep()
    v
sleeping_threads
    |
    | timer interrupt reaches wake_tick
    v
thread_unblock()
    |
    v
ready_list
    |
    | scheduler selects thread
    v
Running thread
```

The thread's embedded `elem` can move between `sleeping_threads` and
`ready_list` because a sleeping thread cannot be ready at the same time.

### Verified Alarm Clock tests

The repository contains passing results for:

- `alarm-single`
- `alarm-multiple`
- `alarm-simultaneous`
- `alarm-zero`
- `alarm-negative`
- `alarm-priority`

The main alarm tests showed many idle ticks and relatively few kernel ticks,
which is evidence that busy waiting was removed.

## Part 2: Basic Priority Scheduling

Status: partially implemented. Basic ready-list ordering and preemption are
working. Priority-aware synchronization and donation remain unfinished.

Pintos priorities range from `PRI_MIN` (0) to `PRI_MAX` (63). A larger number
means a higher priority.

### Original scheduler behavior

Originally, `ready_list` was a FIFO queue:

```text
thread_unblock() or thread_yield()
    |
    | list_push_back()
    v
ready_list
    |
    | list_pop_front()
    v
next_thread_to_run()
```

Priorities were stored in each thread but were not used to choose the next
thread.

### Ready-list priority ordering

A comparator named `thread_priority_more()` was added to `thread.c`. It
returns true when thread A has a numerically higher priority than thread B.

Both `thread_unblock()` and `thread_yield()` now use
`list_insert_ordered()` instead of `list_push_back()`. The ready list is kept
in this order:

```text
highest priority -> ... -> lowest priority
```

`next_thread_to_run()` still removes the front element. Because the list is
ordered, the front element is now a highest-priority ready thread.

The comparator uses `>` rather than `>=`. Equal-priority threads are inserted
after existing equal-priority threads, preserving FIFO/round-robin behavior
among threads of the same priority.

### Preemption when creating a thread

After `thread_create()` calls `thread_unblock()` for a new thread, it compares
the new thread's priority with the current thread's priority.

If the new thread has a strictly higher priority, the current thread calls
`thread_yield()`. The ordered ready list then causes the new higher-priority
thread to run before `thread_create()` returns.

Equal-priority creation does not force an immediate yield.

### Changing the current thread's priority

`thread_set_priority()` now:

1. Validates that the new value is between `PRI_MIN` and `PRI_MAX`.
2. Disables interrupts while changing priority and examining `ready_list`.
3. Updates the current thread's priority.
4. If `ready_list` is nonempty, examines its front thread.
5. Records whether the front thread now has a higher priority.
6. Restores the previous interrupt level on every path.
7. Calls `thread_yield()` if a higher-priority ready thread exists.

Only the front needs to be checked because `ready_list` is ordered.

The `should_yield` Boolean is a local variable that remembers the scheduling
decision while interrupts are disabled. It does not perform scheduling by
itself; `thread_yield()` performs the actual yield.

### Basic priority data flow

```text
thread_create()
    |
    v
thread_unblock()
    |
    | ordered insertion
    v
ready_list (highest priority at front)
    |
    v
next_thread_to_run()
    |
    v
schedule()
```

`thread_yield()` adds the current thread back to the same ordered ready list
before calling `schedule()`.

### Verified basic priority tests

The repository contains passing results for:

- `priority-preempt`
- `priority-change`
- `priority-fifo`

These verify creation-time preemption, yielding after lowering priority, and
round-robin behavior among equal-priority threads.

## Priority-aware semaphores

Status: implemented and tested.

A semaphore contains:

```c
unsigned value;
struct list waiters;
```

`value` is the number of available permits. `waiters` stores threads that
cannot continue because no permit is available.

`sema_down()` has two possible outcomes:

- If `value > 0`, decrement it and continue.
- If `value == 0`, add the current thread to `waiters` and block it.

`sema_up()` increments the value and may move one thread from the semaphore's
waiters list to the scheduler's ready list through `thread_unblock()`.

```text
sema->waiters
    |
    | sema_up()
    v
thread_unblock()
    |
    v
ready_list
```

The original semaphore implementation was FIFO. It used `list_push_back()` in
`sema_down()` and `list_pop_front()` in `sema_up()`, so the oldest waiter woke
regardless of priority.

A thread-priority comparator was added to `synch.c`. `sema_down()` now uses
`list_insert_ordered()` to place blocked threads into the semaphore waiters
list from highest to lowest priority. `sema_up()` also sorts the waiter list
immediately before selecting its front element. Sorting at wake-up time is
important because a blocked thread's effective priority may later change due
to priority donation.

After removing the highest-priority waiter, `sema_up()` calls
`thread_unblock()`. It records whether the awakened thread has a higher
priority than the running thread, finishes the semaphore update, and restores
the previous interrupt level before preempting.

- In normal thread context it calls `thread_yield()`.
- In interrupt context it calls `intr_yield_on_return()` because an interrupt
  handler cannot call `thread_yield()` directly.

`thread_unblock()` only changes a waiter from `THREAD_BLOCKED` to
`THREAD_READY`; it does not directly transfer a semaphore permit. Therefore,
`sema_up()` still increments `value`. When the awakened thread runs, it
rechecks the `while (value == 0)` condition and decrements the available
permit itself.

Semaphores are used for:

- Waiting for an event, commonly with an initial value of 0.
- Representing one available resource with an initial value of 1.
- Representing N identical resources with an initial value of N.
- Implementing Pintos locks internally.
- Supporting condition-variable waiting internally.

`sema_down()` may block, so it cannot be called from an interrupt handler.
`sema_up()` does not block and may be called from an interrupt handler.

The following test and regressions passed after this implementation:

- `priority-sema`
- `priority-change`
- `priority-preempt`
- `alarm-simultaneous`

## Priority-aware condition variables

Status: implemented and tested.

The original condition-variable implementation kept `struct semaphore_elem`
waiters in FIFO order. A `thread` pointer was added to each
`struct semaphore_elem` so that condition-variable code can find the priority
of the thread represented by that waiting ticket.

`cond_wait()` now records `thread_current()` in its local waiter and inserts
the waiter into `cond->waiters` using a condition-waiter priority comparator.
The condition list contains `semaphore_elem.elem`, while each private
semaphore's waiter list contains the blocked thread's `thread.elem`. Separate
embedded list elements are necessary because one list element cannot belong
to two lists simultaneously.

`cond_signal()` sorts the condition waiters immediately before selecting the
front waiter. It then calls `sema_up()` on that waiter's private semaphore.
The private semaphore provides the actual block/wakeup operation, while the
associated lock protects the shared state and the condition variable
organizes threads waiting for that state to change.

The following test and regressions passed after this implementation:

- `priority-condvar`
- `priority-sema`
- `priority-fifo`
- `alarm-simultaneous`

## Thread list membership

Each thread contains two embedded list elements:

- `allelem` is used for the list of all threads.
- `elem` is reused by state-dependent lists.

Typical `elem` membership is:

```text
THREAD_READY              -> ready_list
THREAD_BLOCKED on timer   -> sleeping_threads
THREAD_BLOCKED on sema    -> semaphore waiters
```

This reuse is safe because a thread cannot be ready, sleeping, and blocked on
a semaphore simultaneously. Code must remove `elem` from one state-dependent
list before inserting it into another.

## Interrupt and scheduling concepts learned

- Interrupt state is CPU state, not a per-thread field.
- Disabling interrupts is appropriate for short updates shared with an
  interrupt handler or scheduler code.
- The previous interrupt level must be restored on every control-flow path.
- `thread_block()` changes the current thread to `THREAD_BLOCKED` and invokes
  the scheduler.
- `thread_unblock()` changes a blocked thread to `THREAD_READY` and inserts it
  into `ready_list`; it does not necessarily switch immediately by itself.
- `thread_yield()` changes the running thread to `THREAD_READY`, reinserts it,
  and invokes the scheduler.
- `thread_tick()` enforces a four-tick time slice using
  `intr_yield_on_return()`.
- A normal thread can call `thread_yield()` directly. An interrupt handler
  must request a yield on interrupt return instead.
- Blocking avoids busy waiting because blocked threads are absent from the
  scheduler's ready list and consume no CPU time.

## Current test snapshot

Passing result files currently include:

```text
alarm-single
alarm-multiple
alarm-simultaneous
alarm-zero
alarm-negative
alarm-priority
priority-preempt
priority-change
priority-fifo
priority-sema
priority-condvar
```

Known failing or unfinished areas currently include:

```text
priority donation tests
MLFQS tests
```

A saved `.result` file describes the result of the build that produced it. A
test should be rerun after relevant source changes before relying on an older
result file.

## Next implementation steps

Continue in small, tested stages:

1. Study priority inversion with a low-priority lock holder and a
   high-priority waiter.
2. Add priority donation for one lock.
3. Support removing/restoring donations when locks are released.
4. Support multiple donations.
5. Support nested donation, with a reasonable depth limit if needed.
6. Verify donation when a lock holder is blocked on a semaphore.
7. Run the complete priority test group.
8. Implement the advanced/MLFQS scheduler only after priority scheduling is
    stable.
9. Complete the required `src/threads/DESIGNDOC` as work progresses.

## Work not yet implemented

- Lock priority donation
- Multiple priority donations
- Nested priority donation
- Base/original priority tracking for donation restoration
- Advanced 4.4BSD/MLFQS scheduler
- Final Project 1 `DESIGNDOC`

## Code-quality cleanup before submission

The current implementation is functionally progressing, but several edited
sections should be reformatted to match Pintos coding style before submission.
Examples include spaces around operators and after commas, brace layout,
comment indentation, and the spelling of `should_yield`. Run:

```bash
cd ~/pintos
git diff --check
```

Also review every modified block against the surrounding Pintos style. Style
cleanup should not change scheduler behavior.

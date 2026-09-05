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
4. Requests a yield on interrupt return if an awakened thread has a higher
   effective priority than the interrupted thread.
5. Stops as soon as the front thread is not ready, because the list is sorted.
6. Calls the original `thread_tick()` for scheduler accounting and time-slice
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

Status: basic ready-list ordering, creation-time preemption, and
priority-aware synchronization are implemented. Direct, multiple, and nested
donation tests pass. All 18 alarm/priority tests have passing results for the
current build; scheduling edge-case review remains.

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

`next_thread_to_run()` sorts the list before removing the front element.
Donation can change a ready thread's priority after insertion, so this refresh
ensures the selected thread still has a highest ready priority.

The comparator uses `>` rather than `>=`. Equal-priority threads are inserted
after existing equal-priority threads, preserving FIFO/round-robin behavior
among threads of the same priority.

### Preemption when creating a thread

After `thread_create()` calls `thread_unblock()` for a new thread, it compares
the new thread's priority with the current thread's priority. Interrupts stay
disabled across both operations, and the comparison is saved in
`should_yield` before the previous interrupt level is restored. This prevents
reading the new thread's memory after it has had a chance to run and exit.

If the new thread has a strictly higher priority, the current thread calls
`thread_yield()`. The ordered ready list then causes the new higher-priority
thread to run before `thread_create()` returns.

Equal-priority creation does not force an immediate yield.

### Changing the current thread's priority

`thread_set_priority()` now:

1. Validates that the new value is between `PRI_MIN` and `PRI_MAX`.
2. Disables interrupts while changing priority and examining `ready_list`.
3. Updates `base_priority` and recomputes effective `priority` as the maximum
   of the new base priority and all current donor priorities.
4. If `ready_list` is nonempty, sorts it and examines its front thread.
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

## Priority-donation data model

Status: connected to lock acquisition/release and priority changes; direct,
multiple, and nested donation tests pass.

`struct thread` now distinguishes its normal priority from its effective
scheduler priority:

- `base_priority` stores the priority requested by thread creation or
  `thread_set_priority()` when no donation is considered.
- `priority` remains the effective priority used by scheduling decisions.
- `waiting_lock` points to the lock the thread is currently attempting to
  acquire, or is `NULL` when it is not waiting for a lock.
- `donations` is the list of threads currently donating to this thread.
- `donation_elem` lets a donor appear in another thread's donation list while
  its existing `elem` is used by a semaphore waiter list or ready list.

`struct lock` is forward-declared in `thread.h`, allowing `struct thread` to
store a lock pointer without introducing a circular header dependency.

`init_thread()` initializes `base_priority` to the initial effective priority,
sets `waiting_lock` to `NULL`, and initializes the donations list.

The following regressions passed after adding these fields:

- `priority-condvar`
- `priority-sema`
- `priority-preempt`
- `alarm-single`

## Direct and multiple priority donation

Status: implemented and tested, including donation to a lock holder blocked
on a semaphore. Nested propagation is described below.

In `lock_acquire()`, a thread encountering an occupied lock records that lock
in `waiting_lock` and adds its `donation_elem` to the holder's `donations`.
It raises the holder's effective priority if necessary, then uses
`sema_down()` to wait for the lock. After acquiring it, the thread clears
`waiting_lock` and becomes the holder. Donation is skipped in MLFQS mode.

In `lock_release()`, `remove_lock_donations()` removes only donors waiting for
the released lock. `refresh_priority()` recalculates the holder's effective
priority from its base and remaining donors before the lock is made
available through `sema_up()`. Interrupts are disabled around these updates.

The donor list already supports several donors and several held locks.
Releasing one lock therefore preserves donations associated with other locks.
Sorting semaphore waiters before waking one also handles a blocked holder
whose priority increased through donation.

`thread_set_priority()` changes the base priority without discarding active
donations. `next_thread_to_run()` re-sorts ready threads to account for
effective priorities changing after insertion.

## Nested priority donation

Status: implemented; `priority-donate-nest` and `priority-donate-chain` pass.

`donate_priority()` in `synch.c` follows each donor's `waiting_lock` to its
holder. It raises that holder's effective priority if necessary, then treats
the holder as the next donor. Traversal stops when there is no waiting lock,
no holder, or eight holders have been visited. The helper asserts that
interrupts are already disabled; it does not disable them itself.

For example, H (priority 50) waits for a lock held by M (priority 30), while M
waits for a lock held by L (priority 10). The helper propagates priority 50
through M to L, allowing the dependency preventing H from running to receive
the appropriate scheduling priority. Base priorities do not change.

`lock_acquire()` calls this helper after recording the waiting lock and
inserting the direct donor into the holder's donation list. The helper does
not add list entries: H belongs to M's donor list, and M belongs to L's donor
list. This preserves the rule that one `donation_elem` cannot occupy multiple
lists simultaneously.

## Thread list membership

Each thread now contains three embedded list elements:

- `allelem` is used for the list of all threads.
- `elem` is reused by state-dependent lists.
- `donation_elem` belongs to the donation list of the relevant lock holder.

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

### Resolved regression during direct-donation implementation

The first direct-donation implementation caused `priority-condvar` and
`priority-sema` failures. The old `thread_set_priority()` updated only
`priority`, while the new `lock_release()` restored priority from
`base_priority`. After main lowered itself to 0, releasing the internal
`tid_lock` during thread creation restored its stale base priority of 31.
The test workers therefore did not preempt main as expected.

The setter now updates base priority, recomputes effective priority from all
active donors, and checks whether to yield. The correction is implemented;
both synchronization regressions and `priority-donate-lower` now pass.

Passing results from completed stages include:

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
priority-donate-one
priority-donate-lower
priority-donate-multiple
priority-donate-multiple2
priority-donate-sema
priority-donate-nest
priority-donate-chain
```

Known failing or unfinished areas currently include:

```text
MLFQS tests
```

A saved `.result` file describes the result of the build that produced it. A
test should be rerun after relevant source changes before relying on an older
result file.

After nested donation was implemented, the QEMU make/check run confirmed
up-to-date passing results for all seven donation tests. It freshly ran and
passed all six alarm tests and the five other priority tests. All 18
alarm/priority results therefore pass for the current build. This does not
prove that every scheduling edge case is handled.

## Timer wake-up priority preemption

Status: implemented and built successfully. All 18 alarm/priority tests were
freshly rerun with QEMU after this change and passed (six alarm tests, five
basic/synchronization priority tests, and seven donation tests).

Previously, `timer_interrupt()` unblocked expired sleepers without requesting
preemption when a newly ready thread outranked the interrupted thread.
`thread_unblock()` intentionally does not preempt, and `thread_tick()` only
requests a yield when the time slice expires. A higher-priority sleeper could
therefore wait for the remaining time slice instead of running immediately
after interrupt return.

After unblocking each sleeper, `timer_interrupt()` now compares its effective
priority with `thread_current()->priority` and calls `intr_yield_on_return()`
if it is higher. This requests a switch after the handler finishes; it does
not call `thread_yield()` inside an interrupt. Equal priorities do not request
additional preemption. The wake-up loop still processes all expired sleepers,
and the original `thread_tick()` call remains intact.

The existing alarm tests also passed without this correction, so their success
alone does not establish immediate wake-up preemption. The correction follows
from inspection of the timer, unblock, and interrupt-return scheduling paths;
a dedicated immediate-preemption timing test has not been added.

Only this timer source change was implemented by the assistant with explicit
user permission. No commit was created.

## Thread-creation lifetime correction

Status: implemented and built successfully. All 18 alarm/priority tests were
freshly rerun with QEMU after this correction and passed. `git diff --check`
also passed for this change.

Previously, the creation path read `t->priority` after making the new thread
runnable with interrupts potentially enabled. An intervening timer-driven
switch could allow that thread to run, exit, and have its memory freed before
the read. Accessing that memory would be a use-after-free.

`thread_create()` now saves the old interrupt level and disables interrupts
before calling `thread_unblock()`. It then records the priority comparison in
the local Boolean `should_yield`, restores the previous interrupt level, and
yields if needed. No access to `t` occurs after interrupts are restored.
`thread_unblock()` preserves the caller's disabled interrupt state and does
not itself preempt, so the thread cannot run during the protected comparison.

The source change was explicitly authorized by the user. Only `thread.c` and
this document were edited for this step; no commit was created. Existing
regression tests do not specifically force the original rare race; the
lifetime fix is additionally justified by inspecting the protected sequence.

## Next foundation: MLFQS fixed-point arithmetic

Status: helper header implemented and syntax-checked; it is not connected to
the scheduler yet. The nice/load-average/recent-CPU functions in `thread.c`
remain stubs.

`src/threads/fixed-point.h` defines a signed `fixed_t` based on `int32_t` and
uses 17.14 representation with a scaling factor of 16384. It provides:

- conversion from an integer to fixed point;
- conversion toward zero and conversion rounded to nearest;
- fixed-point addition and subtraction;
- addition and subtraction between fixed-point and integer values;
- fixed-point multiplication and division;
- multiplication and division between fixed-point and integer values.

Fixed-point multiplication and division use `int64_t` intermediate values to
avoid overflowing a 32-bit temporary before the result is scaled back down.
The header uses `static inline` functions so it can be safely included in
multiple C translation units while keeping the helpers small and type-checked.

The header passed a direct `i386-elf-gcc` syntax check. It is currently an
untracked file. It is included by `thread.h`, so a forced Pintos rebuild
parses it throughout the kernel. Its missing final newline has been fixed.

MLFQS needs fractional `recent_cpu` and `load_avg` values without using kernel
floating-point arithmetic. In 17.14 representation, the stored integer is
scaled by 16384: 1 is stored as 16384, and 1.5 as 24576. Conversion tests
should include positive, negative, zero, and half-integer cases.

The fraction remains available while a value stays in `fixed_t` form. Public
Pintos getters later preserve two decimal places by multiplying the fixed
value by 100 before converting it to an ordinary integer. For example, a
fixed-point value representing 1.75 is returned as integer 175.

Per-thread `nice` and `recent_cpu` fields, their zero initialization, and
MLFQS inheritance by newly created threads have been added. The single
system-wide `load_avg` is declared `static` in `thread.c` and initialized to
fixed-point zero in `thread_init()`.

An initial review found `static load_avg` incorrectly declared in `thread.h`,
which gave every C translation unit a separate private copy and generated
many unused-variable warnings. With explicit user permission, the assistant
moved the definition into `thread.c` and fixed the final newline in
`fixed-point.h`. A forced rebuild completed successfully without any
`load_avg` warnings. The pre-existing unrelated warnings in `init.c`, `ide.c`,
and debugging/library code remain outside this step.

After the correction, all 18 alarm and normal-priority tests were freshly run
with QEMU and passed. MLFQS tests are not expected to pass yet because the
formulas and timer updates are still unimplemented.

## MLFQS priority calculation

Status: implemented and build-verified; periodic recalculation is not yet
connected to timer ticks.

`mlfqs_update_priority()` calculates one non-idle thread's effective priority
using:

```text
priority = PRI_MAX - recent_cpu / 4 - nice * 2
```

The fixed-point `recent_cpu / 4` result is converted toward zero before the
integer priority is calculated. The result is clamped to the inclusive range
`PRI_MIN` through `PRI_MAX`. The helper requires interrupts to be disabled and
leaves the idle thread unchanged.

When MLFQS is enabled, `thread_init()` recalculates the initial thread after
its MLFQS fields have been initialized. `thread_create()` copies `nice` and
`recent_cpu` from the parent and recalculates the child's priority before it
becomes ready. It recognizes creation of the idle thread through its function
pointer and preserves that thread's `PRI_MIN` priority.

Review found that `thread_init()` accidentally contained two copies of the
initial thread's status assignment and TID allocation. That would allocate two
IDs and discard the first. With explicit user permission, the assistant
removed the duplicate pair. A forced build completed successfully, and
`priority-preempt`, `priority-donate-chain`, and `alarm-priority` passed under
QEMU. No MLFQS test is expected to pass yet because public APIs and periodic
updates remain incomplete.

## MLFQS nice API

Status: implemented and build-checked; full MLFQS behavior still depends on
the periodic scheduler updates.

`thread_get_nice()` returns the current thread's stored `nice` integer.
`thread_set_nice()` validates the range -20 through 20, changes the current
thread's value with interrupts disabled, and immediately recalculates its
priority when MLFQS is enabled. It sorts and examines `ready_list`, records
whether a strictly higher-priority thread is ready, restores the previous
interrupt level, and yields when required. In normal priority mode, storing a
nice value does not alter manual or donated priority.

The normal build succeeds, `git diff --check` reports no whitespace errors,
and the existing `priority-preempt`, `priority-change`, and
`priority-donate-chain` regression results remain current. The MLFQS nice
tests are not yet meaningful because recent-CPU and timer updates remain
unimplemented.

## MLFQS public value getters

Status: implemented and build-checked. Their returned values will remain zero
because nothing changes `load_avg` or `recent_cpu` until periodic updates are
added.

`thread_get_load_avg()` multiplies the system fixed-point load average by 100
and converts it to the nearest integer. `thread_get_recent_cpu()` performs the
same conversion for the current thread's recent-CPU value. Multiplication
comes before integer conversion so two decimal places are preserved. For
example, a fixed-point value representing 1.75 becomes integer 175 instead of
being rounded to 2 first. The scale of 16384 is the internal binary fixed-point
representation; 100 is the public API scale used to return two decimal places
through an integer result.

The normal Pintos build remains successful and `git diff --check` reports no
whitespace errors after these getter implementations.

## Per-tick recent CPU accounting

Status: implemented and build-verified; once-per-second recent-CPU decay is
not yet implemented.

`thread_tick()` now increases the running thread's fixed-point `recent_cpu` by
one on every timer tick when MLFQS is enabled. It uses `fp_add_int()` because
adding raw integer 1 to a 17.14 value would add only 1/16384, not one whole CPU
tick. The idle thread is excluded because idle time is not CPU usage by a
competing thread. The function already executes in timer interrupt context,
so the update does not require another interrupt-disable section.

The project builds successfully after this change. `priority-preempt`,
`priority-donate-chain`, and `alarm-priority` were freshly run with QEMU and
passed. The comment above the new block currently has two extra leading spaces
and should be aligned with the other function-body comments before commit.

## Once-per-second load average

Status: implemented and verified by `mlfqs-load-1`.

`thread.c` now includes `devices/timer.h` for `timer_ticks()` and `TIMER_FREQ`.
On exact one-second boundaries, `thread_tick()` calls
`mlfqs_update_load_avg()` when MLFQS is enabled. The helper counts threads in
`ready_list` and adds the running thread unless it is the idle thread. Blocked
and sleeping threads are not counted.

The implementation evaluates the required moving-average formula in the
algebraically equivalent fixed-point form:

```text
load_avg = (59 * load_avg + ready_threads) / 60
```

This avoids introducing separate rounded approximations for 59/60 and 1/60.
The helper requires interrupts to be disabled; it is currently called from
timer interrupt context.

The current QEMU results for `mlfqs-load-1` and `mlfqs-load-60` are PASS. The
single-thread test verifies that the load average crosses 0.5 after 41 seconds
and decays to 0.44 after ten idle seconds. The 60-thread test verifies the
ready-thread count and load calculation under a much larger competing
workload. Its earlier FAIL result was stale; rerunning it after the completed
periodic MLFQS implementation passed without another source change.
`priority-preempt` also passed after this work.

## Once-per-second recent CPU recalculation

Status: implemented and verified by `mlfqs-recent-1`.

`mlfqs_update_recent_cpu()` is a `thread_foreach()` callback. It skips the
idle thread and computes the coefficient `(2 * load_avg) / (2 * load_avg + 1)`
using fixed-point operations, then updates each thread with `coefficient *
recent_cpu + nice`. Because it traverses `all_list`, running, ready, sleeping,
and synchronization-blocked threads are all updated.

At each exact one-second boundary, `thread_tick()` first recalculates
`load_avg` and then calls `thread_foreach()` to recalculate every thread's
`recent_cpu` using that new load value. The timer handler already has
interrupts disabled, satisfying the traversal and helper requirements.

The complete 180-second `mlfqs-recent-1` QEMU test passed. This jointly
verifies per-tick charging, once-per-second timing and decay, fixed-point
arithmetic, all-thread traversal, and `thread_get_recent_cpu()` output scaling.

## Every-fourth-tick MLFQS priority update

Status: implemented, build-verified, and tested.

On every fourth timer tick, `thread_tick()` calls `thread_foreach()` with
`mlfqs_update_priority_all()` to recalculate the priority of every non-idle
thread. Because ready threads may now have different priorities, it then sorts
`ready_list` with `thread_priority_more()`. If the first ready thread has a
higher priority than the running thread, `intr_yield_on_return()` requests a
context switch after the timer interrupt finishes.

`thread_priority_more()` is defined later in `thread.c`, so a static forward
declaration was added near the other helper declarations. In C, the compiler
must know a function's declaration before the function is used as the
comparator argument to `list_sort()`.

The four-tick update remains inside `if (thread_mlfqs)`, so normal priority
scheduling is unchanged. The project builds successfully, `git diff --check`
passes, and the QEMU tests `priority-preempt`, `mlfqs-fair-2`, and
`mlfqs-nice-2` pass.

## Ignoring manual priority changes under MLFQS

Status: implemented, build-verified, and tested.

`thread_set_priority()` now returns immediately when `thread_mlfqs` is true.
MLFQS owns `priority` through its formula, so a caller-supplied priority must
not overwrite either the calculated effective priority or the normal
scheduler's `base_priority`. The guard runs before interrupts are disabled and
before any priority or donation state is changed.

The project builds successfully after this integration change. The QEMU tests
`priority-change` in normal scheduling mode and `mlfqs-nice-2` in MLFQS mode
both pass.

## Complete Project 1 regression suite

Status: all 27 supplied tests pass under QEMU.

The complete `make check` suite was run after the MLFQS integration work. All
six alarm tests, twelve normal priority/donation tests, and nine MLFQS tests
passed. This includes the larger `mlfqs-load-60`, `mlfqs-load-avg`,
`mlfqs-fair-20`, `mlfqs-nice-10`, and `mlfqs-block` workloads. No source-code
change was required while running the final suite.

Reference: [Johns Hopkins Pintos scheduler appendix, section B.6](https://jhuopsys.github.io/spring2026/assign/pintos/pintos_8.html).
The online appendix was consulted because the originally supplied local PDF
path was unavailable during the preceding review; it is supplementary and
does not replace any course-specific requirements in the user's PDF.

## Next implementation steps

Continue in small, tested stages:

1. Review and clean the edited code to match Pintos formatting conventions.
2. Complete the required `src/threads/DESIGNDOC`.
3. Run one final `make check` after any cleanup or documentation-related code
   edits, then prepare the submission commit.

## Work not yet implemented

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

The existing nested-donation code has trailing whitespace on the blank line
after the donor-list insertion in `lock_acquire()`, and its closing brace
needs alignment. These unrelated style issues were left unchanged during the
timer preemption correction.

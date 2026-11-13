# Guest scheduler session (branch `claude/guest-scheduler`)

Nothing here has been run on a device. The only check is the GitHub Actions
build of this branch (state at the end of this file). No speed-up and no fix
is claimed: every number below that is not from the handoff is a prediction
to be tested with the counters named beside it.

## Done / in progress / next

- Done: two certain lost wakes fixed (no setting) with a counter each; the
  yield storm's cheaper paths, the APC poke and an adjustable timed look,
  each behind a setting that defaults to today's behaviour; the extra
  measurements (why fibers leave their CPU, the scheduler lock's cost).
- In progress: nothing.
- Next: see "What is next" at the end.

Base: `5f8a0e9` (the architecture branch's head, which is `7648e9c` plus
documents). Two commits: `b182e29` (the lost wakes) and `c55015c` (the
settings and counters), then this file.

## What changed, in one table

| What | Setting | Default |
|---|---|---|
| Lost wake 1: the "anyone blocked" flag written after the epoch was read | none, a correctness fix | on |
| Lost wake 2: an auto-reset event's queue did not hand on to the waiter behind a front waiter that left | none, a correctness fix | on |
| `NtYieldExecution` with nothing else ready returns without a switch | `guest_scheduler_lean_yield` | false |
| A thread that yields in quick succession for 2 ms with nothing to run is parked about 1 ms at a time | `guest_scheduler_park_yield_spinners` (implies the one above) | false |
| A thread that stays in a marked spin-wait for 0.5 ms is parked about 1 ms per pass | `guest_scheduler_park_poll_loops` | false |
| Queueing an APC pokes the target's CPU; alertable waits are not polled every 1 ms | `guest_scheduler_apc_poke` | false |
| The 2 ms timed look at parked waiters' gates, now adjustable | `guest_scheduler_gate_recheck_ms` | 2 |

All are in the "Kernel" group of the config, beside `guest_scheduler_stats`.
When any differs from its default the log has one line at start:
`GuestScheduler: lean_yield=... park_yield_spinners=... park_poll_loops=...
apc_poke=... gate_recheck_ms=...`.

## The per-second lines (`guest_scheduler_stats = true`)

There were two lines a second. There are now five:

1. `GuestScheduler: repolls N/s (rereadied N, backstop rescues N), idle
   wakes N, switches N, forced preempts N | io ...` as before, except that
   "backstop rescues" now counts only those the 64 ms backstop made good.
2. `GuestScheduler: left the CPU: for a wait or sleep N, NtYieldExecution N,
   zero delay N, spin site N, slice end N | yields that found nothing to run
   and returned at once N | parked: yield spinners N, poll loops N | APC
   pokes N`. The first five are why a fiber switched to the idle fiber, so
   they add up to about `switches` (a thread's first run and its exit are
   the difference). This is the split of switches by yields against waits.
3. `GuestScheduler: scheduler lock: taken N times, N us waiting for it (N ns
   each), N us holding it (N ns each)`. Summed over all threads for the
   second. Timing it costs two or three counter reads and three shared
   atomic adds per take, only with the stats on, so the line slightly
   overstates the lock.
4. `GuestScheduler: missed wakes: made good by the timed look N, by the
   64 ms backstop N, satisfied with no wake sent N | not misses: queue
   hand-ons N, signals caught at the park N`.
5. `GuestScheduler: wakes by delay after the signal: ...` as before.

## Task 3 first: the missed pokes

### What the earlier numbers were

Run 39's "six backstop rescues in four minutes" were counted before the
rule that a signal under half a millisecond old is a poke still on its way.
A signaller bumps the epoch and only then takes the scheduler lock to poke
(with a clock read and the signal ring's mutex in between), so a timed pass
on the waiter's CPU that lands in that gap sees "epoch moved, pass not
poked". With six CPUs making a forced pass every 64 ms and thousands of
signals a second, a few of those a minute is what chance gives. Run 43, the
first with the half-millisecond rule, had no rescue at all in the whole run
(handoff, "Right now"). So the measured "one missed poke a minute" is most
likely this artefact, not a lost wake. That is a reading of the code and of
two runs, not a proof.

The reading did find two real ways to lose a wake. Neither moves the count
above, which is why they were invisible.

### Hole 1 (fixed, certain): the flag for the lock-free test, written too late

`WakeForSignal` first asks, without the lock, whether any CPU has a blocked
waiter (`has_blocked`), and returns if none has. `BlockCurrentThread` read
the gate's epoch under the lock and stored `has_blocked` at the end of the
same block. The comment said "at least one side always sees the other", but
that needs each side to write before it reads, and the waiter read first:

1. Waiter W, under the lock, reads the object's epoch: unchanged.
2. Signaller S sets the host primitive and bumps the epoch.
3. S reads `has_blocked` on all six CPUs: all false. S returns, no poke.
4. W links itself into the blocked list and stores `has_blocked = true`.

W is parked on a signalled object until the timed look (2 ms) or the
backstop (64 ms). It needs every CPU's blocked list to be empty at step 3,
so with a title's thirty threads it is rare; early in boot it is not.

Fix: `has_blocked` is stored first thing under the lock, before the epoch is
read. No counter of its own (the window cannot be seen from inside); a case
that slipped through would show as "made good by the timed look".

### Hole 2 (fixed, certain): an auto-reset event's queue did not hand on

Auto-reset events keep a queue of cooperative waiters and only the front may
take a set (`XEvent::CooperativeMayAcquire`). The semaphore and the mutant
wake the queue when a waiter leaves it. The event did not:

1. A is first in the queue and has taken set 1. It has not left the queue
   yet (it is between its successful poll and `LeaveCooperativeWait`, and
   its host thread can be descheduled there).
2. Set 2 arrives: the epoch moves and B, behind A, is re-readied.
3. B polls. A is still first, so B may not take the set. B parks with the
   epoch of set 2.
4. A leaves the queue. Nothing moves the epoch, nothing pokes B's CPU.

The event is signalled, B is first in line, and B stays parked until the
64 ms backstop. The 2 ms timed look does not help (it only compares epochs,
and the epoch did not move) and the rescue counter does not count it (same
reason). The same happens when the front waiter leaves on a timeout or an
APC without taking the set.

This matters for Fable II: a contended critical section waits on exactly
such an event, and since the fix in `xboxkrnl_rtl.cc` a contender is in the
event's queue before it raises `lock_count`. A thread kept 64 ms from a free
lock is two frames. Whether this is what ran the worker late on any of the
stalled runs is not known.

Fix (`xevent.cc`, `xobject.cc/.h`): when the waiter that leaves was the
front of an auto-reset event's queue and others remain, the epoch is bumped
and the watchers' CPUs are poked (`WakeCooperativeWaitersForHandOn`; it stays
out of the signal ring, which is read as "who set what"). Counter: "queue
hand-ons". Cost: one extra re-poll of the next waiter per departure from a
queue of two or more.

### The counter that would have shown hole 2, and shows any other like it

"satisfied with no wake sent": a wait that parked, was re-polled and found
its object takeable although its gate's epoch was where it was at the park.
That can only be a state change that sent no wake. Each is logged
(`GuestScheduler: wait satisfied with no wake sent: tid=... handle ...`,
200 lines at most). With hole 2 fixed it should read nought. A steady
non-zero count names another path; the handle in the log line says which
object. A stray one is possible when a re-poll lands in the few tens of
nanoseconds between a set and its epoch bump (the code waits briefly for
the bump before counting, so this should be very rare).

### Read and found sound

- `WakeForSignal` against `BlockCurrentThread`: the waiter's compare of the
  epoch and the signaller's walk of the blocked lists are under the same
  lock, so one of them sees the other. Counter "signals caught at the park"
  is how often the waiter's side did.
- The CPU's park and recheck in `RunLoop`: `parked` is stored before the
  queues are checked under the lock, and wakers set their flag under the
  lock before they read `parked`. Sound for `EnqueueReady`, `WakeAll`,
  `WakeForSignal`, `RereadyBlocked`'s cross-CPU wakes.
- A multi-wait's summed epoch: each member's epoch only grows, and the poll
  comes after the sample, so a bump before a member is read is seen by the
  poll and a bump after it is seen by the compare at the park. Reading the
  members while one is bumped cannot hide a signal.
- A wait set registered after the epoch was sampled: no. The single wait
  registers (`EnterCooperativeWait`) and the multi-wait records its set
  before `CooperativeWait` takes its first sample.
- Signals from host threads (audio, GPU, timers): they go through
  `XEvent::Set` and so through `WakeCooperativeWaiters`, which is safe from
  any thread.
- Every caller that changes an event, semaphore or mutant: `XEvent::Set`
  and `Pulse`, `XSemaphore::ReleaseSemaphore`, `XMutant::ReleaseMutant` and
  `AbandonAllOwnedByThread` all wake. `XEvent::Reset` and `Clear` cannot
  satisfy a waiter.

### Found, not mine to fix (files owned by nobody or by another session)

- Timers and thread objects do not wake cooperative waiters at all. A
  thread waiting on a timer or joining a thread is not gated: it is
  re-polled every 1 ms, which is correct but is one of the things that
  keeps a dispatch thread on a 1 ms timer. The change, for the merge:
  in `kernel/xtimer.cc`, after the host timer fires and before the APC is
  queued, call `WakeCooperativeWaiters()` on the timer; in
  `kernel/xthread.cc`, where a fiber thread's exit event is set, call
  `WakeCooperativeWaiters()` on the thread; then add `Type::Timer` and
  `Type::Thread` to the gated types in `BlockCurrentThread`,
  `XObject::WaitMultiple` (`gateable_set`) and `CooperativeWait`
  (`epoch_tracked`). Do all of it or none: gating a type whose signal does
  not wake would turn a 1 ms poll into a 64 ms one.
- A kernel-mode APC queued to a parked thread is not delivered until the
  thread next enters a kernel call on its own (`xeProcessKernelApcs` runs
  at the entry of waits and yields, not inside `CooperativeWait`). The
  console would run it at once. Not changed, not known to matter.

## Task 1: the yield storm

### What a yield costs today

A yield that does switch: `EnqueueReady` (lock), a fiber switch to the idle
fiber, the dispatch loop (a millisecond clock read, the poke flag,
`DequeueReady` under the lock), `SwitchTo` (lock, a counter read, the
thread-local writes of `SetCurrentThread`, a fiber switch, the lock again
on the way back). Four takes of the one lock, two fiber switches, two clock
reads.

A zero delay (`KeDelayExecutionThread` with 0 or under 1 ms, which
`XThread::Delay` rounds to a plain yield) with nothing else ready already
returned without any of that. `NtYieldExecution` did not: it asks for a
quantum end, and the short way out was only for calls that do not. So every
`NtYieldExecution` with nothing to run paid the whole round trip to resume
the same fiber. Fable II's busy game thread is such a loop (try the lock,
`NtYieldExecution`, again), which is the 13% and most of the 370,000
switches a second. Line 2 above now says how many switches each cause makes,
so this can be checked on the phone before anything is switched on.

### `guest_scheduler_lean_yield`

`NtYieldExecution` takes the same way out as a zero delay when: the caller
is not marked preempted or suspended, its `preempt_requested` is down, its
CPU's ready queues are empty and no poke is waiting. No lock, no switch, no
clock read.

Why it is safe:
- The answer is the same. The round trip would have re-run the same fiber
  and returned `NO_YIELD_PERFORMED`; so does this. The priority decay of a
  quantum end is still applied.
- Something becoming ready is not missed: a thread made ready on this CPU
  sets the ready summary under the lock, a poke sets `repoll_now`, a
  higher-priority wake sets `preempt_requested`; the next call sees any of
  them and takes the full path. A stale read costs one call.
- The dispatch loop still runs. The slice is not renewed by a lean yield,
  so within one slice (1 ms) the watchdog raises `preempt_requested`, the
  next yield takes the full path and the loop makes its timed re-polls
  (sleep deadlines, ungated waiters, the counters). So timed waiters on
  that CPU are looked at once a slice, as they are for a thread that does
  not yield at all.
- A title that yields for timing gets a call that returns sooner, as it
  does on the console when nothing else is ready.
- The watchdog's "CPU has not switched fibers" report counts these yields
  as progress, or a lone yielding thread would be reported as stalled.

What to compare: `switches` and "left the CPU: NtYieldExecution" should
fall to about a thousand a second per spinning thread, "yields that found
nothing to run" rises by what they lost, "scheduler lock: taken" falls by
about four per switch saved. Frame rate and the wake delay line should not
get worse.

### `guest_scheduler_park_yield_spinners`

A run of empty yields (`NtYieldExecution` or a zero delay) by one thread,
with nothing else dispatched on its CPU, coming at an average of 20 us or
closer (the clock is read every 32nd call), that has lasted 2 ms, parks the
thread: a timed sleep to the next millisecond but one, about 1 ms. After
it, 32 more quick empty yields park it again; a slower cadence or anything
else running there starts the run's age afresh.

The bound: the park is between 1 and 2 ms by the millisecond clock and in
practice about 1 ms plus the host's timer slack. It grows in one step, from
nothing for the first 2 ms to about 1 ms, because the dispatch thread's
sleep (`xe::threading::Wait`) only takes whole milliseconds; steps of 50
to 500 us need a finer wait, which is in `base/threading.h` (another
session's file).

Why it is safe:
- The park is on the scheduler's blocked list with a deadline, so the
  dispatch thread sleeps in its ready event: a thread made ready on that
  CPU, or a poke, wakes the dispatch thread at once and runs. Only the
  spinner itself waits out the millisecond.
- A flag set by a thread on another CPU is seen by the spinner within about
  a millisecond, and only after it has already spun 2 ms for it.
- A thread that does work between its yields is never parked (the 20 us
  cadence), nor is one that yields inside a wait, at DISPATCH_LEVEL or
  under the global lock. Kernel spin locks (`SpinYield`) are never parked
  by this path.
- For a title that paces frames with a yield loop, a call can now take
  about a millisecond once the loop has run 2 ms. On the console a yield
  that does switch takes as long as the other thread runs, so no title can
  rely on less; but a pacing loop will overshoot by up to a millisecond,
  which is why this is its own setting.

What to compare: "parked: yield spinners" (parks a second), `idle wakes`
up by about the same, the busy dispatch thread's CPU time down
(`/proc/<pid>/task/<tid>/schedstat`), frame rate unchanged. If Fable II's
stall or its late wakes get worse with it, switch it off first.

### `guest_scheduler_park_poll_loops`, and why `sub_82B9BF90` is not parked today

The 00:55 profile asks why the 3D Engine thread's spinning wait
(`sub_82B9BF90`, 52.7% of guest CPU 3) is not parked. From the code:

- The translator marks spin-waits with a backoff site (`MemoryPollParkPass`
  for load, test, branch-back loops; the spin-backoff collapse passes).
  Without this scheduler the site sleeps 30 us at a time after 24 passes.
  With it (`a64_seq_memory.cc`) the site does nothing unless
  `preempt_requested` is up, and then calls the scheduler's yield.
- The loop's own safepoint sits at its head and tests the same flag first
  on nearly every pass, clears it and does a slice-end yield. So the
  backoff site is hardly ever reached with the flag up.
- Either way the yield finds nothing else ready and returns. The loop
  spins its dispatch thread flat out for as long as the wait lasts.

So under the cooperative scheduler no marked spin-wait has ever been
parked; they only give way to other ready fibers. Whether `sub_82B9BF90`
is a marked loop at all cannot be told from here: the log of a run has a
line `MemoryPollPark: parked poll loop at guest 82B9BFxx` if it is. The
dumps show `lr=82B9BF98`, a return address inside the function, so it
calls something at `82B9BF94`; a loop with a call in it is rejected by
the pass (`log_memory_poll_park = true` prints why). If it is rejected,
this setting does nothing for it and the fix is in the pass or a title
patch (the CPU translation session's files).

What the setting does, without touching the translator: after a slice-end
yield the scheduler raises the flag again once. If a safepoint sees it
next, the thread is in ordinary code and that is the end of it (one extra
cheap call a slice). If a backoff site sees it, the thread is in a
spin-wait: the flag is then kept up so that every pass of the loop calls
in, from its safepoint and its backoff site in turn; two safepoints running
mean it left the loop. A thread seen in the loop for 0.5 ms is parked about
1 ms per pass until it leaves. Counter: "parked: poll loops".

Why it is safe:
- The loop is not changed: it still tests what it polls on every pass, at
  most about a millisecond apart once parked, and a wait under 0.5 ms (plus
  up to a slice to be noticed) is not slowed at all.
- The park is the same interruptible sleep as above.
- While watched, the thread gives way exactly as before to anything ready,
  to pokes and at the end of its slice (timed here, since the watch holds
  the flag up).
- A loop with two safepoints a pass is never counted as a spin-wait and is
  left alone.

Risk: a wait that the console resolves in well under a millisecond but
that here regularly lasts over 0.5 ms gains up to a millisecond. For a GPU
wait once a frame that is up to 3% of a 33 ms frame. Compare frame rate
and the 3D Engine dispatch thread's CPU time with it on and off.

## Task 2: the timed sleeps

### `guest_scheduler_apc_poke`

Every path that queues an APC ends in `xeKeInsertQueueApc`
(`xboxkrnl_threading.cc`), reached two ways:

- `xeNtQueueApcThread`: the `NtQueueApcThread` export and
  `XThread::EnqueueApc`, which is used by timers (`xtimer.cc`), overlapped
  I/O completion (`kernel_state.cc`), the three completion APCs in
  `xboxkrnl_io.cc` and `xam_media.cc`. It poked every CPU (`WakeAll`).
- The `KeInsertQueueApc` export, which told the scheduler nothing: a
  parked alertable target found its APC on the next 1 ms look.

With the setting on, both call `WakeForApc`, which pokes the one CPU the
target is parked on. A target that is running or queued needs no poke: it
looks at its queue before it parks, and `BlockCurrentThread` now looks
again under the scheduler lock as it parks, which closes the gap between
those two (the APC is in the queue before `WakeForApc` takes the lock, so
either the poke finds the thread parked or the park finds the APC).
Alertable waits on events, semaphores, mutants and gateable sets, and
alertable sleeps, then have no 1 ms cadence. The 64 ms backstop stays.

What to compare: "APC pokes" a second, `repolls` and `idle wakes` down on
CPUs whose parked threads are alertable. If a title's file I/O or timers
stall with it on, an APC path was missed: switch it off and say which.

### What still needs a timer

- The 2 ms timed look itself (`guest_scheduler_gate_recheck_ms`). With any
  gated waiter parked and nothing running, this alone wakes the dispatch
  thread every 2 ms; it is the "25% of the worker's dispatch thread in the
  timed sleep's syscall" of run 43. Once "made good by the timed look"
  and "satisfied with no wake sent" have stayed at nought over long play,
  set it to 0 (the 64 ms backstop remains) or to 16.
- Waits that are not gated and so are polled every 1 ms: on timers, thread
  objects (joins), files and any other type; multi-waits of more than
  eight objects or with such a type in the set; parks on a host fence;
  the offloaded I/O park between the worker's pokes. The change for timers
  and threads is described above.
- Real deadlines (timed waits and sleeps): a timer by nature, already one
  sleep to the deadline.
- The 64 ms backstop.
- The watchdog thread's own 1 ms tick (slice ends).

## Upstream files changed

All under `x360/emulator-core/src/main/cpp/xenia/src/xenia/kernel/`, all in
this session's row of the ownership table, every change marked
`hakuX-Boosted:`.

- `guest_scheduler.h`, `guest_scheduler.cc`: the settings; `TimedLock`
  around the scheduler lock; `YieldCurrentThread` gains a cause and the
  lean and park paths; `NoteEmptyYield`, `ParkSpinner`, the `PollWatch*`
  trio; `WakeForApc`; `has_blocked` stored before the epoch is read; the
  timed look's period from the setting; the new counters and lines; the
  watchdog counts empty yields as progress.
- `xobject.h`, `xobject.cc`: `CooperativeWaiterFifo::Remove` reports
  whether the leaver was the front; `WakeCooperativeWaitersForHandOn`;
  `CooperativeWait` counts a wait satisfied with no wake sent.
- `xevent.cc`: the hand-on in `CooperativeWaitEnd`.
- `xboxkrnl/xboxkrnl_threading.cc`: `NtYieldExecution` names itself to the
  scheduler; `xeNtQueueApcThread` and `KeInsertQueueApc` call `WakeForApc`.

`xsemaphore.*`, `xmutant.*`, `xevent.h` are unchanged. `xthread.cc` (not
mine) is unchanged: its zero-delay call uses the new parameter's default.

## What could not be verified without a device

Everything about behaviour. In particular:

- That the two fixes change anything a player would see, or the stall.
- That any setting saves time; the yield path's share (13%) is the most
  that lean yield can win on that thread, and the park settings trade CPU
  time for up to a millisecond of latency on the parked thread.
- The poll-loop watch depends on how the translator lays out a marked
  loop (safepoint at the head, backoff site before the branch). Read from
  the passes and the emitter, not observed. If "parked: poll loops" stays
  at nought on a title known to spin, either its loop is not marked or the
  watch does not see the alternation.
- The thresholds (2 ms, 20 us, 0.5 ms) are first guesses, as constants at
  the top of `guest_scheduler.cc`.
- With the settings off the paths are today's apart from the two fixes, a
  few extra counters, and one extra re-poll per hand-on. That is by
  reading, and the syntax of the changed files was checked with the host's
  clang; the phone's build is the Actions run.

## Suggested order on the phone

1. Stats on, all settings off: read line 2 (which cause makes the
   switches) and line 4 (should be all nought but the two "not misses").
2. `guest_scheduler_lean_yield`.
3. `guest_scheduler_apc_poke`, then `guest_scheduler_gate_recheck_ms = 0`
   if line 4 has stayed clean.
4. `guest_scheduler_park_poll_loops`, then
   `guest_scheduler_park_yield_spinners`, one at a time, watching the wake
   delay line and Fable II's stall.

## What is next

- Timers and thread exits waking their waiters (the change above), so that
  joins and timer waits need no 1 ms poll.
- A dispatch-thread sleep in microseconds, so the parks can grow in steps
  from 50 us instead of jumping to a millisecond.
- The lock: line 3 will say whether one lock for six CPUs is contended
  enough to split per CPU. `SwitchTo` takes it twice a dispatch only to
  write per-CPU fields.
- The scheduler's per-thread state behind one pointer instead of
  `thread_local` (the host-thread-state session's work) would take the
  rest of the yield path's cost.

## Build
EOF

- `b182e29` (the two lost wakes): Actions run 53, green.
- `c55015c` (the settings and counters) with these notes (`189880e`):
  Actions run 66, green. The commit after it only records this result.

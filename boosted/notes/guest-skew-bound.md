# Guest skew bound (branch `claude/guest-skew-bound`)

Nothing here has been run on a device. The only check is the GitHub Actions
build of this branch (state at the end of this file) and a syntax check of
the changed files with the host's clang. No fix and no speed-up is claimed:
whether this stops Fable II's freeze, and what it costs in frame time, is
for the phone to say with the counters named below.

Base: `claude/integration-1113` at `d940365`. Pushed only to
`claude/guest-skew-bound`.

## Done / in progress / next

- Done: the start gate and the progress gate behind
  `guest_scheduler_skew_bound_us` (0 = off, the default, today's
  behaviour), the hard cap, the counters on the per-second lines, a line
  per long hold, the hold named in late wake lines, the totals in the
  thread dump. Built green by Actions, run 80 (see "Build").
- In progress: nothing.
- Next: the device runs of "What to run on the phone". Then, by what they
  show, one of the items in "What is next".

## Settings

All in the `[Kernel]` table of the config, beside `guest_scheduler_stats`.
None is in the settings screen.

| Setting | Default | What |
|---|---|---|
| `guest_scheduler_skew_bound_us` | 0 (off) | The bound, in microseconds. 0 leaves every path as it was. Try 500 and 2000. |
| `guest_scheduler_skew_cap_multiple` | 20 | The longest one hold may last, as a multiple of the bound: 10 ms at 500, 40 ms at 2000. |
| `guest_scheduler_skew_progress_gate` | true | Only read with the bound set. False leaves the start gate alone, to price the two gates apart. |

With the bound set the log has one line at start: `GuestScheduler: skew
bound N us, one hold at most N us, progress gate on|off, judged every N us
(the watchdog tick)`. If instead it says `the skew bound is off`, the
watchdog did not start (no timeslice) and nothing below happens.

Set `guest_scheduler_stats = true` for every run: the per-second line is
only printed with it.

## The problem, in one paragraph

Each of the console's six hardware threads is a host thread here (`Guest
CPU N`), and guest threads are fibers on them. The console's six all run at
once. The phone, when hot, lowers core frequency and packs the emulator's
threads onto fewer cores (run 73: four dispatch threads on one core, each
waiting about 200 ms of every second for it). A dispatch thread that is not
given a core stops its guest CPU dead while the other five run on. That
reorders the title's threads by tens of milliseconds, which the console
never does and titles rely on. Fable II's main thread starts a worker with
an event at the end of one frame's locked update and comes back for its
result in the next frame's, holding a lock the worker's tasks need; the
worker has always long finished on the console. On run 73 the worker ran
64.0 ms after its start event, on earlier builds 137 ms and 179 ms, and the
title deadlocked each time. Priority (nice -8) lowers the odds; it cannot
stop the phone from doing it.

## The design

The rule: a guest thread must not run far ahead of a guest CPU that should
be running and is not. "Should be running" comes in two kinds, and there is
one gate for each.

### How a CPU is known to be behind

Nothing was added to translated code. Two cheap records:

- **The probe.** Every watchdog tick (1 ms by default,
  `guest_scheduler_quantum_us`) each CPU that has a running fiber gets the
  time written to its `skew_probe_tick` (unless a probe is still
  outstanding) and the fiber's `preempt_requested` flag is raised. The
  fiber answers by storing 0 at the next place it passes: a JIT safepoint,
  any yield, a spin-wait's backoff site, or by leaving the CPU. At the
  safepoint, if the flag was up for the probe alone, the handler returns at
  once: one extra cheap call per running thread per tick. A probe that
  stays unanswered for longer than the bound means that host thread is not
  reaching guest code: it is descheduled, or it is deep in host code. That
  CPU is **stalled**.
- **The debt.** A guest thread whose signal readies a thread on another
  guest CPU records, for that CPU, the time and the CPU's `skew_serve_seq`.
  That counter moves each time the CPU's dispatch thread dispatches a fiber
  and each time it finds, under the scheduler lock, nothing to run and no
  poke pending. Until it moves, the signal has not been acted on. The debt
  is in the signaller's own `SchedulerLinks` (one bit per CPU), written by
  the signaller under the lock and read only by the signaller. It holds an
  id of the readied thread, never a pointer.

What counts as a signal that readies: an event set, a semaphore release, a
mutant release (all through `WakeForSignal`, and a critical section's
hand-over is an event set), a resume and a new thread's first queueing
(`EnqueueReady`). Only signals from a guest fiber: a host thread (audio,
GPU, timers) has nobody to hold. APC pokes and `WakeAll` are not counted.

### The start gate

A thread with a debt on CPU *c* that the CPU has not acted on is held when:

- no fiber runs on *c*, and that has lasted longer than the bound, counted
  from the signal or from when *c*'s last fiber left, whichever is later.
  This is the measured case: the dispatch thread was woken and has not
  turned up; or
- a fiber runs on *c* and *c* is stalled.

If a fiber runs on *c* and answers its probes, nothing is held: the readied
thread waits its turn behind that fiber, as it would on the console. Only
the signaller waits; the other CPUs run on. The hold ends when *c* makes a
scheduling decision (in the first case) or also when it answers (in the
second).

### The progress gate

Any running thread is held while another CPU is stalled: it has a running
fiber whose probe has gone unanswered for longer than the bound. This is
the worker that started and whose host thread was then descheduled. Every
running thread waits, not only one. The hold ends when that CPU answers its
probe or its fiber leaves the CPU. `guest_scheduler_skew_progress_gate =
false` switches this gate off alone.

### Where and how a thread is held

- **Where.** At a JIT safepoint and at the entry of `NtYieldExecution`,
  once per probe, and only where a guest sleep could stand: not inside a
  wait, below DISPATCH_LEVEL, not under the global critical region (the
  test is `CanParkSpinner`, the one the yield spinner's park already uses).
  Any other yield (a zero delay, a kernel spin lock, a backoff site)
  answers the probe but may be kernel code in the middle of something, so
  the look is owed and is made at the thread's next JIT safepoint. A thread
  at raised IRQL or under the global lock answers its probe and is never
  held: it is the one others may be waiting for.
- **How.** The fiber stays the running fiber of its CPU and its host thread
  sleeps on a condition variable, woken by the awaited CPU when it acts or
  answers. It also looks again every 200 us on its own. Nothing else on the
  holder's CPU runs meanwhile, so that CPU stands still in guest time,
  which is the point. A hold ends early when something asks the held thread
  to give way: a thread that outranks it becoming ready on its CPU, a
  terminate, a suspend, shutdown. The thread then yields as it would have
  and looks again when it next runs. A held thread's time slice is extended
  by the hold, since it did not run.
- **The cap.** One hold lasts at most the bound times
  `guest_scheduler_skew_cap_multiple`. After it the thread runs on, and the
  same lapse is not waited for again: the debt is dropped, and the probe
  that stayed unanswered is marked so that no thread waits for it any more.
  A host thread that is truly stuck costs each other thread one cap, not a
  hang.
- **Only runnable thread.** A hold only ever waits for a CPU that has a
  running fiber (progress gate) or a thread just readied on it (start
  gate), so a thread with nothing else runnable anywhere is never held.

### Why holding, and not running the readied fiber elsewhere

The brief asked to consider letting an idle dispatch thread, or the
signaller's own, run the readied fiber. I chose to hold, for four reasons:

1. A guest thread carries its guest CPU number, and titles and the kernel
   keep per-processor data without a lock. If CPU 1's worker ran on CPU 0's
   host thread while CPU 1's own host thread came back and ran another
   CPU 1 thread, two threads of one guest CPU would run at once. Changing
   the worker's CPU number instead is a migration the guest can see and
   that its affinity forbids. `ignore_thread_affinities = true` does not
   lift this: it stops the host's affinity being set, the guest's
   `current_cpu` still picks the dispatch thread.
2. On the signaller's own host thread the signaller and the readied thread
   would run one after the other, not side by side: a different reorder,
   not the absence of one.
3. An idle dispatch thread is a sleeping host thread that needs the same
   wake-up, on the same crowded cores, as the one that is late.
4. It does nothing for the progress gate: a fiber that is mid-run on a
   descheduled host thread cannot be moved.

Holding also has a property that helps on a hot phone: the holder's host
thread sleeps, which frees its core for the thread that is late. On run 73
GameThread's and the worker's dispatch threads were on the same core.

The better form of the idea is under "What is next": let a holder's host
thread take over the late CPU's whole dispatch loop, one host thread per
guest CPU at a time.

### The bound is judged once a tick

Probes are raised on the watchdog's tick and each thread looks once per
probe. So a thread runs on for the bound plus up to one tick (1 ms) before
it is held, and a stall is seen at the first tick after it is older than
the bound. In practice 500 means "behind for 1 to 1.5 ms" and 2000 means
"behind for 2 to 3 ms". Both are far below the 64 ms that broke Fable II.
With the bound on, the watchdog thread runs at nice -12 (above the dispatch
threads' -8), because a late watchdog means late probes; a hold, once
begun, ends without the watchdog.

## Which bound to try first

- **2000 first.** The freeze needs a skew of tens of milliseconds; a bound
  of 2 to 3 ms removes it with room to spare, and it ignores the short
  lapses that are normal on a phone. Run 43 had 1,576 of 40,544 wakes later
  than 0.25 ms, most of them under 4 ms: at 2000 most of those hold nobody.
  This is the setting most likely to cost nothing visible.
- **500 second.** As tight as the 1 ms tick allows. It holds on most
  lapses over about a millisecond, so it shows the cost of a tight bound:
  if frame time is the same as at 2000, tight is free and safer; if it is
  worse, the per-second line says by how much (held us per second).
- **The cap matters as much as the bound for the freeze.** A hold ends at
  the cap whether or not the late CPU has turned up: 10 ms at 500, 40 ms at
  2000. The measured stalls were 64, 137 and 179 ms. If a host thread
  really stays off a core that long while the signaller sleeps, the cap
  ends the hold first and the reorder happens anyway, only smaller. I
  expect the hold itself to shorten the stall (the holder gives up its
  core), but that is a prediction. So: if `ended by the cap` is not nought
  and the freeze returns, raise `guest_scheduler_skew_cap_multiple` (100
  with 2000 is 200 ms) before concluding anything about the bound.

## What to run on the phone

Each run: `guest_scheduler_stats = true`, `host_perf_log = true`, cold
cache as for run 73's baseline, play until the phone is hot (the freeze
came after about six minutes when hot).

1. Baseline, bound 0. Check the log has no `skew bound` line at all.
2. `guest_scheduler_skew_bound_us = 2000`.
3. `guest_scheduler_skew_bound_us = 500`.
4. If the holds cost frame time: 2000 with
   `guest_scheduler_skew_progress_gate = false`, to see which gate the cost
   is in.
5. If holds end at the cap: 2000 with
   `guest_scheduler_skew_cap_multiple = 100`.

### Log lines to compare

The new per-second line (only with the bound set):

```
GuestScheduler: skew bound: holds N (start gate N, progress gate N), held N us in all, longest N us (tid=XXXXXXXX 'name' on cpu A waited for cpu C tid=YYYYYYYY 'name', start|progress gate[, ended by the cap]), ended by the cap N, cut short by a preemption N | awaited host threads on a core for N us of the holds | signals that readied a thread on another CPU N | wakes that waited behind a hold N (N us)
```

- `holds`, by gate: holds started in that second.
- `held N us in all`: the frame time cost, summed over all CPUs. 33,000 us
  a second on one thread is about one frame's worth of that thread's time.
- `longest ... (who waited for whom)`: the longest hold of the second, the
  thread that waited, the CPU and thread it waited for, and the gate. For
  Fable II the line to look for is GameThread on cpu 0 waiting for the
  worker (`XThread001B` on run 73) on its CPU, start gate.
- `ended by the cap`: should be nought. Each one is a lapse longer than the
  cap, so a reorder the bound did not prevent.
- `cut short by a preemption`: a hold ended by a higher-priority thread on
  the holder's CPU. Normal in small numbers.
- `awaited host threads on a core for N us of the holds`: of the time held,
  how long the awaited dispatch thread was actually on a core (its thread
  CPU-time clock). Near nought means the phone had descheduled it, which is
  what the bound is for. Near `held` means it was running host code:
  translation of new guest code, or a long kernel call. That is the cost to
  watch in the first minutes of a title.
- `signals that readied a thread on another CPU`: debts recorded. Almost
  all are answered within microseconds and hold nobody.
- `wakes that waited behind a hold`: see "Late wakes" below.

Each hold of 4 ms or more, and each that ended at the cap, also has its own
line (400 at most):

```
GuestScheduler: skew hold, tid=... '...' on cpu A waited N ms for cpu C (start gate, it had not dispatched the thread this one readied | progress gate, its running thread showed no progress: tid=... '...'; that CPU was N ms behind in all, its host thread on a core for N ms of the hold)[, ended by the cap|, cut short] (frame N)
```

Lines that exist already, and what to expect of them:

- `GuestScheduler: late wake, tid=... ran N ms after the signal` and the
  `wakes by delay after the signal` line: **these will not fall.** The
  bound does not make a late thread any earlier; it keeps its signaller
  from running ahead. With the bound on, each late wake of a thread readied
  by a guest thread on another CPU should have a `skew hold` line of about
  the same length beside it, at the same frame. A late wake of 16 ms or
  more with no hold beside it is one the gates do not cover (see "Not
  covered").
- `waited N ms for critical section` and the `critical sections: N
  contended` totals: for Fable II's section `83499138` these should not get
  worse. If the worker now starts on time relative to GameThread, long
  waits for that section should become rarer.
- The freeze itself: the no-frame report (`no guest frame presented in ...
  watchdog ticks`) with GameThread on the done event and the worker on the
  section's event. The thread dump now has a `skew bound:` totals line and,
  under each running CPU, `skew bound: in a hold|not in a hold, probe
  answered|unanswered for N ms`. A freeze with CPUs `in a hold` would be
  this change's fault; every hold is capped, so that should be impossible
  for longer than the cap.
- Frame rate from `host perf`: median, tenth percentile, seconds with a
  frame over 100 ms, against run 73's baseline (median 30, tenth percentile
  28).
- `GuestScheduler: left the CPU: ... slice end N`: with the bound on, a
  yield spinner takes one full yield per tick for the probe, so `switches`
  and `NtYieldExecution` round trips may rise by about a thousand a second
  per spinning thread. That is expected.
- `forced preempts` in the first per-second line should not change. If it
  rises with the bound on, a probe is being counted as a declined
  preemption, which `SkewOnlyProbed` is there to prevent.

## Late wakes and `NoteWakeLatency`

A late wake is measured in `ReportWakeLatency` (`xobject.cc`): the time
from the object's last signal (`cooperative_signal_time_us`, stamped in
`WakeCooperativeWaiters`) to the waiter's successful poll, counted only if
the signal came after the waiter first parked. `NoteWakeLatency` puts it in
the histogram and logs each of 16 ms or more.

A hold meets this in two ways:

1. The readied thread is late, its signaller is held. The latency is real
   host time and is counted as it was. Nothing is subtracted: the wake was
   late, and the histogram should say so. What is new is the hold line
   beside it.
2. A hold freezes the holder's whole CPU. A thread parked on that CPU and
   signalled during the hold (by a host thread, or from another CPU) runs
   late because of the bound, not because of the host, unless it outranks
   the held thread (then it cuts the hold short within 200 us). That is a
   cost of this change and must not be read as the phone's fault. So
   `NoteWakeLatency` now works out how much of each wake's delay overlaps
   the last hold on its CPU. It counts those in `wakes that waited behind a
   hold N (N us)` and adds `, N ms of it behind a skew hold on its CPU` to
   the late wake's line. Only the most recent hold on the CPU is compared,
   so with several holds inside one wake's delay the figure is low.

With the bound at 0 the late wake line's text is exactly what it was.

## Risks

### Frame time

- Every hold is time a guest thread did not run. The start gate holds one
  thread, for as long as the host really was late; that time is not lost
  twice if the title would have waited for that thread anyway. The progress
  gate holds every running thread, so one late host thread costs up to five
  others the same time.
- **Host code looks like a stall.** A fiber that is translating new guest
  code, or is in a long kernel call, does not reach a safepoint and does
  not answer its probe. Past the bound the progress gate holds the other
  CPUs for it, up to the cap, once per probe. The profile of run 43 had
  about 6% of GameThread's time in the translator in the first minutes. So
  the first minutes of a title, and any scene with new code, are where the
  progress gate will cost most. The `on a core` figure separates this from
  real descheduling. If it is most of the hold time, the choices are the
  start gate alone, a larger bound, or the change under "What is next".
- A hold delays lower and equal priority threads parked on the holder's
  CPU (above). The audio threads have high priority and should cut a hold
  short, but if `XThread0009` shows `behind a skew hold` in its late wakes,
  that is this.
- A yield spinner makes one full round trip through the dispatcher per
  tick while the bound is on (the probe's flag sends it the long way once).
  Small beside what it does already.
- Probes cost one handler call per running thread per millisecond.

### Could the scheduler deadlock itself

No, by construction, and the cap bounds it even if an assumption fails:

- A hold takes no lock while it sleeps except `skew_mutex_`, inside the
  condition wait. That mutex is a leaf: it is never taken under the
  scheduler lock, and nothing is taken under it. The scheduler lock is
  taken only briefly, before and after the sleep.
- A CPU whose fiber is in a hold counts as on time: it is never stalled and
  never the target of a hold. Every hold waits for a CPU that is not
  holding, so there is no cycle of holders, and six CPUs cannot all wait
  for each other.
- What a hold waits for is that a host thread shows life (a dispatch, a
  verified idle pass, an answered probe, a fiber leaving), never that the
  guest program gets anywhere. So a guest lock the held thread owns cannot
  keep the hold from ending: the awaited thread ends it by starting to run,
  even if it then blocks on that very lock. That is Fable II's case.
- A held thread is at a JIT safepoint or at the entry of
  `NtYieldExecution`. It holds no host lock there; every fiber switch at a
  safepoint already assumes so, and the one recursive lock that can be held
  there, the global critical region, is tested. If some path broke this,
  the awaited thread would be stuck behind the held one until the cap, once.
- A thread at raised IRQL or under the global lock is never held, so a
  spin lock's holder or the global lock's holder cannot be frozen while
  others wait for it.
- Terminate, suspend and shutdown end a hold within 200 us; shutdown's
  join loop is as before.
- The watchdog never waits for a holder. If the watchdog itself is late,
  probes are late and holds begin late; holds in progress still end.

### What could go wrong that the cap does not make harmless

- A reason to give way that `SkewOnlyProbed` does not know about would be
  delayed until its source raises the flag again or the slice ends (1 ms).
  The reasons known are tested: priority preemption and outranking wakes
  (`preempted`), terminate, shutdown, suspend, slice end.
- A title whose threads on one guest CPU rely on a lower-priority thread
  running while a higher one is held: it does not run until the hold ends.
  The console would not run it either while the higher one is runnable.

## Not covered

- A CPU that is late to dispatch a thread readied by a host thread (audio,
  GPU, a timer) or by its own timeout: there is no signaller to hold. The
  progress gate takes over once the thread has started.
- APCs (`WakeForApc`, `WakeAll`): not counted as readying signals.
- A waiter whose affinity was changed while it was parked is rereadied on
  its new CPU; the debt is on the old one and is answered by that CPU's
  idle pass, so the signaller is not held for the new CPU.
- Lapses longer than the cap.
- A stall shorter than the bound plus a tick: by design.

## Upstream files changed

All under `x360/emulator-core/src/main/cpp/xenia/src/xenia/kernel/`, every
change marked `hakuX-Boosted:`. Nothing in `gpu/` or `cpu/`.

- `guest_scheduler.h`: the three hooks (`SkewAtSafepoint`,
  `SkewOnlyProbed`, `SkewAtBackoff`); the per-CPU atomics; the private
  helpers; the counters.
- `guest_scheduler.cc`: the three settings; the hooks in
  `PreemptCurrentFiber` and `SpinBackoffYieldFiber`; the set-up in
  `EnsureStarted` (and the watchdog's priority, only with the bound on);
  debts in `WakeForSignal` and `EnqueueReady`; the serve count, the
  running flag and the probe's flag in `SwitchTo`; the probe's answer and
  the `NtYieldExecution` look in `YieldCurrentThread`; the verified idle
  pass and the CPU-time clock in `RunLoop`; the probe in `WatchdogLoop`;
  the block of new functions above `SpinYield` (its opening comment is the
  design in short); the overlap in `NoteWakeLatency`; the per-second line
  in `ReportStatsIfDue`; the totals and per-CPU state in
  `ReportNoProgress`.
- `xthread.h`: the debt fields and `skew_eval_owed` in `SchedulerLinks`.

`xobject.cc` is unchanged: signals reach the scheduler through
`WakeForSignal` as before and the latency is still reported from there.

## What the build could not verify

Everything about behaviour. In particular:

- That the gates fire when the phone squeezes the threads, that the holds
  end when the late thread turns up, and that Fable II's freeze stops.
- The cost in frame time, at either bound.
- That a hold shortens the stall by freeing the holder's core.
- That a probe is answered promptly in ordinary play: it depends on every
  running fiber passing a safepoint or a yield within the bound. Long
  kernel calls and translation will show as progress holds.
- That `SkewOnlyProbed` leaves no real preemption unserved (read from the
  code: every raiser of the flag was listed with grep and each leaves a
  mark that is tested).
- The watchdog's nice -12: it goes through the same `set_priority` path as
  the dispatch threads' -8, which was confirmed on device for them, not for
  this value.
- `pthread_getcpuclockid` and reading another thread's CPU-time clock on
  Android: standard, but if `on a core` always reads nought or the hold
  lines say `-1.0`, the clock is not available and only that figure is
  lost.
- With the bound at 0 the paths are today's. That is by reading: each
  addition is behind a test of `skew_bound_ticks_`, which stays 0. The
  only code that runs regardless is the readied thread's id being noted in
  `WakeForSignal`'s walk (one store per watcher).

## What is next

By what the device runs show:

- **If the freeze stops and the cost is small:** make a bound the default
  after long play on more titles, and put the settings in the settings
  screen.
- **If progress holds are mostly `on a core` (host code):** tell
  translation apart from descheduling at the gate itself, with the same
  CPU-time clock the counter uses: do not hold for a CPU whose host thread
  has been on a core for most of the lapse. About twenty lines in
  `SkewCpuStalled`'s callers. Not built, because whether to hold for
  translation is a question the device should answer first: a thread that
  translates for 5 ms is also 5 ms behind the others.
- **If holds end at the cap:** raise the cap; then the take-over below.
- **Take-over instead of waiting (the brief's idea, in the form that keeps
  one host thread per guest CPU at a time):** a holder's host thread is
  idle by definition. Give each guest CPU a run token; a holder whose
  target has no running fiber takes the token and runs that CPU's dispatch
  loop until its own host thread turns up and takes it back. Two threads of
  one guest CPU never run at once, and the work is done by a host thread
  known to be on a core. It needs `t_current_cpu` to change hands and an
  idle fiber per host thread and CPU. It does not help a fiber that is
  mid-run on a descheduled thread.
- **A finer tick:** probes on a tick of their own (250 us) would make the
  bound mean what it says below a millisecond. Only worth it if 500 proves
  better than 2000.
- **Timers and thread exits** do not wake cooperative waiters (see
  `guest-scheduler.md`), so they carry no debt either.

## Build

- `08a7171` (the code): Actions run 79, replaced before it finished by
  the push of this file (a newer push cancels a running build).
- `df2aa96` (the same code, plus this file): Actions run 80, green, both
  jobs (hakuX's core, and the APK with the 360 core).
- The commit that records this result changes this file only; its own run
  builds the same code again.

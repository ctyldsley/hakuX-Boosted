# Guest waits (branch `claude/guest-waits`)

**Nothing here has been run on a device.** The only check is the GitHub
Actions build of this branch (state at the end of this file) and a syntax
check of the changed files with the host's clang (not the a64 backend file,
which needs an arm64 sysroot). No speed-up and no fix is claimed. Every
figure below that is not from `OVERNIGHT-2.md` is what the code is meant to
do, to be checked against the log lines and profile symbols named beside it.

Base: `claude/integration-1113` at `8e03766`. Pushed only to
`claude/guest-waits`.

## Done / in progress / next

- Done, built by Actions (see "Build"): all five steps of the brief.
  1. The current-thread and current-CPU lookups off the TLS resolver.
  2. The empty yield's short way, and parks for yield spinners that work
     for a thread that spins in bursts and are ended by events.
  3. Pause-and-poll loops of several blocks recognised by the translator
     and parked by the scheduler.
  4. The skew bound no longer holds for a host thread that is busy in host
     code.
  5. `host perf` finds the GPU command thread.
- In progress: nothing.
- Next: the device runs under "Order on the phone". Then "What is next".

Commits: `1692228` (step 1), `06dfbd5` (step 5), `e64aaae` (steps 2, 3 and
4 together: they share `guest_scheduler.cc` and the parks of 2 and 3 are one
mechanism), then this file.

## Settings

Defaults are today's behaviour for a config that sets nothing. The one
exception to "off" is marked.

| Setting | Group | Default | What |
|---|---|---|---|
| `host_thread_state_slot` | CPU | false | Existing. Now also covers the scheduler's CPU index and the guest thread of a JIT hook or of `NtYieldExecution` (found from the PPC context) |
| `host_thread_state_verify` | CPU | false | Existing. Now also compares those two |
| `guest_scheduler_lean_yield` | Kernel | false | Existing. Its path is now `TryEmptyYield` (below) |
| `guest_scheduler_park_waits` | Kernel | false | New. Busy waits sleep: yield spinners, and with the next setting pause-and-poll loops. Implies lean yield; replaces `guest_scheduler_park_yield_spinners` and `guest_scheduler_park_poll_loops` (leave those off with it) |
| `park_pause_poll_loops` | CPU | false | New. The translator marks pause-and-poll loops and their site calls the scheduler on every pass. Read when a function is translated. Parks nothing without `guest_scheduler_park_waits` |
| `guest_scheduler_park_wait_after_us` | Kernel | 200 | New. How long one burst of busy waiting lasts before the first park |
| `guest_scheduler_park_wait_max_us` | Kernel | 1000 | New. The longest single park (clamped to 100..4000) |
| `guest_scheduler_skew_host_progress` | Kernel | **true** | New. Only read with `guest_scheduler_skew_bound_us` set, which is 0 by default. With the bound set (the phone's working config has 2000) it changes what that config does: that is the fix for the 27 s start. `false` is run 90's behaviour exactly |
| `log_memory_poll_park` | CPU | false | Existing. Now also prints why a loop was not taken for a pause-and-poll loop |

Not changed: the defaults of `guest_scheduler_skew_bound_us` (0) and
`guest_scheduler_skew_cap_multiple` (20). My row gives me no leave over
defaults; the recommendation and its reasons are under "Step 4".

## Order on the phone

Each run with `guest_scheduler_stats = true` and `host_perf_log = true`,
Fable II, same scene, and a simpleperf profile of the `Guest CPU` threads
where a symbol is named. One group per run:

1. **Nothing set** (the merge check). The log has no `busy waits are
   parked` line, `host perf` now names the GPU thread (`GPU Commands tid
   ...`, and `GPU Commands in` in the hint session's line) instead of `GPU
   missing`. Everything else as run 90.
2. **`host_thread_state_slot = true` with `host_thread_state_verify =
   true`**, once, to prove the lookups: no `HostThreadState mismatch` line
   at all (two new kinds can appear: `the scheduler's CPU index` and `the
   fiber thread found from its context`). Then verify off.
3. **`guest_scheduler_lean_yield = true`** (with 2 kept on).
4. **`guest_scheduler_park_waits = true`** (lean yield is implied).
5. **`park_pause_poll_loops = true`** added to 4.
6. **The skew bound as in run 90** (`guest_scheduler_skew_bound_us = 2000`,
   `guest_scheduler_skew_cap_multiple = 100`) on a cold start, for the time
   to the first frame.

If 4 or 5 makes anything worse, the first things to try are
`guest_scheduler_park_wait_max_us = 300` (shorter parks) and
`guest_scheduler_park_wait_after_us = 500` (later parks).

## What should change, run by run

### Run 2: the lookups

Profile of Guest CPU 0: `tlsdesc_resolver_dynamic` (8.2% of the thread in
run 90) and `CurrentCpu` (2.0%) should fall to near nothing on the yield
path. What stays through the resolver is listed under "What remains".

### Run 3: lean yield

- `GuestScheduler: left the CPU: ... NtYieldExecution N`: from about
  575,000 a second to about a thousand per spinning thread (one a slice).
- `switches` and `scheduler lock: taken` fall with it; `yields that found
  nothing to run and returned at once` rises by what they lost.
- Profile of Guest CPU 0, symbols that should leave the yield path:
  `__aarch64_cas2_acq` (5.0%, the mutex), `pthread_mutex_lock`/`unlock`
  (3.5%), `clock_gettime` (3.1%), `__aarch64_ldadd8_relax` (3.0%, the
  counters), the dispatch loop (`EnsureStarted` lambda, 3.7%),
  `EnqueueReady` (1.1%), `jump_fcontext` and `SwitchTo` (1.6%),
  `set_current_thread_id` (1.7%). `YieldCurrentThread` should be replaced
  by `TryEmptyYield` at a fraction of its 2.3%.
- The thread still fills its core: it now spins cheaply. Run 4 is what
  takes the core back.

### Run 4: busy waits parked

- One line at start: `GuestScheduler: busy waits are parked after 200 us,
  for 100 us at first and 1000 us at most, and woken by events`.
- A sixth per-second line: `GuestScheduler: busy waits: parked yield
  spinners N, poll loops N, N us parked in all | parks ended by the
  deadline N, by an event N | looks that did not park: an event since the
  last look N, not allowed here N | events counted N | poll site looks N,
  registers changed N`.
  - `parked yield spinners`: expect hundreds to thousands a second for
    Fable II's main thread (run 78 had 9 with the old park).
  - `N us parked in all`: the core time given back, per second. Near
    1,000,000 for a thread that only waits.
  - `ended by an event` against `by the deadline`: how often the wait's
    cause was seen. Mostly deadline means the thread waits for a plain
    memory write (see "What is not seen").
- `host perf`: G0's `run` should fall from 930 to 960 ms a second by about
  what `parked in all` says; `MHz limit` should fall more slowly over five
  minutes. Frame rate and `late wake` lines must not get worse.

### Run 5: pause-and-poll loops

- At translation: `MemoryPollPark: pause-and-poll loop at guest XXXXXXXX: N
  block(s), N load(s), N pause(s)` for each loop taken. For the 3D Engine
  thread's wait look for an address inside `82B9BF90`'s function. If there
  is none, set `log_memory_poll_park = true`: `MemoryPollPark: loop at
  guest XXXXXXXX (N block(s)) is not a pause-and-poll loop: <reason>` says
  which rule refused it.
- Per second: `parked ... poll loops N`, `poll site looks N` (one in eight
  passes of such loops), `registers changed N` (a look at which the thread
  had moved on: high beside `looks` means the loop is not a pure wait and
  is rightly left alone).
- `host perf`: G3 (98% of a core in run 90) should fall. Profile of Guest
  CPU 3: `sub_82B9BF90`'s share (61.6%) should fall and
  `GuestScheduler::PollSiteVisit` appear, small.

### Run 6: the skew bound at start-up

- Start line: `GuestScheduler: skew bound 2000 us, one hold at most 200000
  us, progress gate on, judged every 1000 us (the watchdog tick), a host
  thread busy in host code counts as progress`.
- The skew line gains `| overdue probes of a host thread busy in host
  code, not held for N`: large in loading, small in play.
- Time to the first frame: 27 s in run 90, 8 s without the bound. `holds`
  and `held N us in all` before frame 1,000: 1,221 holds and 35.3 s in run
  90. `awaited host threads on a core for N us of the holds` should now be
  small beside `held`: what is still held is for threads that were off
  their core.
- In play the figures should be about run 90's (2,694 holds, 6.7 s in
  792 s); the freeze condition (the worker's late wake of 64 ms or more)
  must still show a `skew hold` line beside it.

## Step 1: the lookups

`libe.so` is loaded with `dlopen`, so each `thread_local` read is a call to
`tlsdesc_resolver_dynamic`. An empty `NtYieldExecution` made six or seven:
the bound context (the APC test), the current thread (three times), the CPU
index (three or four times). Now:

- `CurrentCpu()` reads `HostThreadState::scheduler_cpu` when the slot is in
  use. The `thread_local` is still written (once per dispatch thread) and is
  what the verify mode compares with.
- `cpu::ThreadState` has an `owner` (the `XThread` that made it), so
  `XThread::FromContext(context)` is two loads. The safepoint handler, the
  backoff site and `NtYieldExecution` use it through
  `GuestScheduler::FiberThreadOfContext` when the slot is in use, and
  `XThread::GetCurrentFiberThread()` otherwise. The backoff site passed a
  null context; it now passes its own.
- `NtYieldExecution` hands its context to `xeProcessKernelApcs` (always;
  it is the same context the lookup returned).
- With lean yield on, the empty yield makes no lookup at all inside the
  scheduler: the thread comes from the caller and the CPU from the running
  fiber's own link (`links.cpu`), slot or no slot.

### What remains through the resolver

- `base/mutex.cc`, `gettid()`: a function-static `thread_local` read by
  every lock of the global critical region and by
  `is_held_by_current_thread()`, which the scheduler asks at each full
  fiber switch (`YieldToScheduler`), each slice-end safepoint and each park
  decision. Not on the empty yield's path any more. Not my file. The
  change: bionic's `gettid()` reads the id from the thread's own control
  block without the resolver, so on Android the cache can go:
  `XE_NOINLINE pid_t gettid() { return ::gettid(); }`.
- `XThread::SetCurrentThread` at each real fiber switch: with the slot on
  its four writes are slot writes; with it off, four resolver calls per
  switch (lean yield removes nearly all the switches).
- `base/threading_posix.cc`: `alertable_state_` and the `t_wait_any_*`
  counters (host waits, not the yield path), `current_thread_`
  (`PosixThread`, four callers). Left as they are.
- `kernel/util/shim_utils.cc` `thread_local_string_buffer` and
  `base/logging.cc` `thread_log_buffer_`: only when a line is logged.
- `ThreadState::Get()` for exports that call `xeProcessKernelApcs(nullptr)`
  (five other waits in `xboxkrnl_threading.cc`): slot on, it is a slot read.

## Step 2: the yield path

### `TryEmptyYield` (lean yield and the park settings)

Entered first in `YieldCurrentThread` for `NtYieldExecution` and zero
delays. It returns "nothing ran" after: the fiber's `running` flag and CPU
link, `preempted`, `terminate_pending`, the preempt flag, the suspend
count, the CPU's ready summary and poke flag, (with the skew bound) the
probe and the owed look, the priority decay of a quantum end, and one plain
store to the CPU's empty-yield count. No mutex, no compare-and-swap, no
atomic add, no clock, no fiber switch, no lookup. With parking on it reads
the clock on every 32nd call. Anything else (something ready, a poke, the
flag up at a slice's end or for a probe, a terminate, a suspend) falls
through to the unchanged long way, so the dispatch loop's timed passes
still happen once a slice. Lean yield off: this function is not entered and
the path is today's.

### `guest_scheduler_park_waits`, yield spinners

Why the old park failed on the device: it needed 2 ms of unbroken spinning
and the thread's own short waits restarted the count every 1.5 to 3 ms; and
once parked it slept a whole millisecond and only the clock ended it.

- **Per burst.** The record of a burst is the thread's own
  (`SchedulerLinks::spin_*`). A wait of the thread's own ends the burst and
  nothing else; the next burst is judged afresh and parks after
  `guest_scheduler_park_wait_after_us` (200) of yields that come 32 within
  640 us. A thread that works between its yields is never parked.
- **How long.** 100 us for the first park of a burst, twice the last each
  time, at most `guest_scheduler_park_wait_max_us` (1000). Between parks
  the thread makes 64 empty yields, so it still runs its own test.
- **Woken by what it waits for, where that can be known**
  (`NoteWaitEvent`): any kernel object signalled (`WakeForSignal`: events,
  semaphores, mutants, a critical section handed over), a critical section
  released with nobody queued (`RtlLeaveCriticalSection`), another guest
  thread parking in a wait or exiting (a worker that has finished goes back
  to sleep), an APC queued, an I/O completion (`WakeAll`), the title's GPU
  interrupt routine having run (`EmulateCPInterruptDPC`). An event ends a
  park no sooner than 50 us after it began, which bounds how often a busy
  neighbour can wake a spinner.
- **No event slept through.** Parking takes two looks 32 yields apart. The
  first arms: from then events move a sequence number. The second parks only
  if it has not moved, and `BlockCurrentThread` compares once more under
  the lock after counting the thread as parked; the event's source moves
  the sequence and then reads the parked count. An event in that last
  window shortens the park to 50 us.
- **Cannot add a frame of latency.** A park ends at its deadline whatever
  happens, the thread then runs its own test, and nothing decides for it
  that the wait is over. The most one pass of a wait can be late is one
  park: 1 ms by default, 4 ms at the clamp, against 16.7 or 33.3 ms of
  frame. Behind the fine deadline there is a millisecond deadline and the
  64 ms backstop.
- **On the dispatch thread.** The ready event's wait only takes whole
  milliseconds. With a parked spinner the dispatch thread sleeps on a
  condition variable until the spinner's deadline (`FineSleep`), and every
  waker goes through `WakeDispatchThread`, which signals whichever of the
  two the thread sleeps on. Anything made ready on that CPU still runs at
  once. With the setting off `fine_parked` is never true and the wakers do
  what they did, after one extra load.
- **Cost of the hooks with the setting off:** one relaxed load each
  (`wait_armed_until_` is 0 until a thread first arms).

### What is not seen

A plain store to memory by another guest thread that keeps running, or by a
host thread (the GPU command thread's read pointer write-back, a scratch
register write-back). For those the deadline is the answer. For the GPU
there is a one-line change in a file I do not own, under "For the merge".

## Step 3: busy waits in translated code

### Why the passes missed the loop

`MemoryPollParkPass` only looked at one block that branches to itself, and
refused a block holding a collapsed spin sled (`SPIN_BACKOFF` is not on its
list). The profile's loop is a counted loop of no-op register moves, then
loads and a compare, and round again: either several blocks, or one block
with a collapsed sled in it. Both were out. Also, `or rX,rX,rX` is only
turned into a pause hint for `r31` (`ppc_emit_alu.cc`); the other register
numbers are plain no-ops, so the old "four hints in a block" rule did not
see such a sled at all. And the old backoff site only calls the scheduler
with the preempt flag up, which the loop's own safepoint usually takes
first.

### Which loops are recognised (`park_pause_poll_loops`)

A loop, taken as the run of blocks in layout order from the target of a
backward (or self) branch to the block with that branch, for which all of
this holds:

1. At most 8 blocks and 256 instructions.
2. Every instruction in it (in the last block, up to the backward branch)
   is one of: a no-op, a pause hint, a collapsed spin sled, a safepoint, a
   memory barrier, a read or write of the thread's own registers, a load of
   guest memory, integer arithmetic or a compare from the pass's existing
   list, a branch to a label. Anything else refuses the loop: a store to
   guest memory, any call, a return, an atomic or reserved access, a clock
   read, a trap, a local, floating-point or vector work.
3. It pauses: a pause hint, a collapsed sled, or an inner block that loops
   on itself without loading memory (the counted loop of no-ops).
4. It loads guest memory at least once.
5. No block of it but the first is entered from outside.

The pause is the marker that the code means to wait: a string scan or a
search is load, compare, branch as well, and does not pause.

What is done to it: one `SPIN_BACKOFF`, flagged, in front of the backward
branch. Nothing is removed, reordered or made conditional. The a64 backend
calls the scheduler there on every pass (`SpinPollSiteThunk`, with the
loop's guest address), not only with the preempt flag up.

### When the scheduler parks (`PollSiteVisit`)

On every eighth visit it hashes the thread's whole architectural state
(everything in the PPC context before `thread_id`: condition and status
registers, r0 to r31, CTR, LR, the floating-point and vector registers).
The thread is parked only when the hash has been the same at every look for
`guest_scheduler_park_wait_after_us`, with looks no more than 400 us apart.
The park is the same as a yield spinner's: armed first, 100 us doubling to
the maximum, ended by an event or its deadline.

### Why none can be mis-recognised into a hang

- A park is a timed sleep. When it ends the loop runs its own load and
  compare. The scheduler never takes the thread out of the loop, never
  keeps it in, never skips a pass and never answers the test for it. The
  worst a wrong park can do is make one pass up to
  `guest_scheduler_park_wait_max_us` late.
- A loop that makes progress of its own changes a register (a scan's
  pointer, a counter, a retry count): its hash differs at every look and it
  is never parked. `registers changed` counts those looks.
- A loop with the same registers pass after pass, in code that stores
  nothing and calls nothing, can only be waiting for memory to change under
  it. That is the case a park is right for.
- With pinned guest registers the context is made current before the call
  (`CallNativeSafe` flushes the pins), so the hash sees the real registers.

The residual risk, stated plainly: the visits are not proven to be
consecutive passes of one stay in the loop. A thread that goes through such
a loop once per turn of a larger working loop, with every register the same
each time, at least every 50 us, for 200 us on end, would be parked and its
larger loop slowed by up to a park per turn. I know of no such code; the
counters would show it as `poll loops` parks on a thread that is not idle,
and `guest_scheduler_park_wait_max_us` bounds it. Closing it needs a mark
on the loop's exits, which needs a byte in the backend's per-thread data
(`a64_backend.h`, not my file); see "What is next".

## Step 4: the skew bound at start-up

### What was wrong

A fiber in the translator or a long kernel call does not reach a safepoint,
so its probe stays unanswered and the progress gate held every other CPU up
to the cap, again and again: 35.3 s held in loading in run 90.

### The fix (`guest_scheduler_skew_host_progress`)

A host thread that is descheduled gains no CPU time; one that is in host
code gains it at the rate of the clock. Each dispatch thread's CPU-time
clock was already kept for the hold's log line. Now, at the top of each
tick and before the new probes, the watchdog reads it for every CPU whose
probe has been outstanding a whole tick (`SkewSampleHostThreads`): the
first reading of a probe is the base, each later one gives the share of the
tick the thread spent on a core. `SkewCpuStalled` then does not take a CPU
for stalled when the last reading belongs to this probe, is no more than
three ticks old, and says half the tick or more on a core.

Why this does not weaken the bound's own case:

- A descheduled dispatch thread has a share near nought: the verdict is as
  it was. Four dispatch threads packed on one core (run 73) get a quarter
  each: below the half, still stalled, still held for.
- No reading, or a stale one (the watchdog itself not being run), falls
  back to the safepoint alone, as before.
- The start gate's first case (no fiber running there, the dispatch thread
  woken and not turned up: the measured 64 to 179 ms) does not go through
  this test at all.
- A hold in progress ends within a tick of the awaited thread being found
  on a core.

What it does change: a thread that translates for 20 ms no longer holds the
others, so they run ahead of it by that much. The console has no
translator, so this is a skew the bound used to remove. The brief asks for
it to count as progress; if a freeze returns in a scene with new code, set
it `false` first and compare.

Cost: one `clock_gettime` on another thread's clock per tick per CPU with
an overdue probe, on the watchdog thread, outside the scheduler lock.

With the bound under two ticks (500) the first hold for a lapse can begin
before the second reading exists; it then ends within a tick.

### Defaults: recommended, not applied

- **`guest_scheduler_skew_bound_us = 2000`.** It is the only value run on
  the device; it rode out the one freeze condition seen (a worker 116.6 ms
  late where 64 ms used to deadlock) and cost 0.8% of play. 500 has never
  run. Judged once a tick, 2000 means 2 to 3 ms behind, far under the tens
  of milliseconds the freeze needs and above the sub-millisecond lapses
  that are normal on a phone.
- **`guest_scheduler_skew_cap_multiple = 100`** (200 ms at 2000). The cap
  must outlast the lapse or the reorder happens anyway: the measured late
  wakes were 64, 116.6, 137 and 179 ms, all over the present default's
  40 ms. 100 is what run 90 used. A host thread that is truly stuck costs
  each other thread 200 ms once, not a hang.
- **Not applied because** the literals are the defaults session's by the
  ownership table, the start-up fix above has not run, and one hot run
  without a freeze is evidence, not proof. When run 6 shows the first frame
  back near 8 s with the play figures unchanged, make both the defaults.

## Step 5: `host perf`

The GPU command thread is a guest-visible host thread; `XThread::set_name`
appends its handle and the kernel keeps 15 characters, so its `comm` is
`GPU Commands (0` and the exact compare never matched. It is now matched by
`GPU Commands` followed by the end or a space. The threads it starts carry
the same `comm` until renamed; the earliest started is taken, as before.
The hint session and the core policy already included the thread when it
was found, so they now do.

## Risks

- **Lean yield:** as in `guest-scheduler.md`. New here: the CPU is taken
  from the fiber's own link instead of the host thread. It rests on "a
  running fiber is linked to the CPU that runs it", which every queueing
  path keeps under the lock.
- **The thread from the context** (slot on): rests on guest code and kernel
  exports running on their own thread's context. The verify mode checks it
  on every call.
- **Parks:** up to one park of latency per pass of a wait whose cause is
  not one of the events. Sub-millisecond sleeps depend on the phone's timer
  slack; if 100 us sleeps come back after 1 ms, `parked in all` will be
  large and frame time may suffer: lower the maximum. A spinner woken by a
  busy neighbour's events can be woken up to 20,000 times a second (the
  50 us floor); it would show as `ended by an event` in the tens of
  thousands and little time parked.
- **Events as wakes:** each hook, with a thread armed or parked, is an
  atomic add and at most one take of the scheduler lock per park.
- **Poll sites:** a thunk call on every pass of a marked loop (the cost of
  seeing the passes), and the residual risk under step 3.
- **The skew change:** under step 4.
- **The scheduler lock** is taken by `WakeParkedWaits` from any thread that
  signals; it is never taken under another scheduler lock, and
  `EmulateCPInterruptDPC` calls it while holding only its own mutex.

## Upstream files changed

Under `x360/emulator-core/src/main/cpp/xenia/src/xenia/`, every change
marked `hakuX-Boosted:`. All in my row.

| File | Change |
|---|---|
| `base/host_thread_state.h` | `scheduler_cpu` in use; `HostThreadStateInUse()` |
| `base/host_perf.cc` | the GPU command thread's name match |
| `cpu/thread_state.h` | `owner` |
| `kernel/xthread.h`, `.cc` | `XThread::FromContext`; the owner set where a `ThreadState` is made; the new `SchedulerLinks` fields |
| `kernel/guest_scheduler.h`, `.cc` | everything else of steps 1 to 4 |
| `kernel/xboxkrnl/xboxkrnl_threading.cc` | `NtYieldExecution` takes its context |
| `kernel/xboxkrnl/xboxkrnl_rtl.cc` | `RtlLeaveCriticalSection` reports a release with nobody queued |
| `kernel/kernel_state.cc` | `EmulateCPInterruptDPC` reports the interrupt routine having run |
| `cpu/compiler/passes/memory_poll_park_pass.h`, `.cc` | `park_pause_poll_loops`, `MarkPausePollLoops`, the site's flag |
| `cpu/backend/a64/a64_seq_memory.cc` | the `SPIN_BACKOFF` sequence: the poll site's call and thunk, the handler pointer; the backoff thunk passes its context |

`SPIN_BACKOFF`'s `src1.offset` now carries the loop's guest address in its
upper half for flagged sites; the a64 sequence reads the lower half as the
count, as before. The x64 backend (not built for the phone) reads the whole
value: do not switch `park_pause_poll_loops` on there without the same
mask.

## For the merge: changes in files I do not own

1. `base/mutex.cc`: `gettid()` as under "What remains".
2. `gpu/command_processor.cc`, after the read pointer write-back in the
   worker loop (`xe::store_and_swap<uint32_t>(memory_->TranslatePhysical(
   read_ptr_writeback_ptr_), read_ptr_index_);`), and wherever a scratch
   register is written back to guest memory:
   `kernel_state_->guest_scheduler()->NoteWaitEvent();` (include
   `xenia/kernel/guest_scheduler.h`). One load when nobody busy-waits. That
   makes a wait on the read pointer end with the GPU's progress instead of
   at its deadline.
3. `cpu/ppc/ppc_emit_alu.cc`, `InstrEmit_orx`: the other pause idioms
   (`or r1,r1,r1`, `or r2,r2,r2`, `or r6,r6,r6`, `or r26` to `r30`) are
   emitted as plain no-ops. Emitting `DelayExecution()` for them as for
   `r31` would let rule 3 see a sled that is not in a counted loop. Not
   needed for the counted-loop shape.

## What the build could not verify

Everything about behaviour: that the slot and the context agree, that the
symbols leave the profile, that sub-millisecond parks sleep that long on
this phone, that events end parks, that the 3D Engine thread's loop meets
the five rules (it may not: the log says why), that its registers are the
same pass after pass, that the first frame comes back to 8 s, that the
freeze stays away. With every setting at its default the paths are today's
by reading: each addition is behind `lean_yield_`, `park_waits_`, a count
that stays 0, or the skew bound; the exceptions are the context handed to
the APC test in `NtYieldExecution`, the dispatch-thread wake going through
`WakeDispatchThread` (one extra load), and the hooks' one load each.

## What is next

- The GPU hook of "For the merge", then look at `ended by an event` for the
  3D Engine thread.
- Exit marks for poll sites (a byte in the backend's per-thread data,
  cleared inline where a marked loop is left), which would turn "same
  registers at every look" into "did not leave the loop".
- If `registers changed` is high for the profile's loop, it keeps a retry
  count: recognise a counter that is only incremented and compared, and
  leave it out of the hash.
- Timers and thread exits waking their waiters (`guest-scheduler.md`).
- The scheduler lock per CPU, if `scheduler lock:` still shows waiting
  once the yield storm is gone.

## Build

- `1692228` (step 1): Actions run 98.
- `e64aaae` (steps 2 to 5): Actions run 100.
- Results are recorded here by the commit that follows them.

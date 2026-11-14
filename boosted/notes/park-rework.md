# Park rework (branch `claude/park-rework`)

**Nothing here has been run on a device.** The checks are the GitHub Actions
app build, the bench rig on GitHub's ARM64 runner (runner numbers, not phone
numbers), and the same rig cross-built and run under `qemu-aarch64` in the
cloud session while the work was done (timings there mean nothing; whether a
thread is parked does). No fix and no speed-up is claimed for any title.

Base: `claude/integration-1115` at `5aea2e2`. Pushed only to
`claude/park-rework`.

## Done / in progress / next

- Done:
  1. The phone's result reproduced on the rig: with today's code a busy wait
     beside unrelated events is never parked, and none of the poll-loop
     shapes is parked even alone ("The bench scenarios", "Before").
  2. A new gate for the parks, `guest_scheduler_park_wait_adaptive` (default
     true, only read with `guest_scheduler_park_waits`): unrelated events no
     longer forbid a park ("The gate").
  3. Why `poll site looks` was 0, as far as the rig can say, and the
     recognition made general: a defect in how loops were picked, a second
     way to know a loop is a wait, two more shapes ("Poll loops").
  4. A loop of zero-timeout waits counts as a busy wait, and is woken at
     once by a signal of the object it polls.
  5. The main thread's empty yields: the arithmetic says there is little
     there to win ("The main thread").
  6. Bench rows, and the tables of bench runs 15, 17 and 18 ("Bench
     results"). Run 18 is the final code: the phone's case is a whole core
     with the first gate and a tenth of one with the adaptive gate.
- In progress: nothing.
- Next: the device runs under "What a device run must show". Then "What is
  next".

## Settings

| Setting | Group | Default | What |
|---|---|---|---|
| `guest_scheduler_park_waits` | Kernel | false | Existing. Busy waits sleep. Still the master switch; nothing below is read without it |
| `guest_scheduler_park_wait_adaptive` | Kernel | **true** | New. The new gate. `false` is the gate the phone ran on 2026-11-14 (no park if any event happened since the last look), kept to compare |
| `park_pause_poll_loops` | CPU | false | Existing. The translator marks wait loops and their site calls the scheduler on every pass. Its recognition is what changed here |
| `guest_scheduler_park_wait_after_us` | Kernel | 200 | Existing, unchanged |
| `guest_scheduler_park_wait_max_us` | Kernel | 1000 | Existing, unchanged: the longest park, and so the most a waiter is ever answered late |
| `log_memory_poll_park` | CPU | false | Existing. Says why a loop was not taken. **Switch it on for the next device run** |

With every setting at its default the paths are today's by reading: the new
code is behind `park_waits_` (the scheduler), behind `park_pause_poll_loops`
(the pass), or is one load (`WakeForSignal` reads a count that stays 0; the
two single-object waits compare their timeout with nought and test
`park_waits_`).

## The device result, and what it was

Fable II, 2026-11-14, `guest_scheduler_park_waits` and
`park_pause_poll_loops` on: `parked yield spinners 0, poll loops 0`, `looks
that did not park: an event since the last look 131782`, `events counted
4223165`, `poll site looks 0`; Guest CPU 3 at 99% of a core throughout.

Two separate failures:

1. **The gate.** A thread was parked only if no event had happened since its
   last look. Looks come every 32 yields, which for a thread that does a
   little work per pass is hundreds of microseconds apart; the title's
   events (every signal, every critical section left with nobody queued,
   every GPU read pointer write-back, every thread going to sleep) came at
   12,400 a second, one every 80 us. So there was always one. The waiting
   thread's own loop also makes such events (it leaves critical sections,
   it sets events), and they counted against it too.
2. **The recognition.** No loop was marked. See "Poll loops".

## The bench scenarios (`boosted/bench/core/bench_sched.cc`)

A waiter on guest CPU 0 spins until a word of shared memory is nonzero,
tells the harness, and spins again. Beside it, as the run asks:

- **noise**: a thread on CPU 1 that sets an event nobody waits on, then
  sleeps its host thread 100 us in the harness: several thousand unrelated
  events a second (the row reports how many), at next to no CPU of its own.
- **producer**: a thread on CPU 2 that, 3 ms after the waiter took the last
  one, makes the word true with a plain store and notes the time. The
  waiter's call to the harness gives the latency: flag made true to flag
  seen. Variants: the store followed by an unrelated event; the store
  followed by a signal of the event the waiter polls.

The waiter comes in eight shapes, the ones the brief lists:

| Name | The wait |
|---|---|
| `yield` | `NtYieldExecution` on every pass |
| `zero_delay` | `KeDelayExecutionThread(0)` on every pass |
| `zero_timeout_wait` | `NtWaitForSingleObjectEx` with a zero timeout on every pass |
| `poll_sled` | a counted loop of no-op register moves (`or r1,r1,r1`, `bdnz`), then load, compare, branch: the shape of the phone's profile |
| `poll_hint` | `or r31,r31,r31` (db16cyc) inline, then load, compare, branch |
| `poll_plain` | load, compare, branch and nothing else |
| `poll_clock` | the same with a time-base read (`mftb`) each pass: a deadline loop |
| `spin_lock` | a pause-and-load wait loop, then `lwarx`/`stwcx.` |

Runs, per shape: `_quiet` (never answered, nothing else happens: is the
shape parked at all), `_noisy` (never answered, with the noise: the phone's
case), `_answered_noisy` (with noise and the producer). For `yield` and
`poll_sled` also `_answered_quiet` and `_answered_with_event_noisy`; for
`zero_timeout_wait` also `_answered_with_its_event_noisy`.

Metrics: `cores_busy` (the process's CPU time over wall time: about the
waiter alone, since the other two sleep in the harness), the latency's
median, 95th percentile and worst in microseconds, the number of waits
answered, and the noise's events a second. The poll loops keep no counter of
their passes on purpose: a wait that counts its passes changes a register
every pass, which is exactly the loop that must not be parked.

They run for the rows of `run-core.sh` marked `BENCH_BUSY_WAITS=1`:
`defaults` (a busy wait left alone), `park_waits`, `park_waits, first gate +
poll sites` (what the phone ran), `park_waits + poll sites`, the same with
`max 500 us`, and the same with the skew bound. Two samples of 100 ms each a
round, because there are many of them and the job has twenty minutes.

To run one alone and read the core's own log:

```
BENCH_SCHED_ONLY=busy_wait_poll_sled_noisy BENCH_CORE_LOG=1 ./bench_core \
  --suite=sched --set guest_scheduler_park_waits=true \
  --set park_pause_poll_loops=true --set log_memory_poll_park=true \
  --set guest_scheduler_stats=true --set log_file=/tmp/core.log
```

## The gate (`guest_scheduler_park_wait_adaptive`)

### Options weighed

| Option | Verdict |
|---|---|
| Only events that could matter to the thread | Taken where the kernel can know what the thread waits for: a zero-timeout wait names its object, and a signal of that object ends the park at once. Not possible in general: a wait on a memory word names nothing, and the hooks in files I do not own (`RtlLeaveCriticalSection`, the GPU) call `NoteWaitEvent()` with no argument |
| A per-thread or per-site generation instead of the global one | Not taken as such. The global sequence is kept only to catch an event between the thread's last look and its park; what is per thread is the *learning* below, which does the job a per-thread generation would (a thread stops being woken by events that are not its own) without the sources having to know the thread |
| Parking briefly anyway, with an event-ended wake | Taken, and it is the core of the change: an event never forbids a park |
| Adaptive park lengths that grow while the wait keeps failing | Taken: a park lasts a quarter of the wait so far |
| The waiting thread's own events not counting | Falls out: a thread's own events come while it runs, never while it is parked. They can only shorten its next park to the hold-off, and the hold-off learns |

### What it does

1. **An event does not forbid a park.** Once a burst of busy waiting is
   `guest_scheduler_park_wait_after_us` old, the thread is parked at its
   next look whatever has happened.
2. **How long.** A quarter of the time the burst has lasted so far, no less
   than 100 us and no more than `guest_scheduler_park_wait_max_us`. Between
   two parks the thread makes four passes of its own loop (32 before the
   first park of a burst).
3. **What an event may do: end a park early, after the thread's hold-off.**
   The hold-off starts at 50 us. It is learnt per thread from what follows
   an early wake:
   - the thread comes back to park in the same burst (it was woken and is
     still waiting): the hold-off doubles, up to longer than any park, at
     which point events no longer wake the thread at all;
   - the burst ends (the thread goes on to work or to a wait of its own):
     the wake was worth it and the hold-off goes back to 50 us.
   Every 32nd park the hold-off is halved once, so a change is found out.
4. **An event between the thread's last look and its park** makes that park
   end at the hold-off (the sequence is sampled at each look and compared
   under the lock as the thread parks). The first gate refused the park
   here; that was the bug.
5. **What an unrelated event costs.** Inside a hold-off that covers the
   whole park: two loads and a store, no lock, no wake. Inside a shorter
   hold-off: the first event wakes the parked thread's dispatch thread
   once. The first gate's wake path took the scheduler lock and woke the
   dispatch thread for every event, which at 12,400 events a second would
   have kept a parked thread's host thread awake.
6. **A zero-timeout wait** that finds its object not signalled counts as a
   pass of a busy wait, like an empty yield. While such a thread is parked,
   a signal of that object ends the park at once and is kept out of the
   hold-off's learning.

### The latency bound, stated

For a waiter whose condition has become true, its own test runs again:

| When the condition becomes true by | The waiter tests again within |
|---|---|
| a plain store nothing reports (another thread's memory write, the GPU) | the rest of its park: at most a quarter of the time it has already waited, no less than 100 us, never more than `guest_scheduler_park_wait_max_us` (1 ms) |
| something followed by an event, while events have proved worth waking for | the hold-off, 50 us, plus the wake |
| something followed by an event, after the thread has learnt to ignore events | as the first row |
| a signal of the object it polls with a zero timeout | the wake alone |
| a thread that outranks it becoming ready on its CPU | at once, as for any blocked thread |

Nothing ever answers the test for the thread, takes it out of its loop or
keeps it in: a wrong park costs time within the first row's bound and
cannot change what the guest computes. Behind the fine deadline there are
still the millisecond deadline and the 64 ms backstop.

Against a frame: a wait of 3 ms is answered at most 0.75 ms late and on
average about half that; any wait at most 1 ms late. At 30 frames a second
that is 3% of a frame per wait in the worst case. A title that makes
several long busy waits a frame one after the other could lose a
millisecond to each; `guest_scheduler_park_wait_max_us = 500` halves that
for twice the wake-ups, and is a bench row.

### The Fable II freeze

The freeze is a worker running tens of milliseconds after its start signal
(64, 116, 137, 179 ms measured). A park delays only the parked thread, by
at most 1 ms a pass, and the parked thread is by definition one that is
waiting. Where the main thread busy-waits for the worker, the park makes
the main thread later, which is the safe direction. A worker that waits
for its start in a kernel wait is not touched at all. A worker that
busy-waits for its start would start up to 1 ms late, against the tens of
milliseconds the freeze needs. The skew bound is untouched and there is a
bench row with both on.

## Poll loops (`park_pause_poll_loops`, `MemoryPollParkPass`)

### Why nothing was marked

The rig cannot translate Fable II, so what follows is what the rig showed
with loops written for it, and one reading of the title's dumps.

- **The defect found.** Candidate loops were merged per first block: of
  several backward branches to one block, only the last in layout order was
  kept, and only that one loop was judged. A wait loop whose first block is
  also the first block of a larger loop ("wait; then do something; and round
  again") was therefore only ever judged as the larger loop, which calls or
  stores, and was refused. Every bench waiter is laid out like that and
  none was marked; the log said `an instruction that is neither pause nor
  poll: set_return_address` for each.
- **`lr=82B9BF98` is not a call in the loop.** Earlier notes took that
  return address, inside `sub_82B9BF90`, for a call the loop makes at
  `82B9BF94`. It is the function's own prologue: `mflr r12` at `82B9BF90`
  and `bl __savegprlr_NN` at `82B9BF94` leave exactly that in LR for as long
  as the function then calls nothing. So the loop most likely makes no
  call, which fits the profile ("93% translated code, almost no calls into
  the emulator"). This is a reading, not a disassembly.
- **Whether the merge is what refused the title's loop is not known.** The
  device run did not have `log_memory_poll_park` on. With it on, the log
  says for each loop either `MemoryPollPark: pause-and-poll loop at guest
  82B9BFxx: ...` or `MemoryPollPark: loop at guest 82B9BFxx (N block(s)) is
  not a pause-and-poll loop: <reason>`, once for each backward branch.
  That line is the first thing to read after the next run.

### What changed

1. **Every backward branch is a candidate of its own**, judged largest
   first, so a refused outer loop leaves the inner one to be taken.
2. **Every branch back to an accepted loop's first block gets the site.** A
   pass takes exactly one of them, so it still visits one site. With the
   site at the last such branch only, a loop whose waiting path goes back
   from an earlier block never reached it (the bench's spin lock).
3. **A loop needs no pause if it is stateless.** A loop is stateless when no
   register it writes is written with a value that depends on what the loop
   left in a register on an earlier pass: every value it stores to the
   context comes from constants, loads of guest memory, registers the loop
   never writes, or registers it has already written in the same pass with
   such a value. Each pass of such a loop is the same as the last unless
   the memory it loads has changed: it is waiting for memory, or it never
   ends. A scan, a search or a retry counter advances something from its
   own last value and fails. This takes `while (!flag) {}` with no hint in
   it. The pause is kept as the other way in, as before.
4. **A reserved load (`lwarx`) reads as a load.** The conditional store is
   still refused, so the loop that takes the lock is not marked, only the
   wait in front of it.

The rest of the rules stand: at most 8 blocks and 256 instructions; only
no-ops, pauses, safepoints, barriers, the thread's own registers, loads,
integer arithmetic and compares, and branches to labels; at least one load;
entered from outside only at the top. The scheduler's side stands too: the
thread is parked only when its whole register state is the same at every
eighth visit for 200 us.

### Why none of this can turn a finite loop into a hang

- The site does nothing to the loop. It is one call in front of a backward
  branch. No instruction is removed, moved or made conditional, and the
  call returns to the same place whatever the scheduler does.
- A park is a timed sleep that ends by itself, after which the loop runs
  its own load and compare. The scheduler never answers the test.
- A loop that makes progress of its own changes a register every pass, so
  its register hash differs at every look and it is never parked. That is
  judged at run time and does not rest on the pass being right: the
  stateless rule only decides which loops get a site at all. A loop wrongly
  given a site pays a call a pass and is otherwise as it was.
- A loop that counts its passes down to a fall-back (a spin count before a
  kernel wait) changes a register every pass and is never parked, so it
  reaches its fall-back after exactly as many passes as before.
- A deadline loop reads the time base, which the pass refuses
  (`load_clock`), and would have changing registers if it did not.
- More sites than before can only mean more loops that pay a call a pass.
  Stateless loops and loops that pause are waits by construction, so the
  call lands on threads that are wasting their time anyway.

The residual risk is the one `guest-waits.md` states and is unchanged: the
visits are not proven to be consecutive passes of one stay in the loop.

### Shapes, and what happens to each now

| Shape | Marked | Parked |
|---|---|---|
| counted no-op sled, then load and compare (`poll_sled`) | yes | yes |
| db16cyc inline, load and compare (`poll_hint`) | yes | yes |
| load, compare, branch alone (`poll_plain`) | yes, as stateless | yes |
| pause-and-load wait in front of `lwarx`/`stwcx.` (`spin_lock`) | the wait, yes | yes |
| `mftb` deadline loop (`poll_clock`) | no (`load_clock`) | no, on purpose: it ends by its own deadline |
| `NtYieldExecution` loop | not a translator matter | yes, by the scheduler |
| `KeDelayExecutionThread(0)` loop | the same | yes |
| zero-timeout wait loop | the same | yes (new), and woken by its object |
| a loop that stores to memory or calls | no | no |

## The main thread (item 4 of the brief)

The device line gives 1.3 million empty yields in under two minutes: about
11,000 a second. With lean yield an empty yield is about 40 ns on the
runner, so on any core that is of the order of a millisecond a second. The
main thread's remaining 60 to 70% of a core is therefore not in empty
yields: it is the title's own code, the translator and real waits.
`repolls 2956/s` is the dispatch loops' timed passes, a few microseconds
each. So there is no core to win by parking the main thread harder.

What the new gate does for it: the 131,782 looks of that run (390 a second)
were bursts of yielding 200 us old with 32 yields inside 640 us. Each would
now be a park of 100 us or more, so a few per cent of a core, more where the
bursts are long. A thread that works for more than 20 us between its yields
is rightly never counted as spinning. The core to win back is Guest CPU 3,
which is a poll loop.

## Bench results

Runner numbers (a GitHub ARM64 runner), not phone numbers. They say what a
path costs relative to another on a server core; nothing about frame rate
or heat on a phone.

### Bench run 17: the phone's result reproduced on the runner

https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43651784030, green,
the code of `82aac63` with the scenarios as corrected after run 15
(`yield_with_work`, a varied delay, a fresh burst for each wait). Cores busy,
waiter never answered, about 6,500 unrelated events a second:

| Waiter (`_noisy`) | defaults | `park_waits` (no poll sites) | first gate + poll sites | adaptive + poll sites | adaptive, max 500 us | adaptive + skew bound |
|---|---|---|---|---|---|---|
| `yield_with_work` | 1.02 | 0.345 | **1.02** | **0.346** | 0.493 | 0.347 |
| `yield` | 1.02 | 0.026 | 0.081 | 0.025 | 0.032 | 0.026 |
| `zero_delay` | 1.02 | 0.028 | 0.086 | 0.027 | 0.034 | 0.027 |
| `zero_timeout_wait` | 1.02 | 0.038 | 0.214 | 0.037 | 0.052 | 0.037 |
| `poll_sled` | 1.02 | 1.02 | 0.070 | 0.025 | 0.030 | 0.025 |
| `poll_hint` | 1.02 | 1.01 | 0.070 | 0.025 | 0.031 | 0.025 |
| `poll_plain` | 1.02 | 1.02 | 0.069 | 0.025 | 0.030 | 0.026 |
| `spin_lock` | 1.01 | 1.02 | 0.071 | 0.025 | 0.031 | 0.026 |
| `poll_clock` | 1.02 | 1.02 | 1.03 | 1.02 | 1.02 | 1.02 |

- **`yield_with_work` with the first gate: 1.02, nothing parked.** This is
  the phone's result on the runner: a thread that does a few microseconds
  of work before each yield, beside unrelated events, is never parked by
  "no event since the last look". With the adaptive gate it is.
- **But only down to a third of a core, in this run.** The scheduler looked
  at a thread every 32 yields, also straight after a park, so this thread
  was awake for 32 passes of its work (about 260 us) for every park of at
  most a millisecond. Fixed after this run: back from a park, the next look
  comes after 4 yields (`kWaitPassesAfterPark`). The guest still runs its
  own test four times between two parks. The figures of the build with
  that fix are under "Bench run 18" below.
- The other rows are as in run 15.

Latency with the corrected scenarios: the flag is made true by a plain
store 2 to 4 ms after the waiter last took one; microseconds; median / 95th
percentile / worst:

| Waiter (`_answered_noisy`) | defaults | first gate + poll sites | adaptive + poll sites | adaptive, max 500 us |
|---|---|---|---|---|
| `yield` | 0.3 / 1.7 / 34 | 56 / 147 / 154 | 321 / 719 / 762 | 165 / 476 / 497 |
| `zero_delay` | 0.2 / 0.3 / 0.3 | 73 / 138 / 148 | 326 / 702 / 774 | 222 / 417 / 473 |
| `zero_timeout_wait` | 0.4 / 0.5 / 0.7 | 47 / 103 / 125 | 329 / 717 / 771 | 169 / 462 / 499 |
| `zero_timeout_wait`, its own event signalled | 0.4 / 0.5 / 0.6 | 11 / 78 / 99 | 11 / 14 / 16 | 11 / 14 / 17 |
| `poll_sled` | 0.3 / 0.3 / 0.3 | 81 / 149 / 155 | 262 / 642 / 690 | 151 / 451 / 523 |
| `poll_plain` | 0.2 / 0.3 / 0.3 | 82 / 150 / 152 | 254 / 703 / 826 | 246 / 488 / 529 |
| `spin_lock` | 0.3 / 0.4 / 0.4 | 54 / 143 / 153 | 299 / 775 / 802 | 237 / 447 / 499 |

- With the adaptive gate a wait of 2 to 4 ms is answered about 0.3 ms late
  in the middle and 0.7 to 0.8 ms at worst: the designed bound (a quarter
  of the wait so far, at most the longest park) and not more. With `max
  500 us` the worst is 0.5 ms.
- The first gate answers sooner here (0.15 ms at worst) because on this
  machine it is woken by the unrelated events, about every 150 us. That is
  the same behaviour that leaves it three to six times the CPU in the table
  above, and on the phone it did not park at all.
- A waiter that polls an object with a zero timeout sees that object's
  signal in 11 us.
- Quiet, `yield_answered_quiet`: 392 / 746 / 882 with the adaptive gate,
  549 / 962 / 1026 with the first (its parks double and reach the longest
  sooner).

### Bench run 18, the final code (`9d9b8e0`)

https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43653241495, green.
**This is the before and after the brief asks for.** "Before" is the column
"first gate + poll sites": the park as the phone ran it on 2026-11-14, with
this branch's recognition. "After" is "adaptive + poll sites".

Cores busy, the waiter never answered, about 6,500 unrelated events a
second:

| Waiter (`_noisy`) | defaults (no parking) | `park_waits` (no poll sites) | before: first gate + poll sites | after: adaptive + poll sites | adaptive, max 500 us | adaptive + skew bound |
|---|---|---|---|---|---|---|
| `yield_with_work` (the phone's case) | 1.02 | 0.093 | **1.01** | **0.096** | 0.134 | 0.097 |
| `yield` | 1.02 | 0.025 | 0.084 | 0.026 | 0.030 | 0.026 |
| `zero_delay` | 1.02 | 0.025 | 0.089 | 0.025 | 0.031 | 0.026 |
| `zero_timeout_wait` | 1.02 | 0.027 | 0.217 | 0.028 | 0.034 | 0.028 |
| `poll_sled` (the profile's shape) | 1.02 | 1.02 | 0.072 | 0.025 | 0.031 | 0.026 |
| `poll_hint` | 1.02 | 1.01 | 0.073 | 0.025 | 0.031 | 0.026 |
| `poll_plain` | 1.02 | 1.01 | 0.072 | 0.025 | 0.031 | 0.026 |
| `spin_lock` | 1.02 | 1.02 | 0.073 | 0.026 | 0.031 | 0.026 |
| `poll_clock` (left alone on purpose) | 1.02 | 1.02 | 1.02 | 1.02 | 1.02 | 1.02 |

Wake latency: the flag made true by a plain store 2 to 4 ms after the
waiter last took one, to the waiter seeing it; microseconds; median / 95th
percentile / worst; same noise:

| Waiter (`_answered_noisy`) | defaults (no parking) | before: first gate + poll sites | after: adaptive + poll sites | adaptive, max 500 us |
|---|---|---|---|---|
| `yield_with_work` | 6.3 / 14 / 15 | 6.4 / 13 / 14 (not parked) | 331 / 634 / 773 | 293 / 537 / 546 |
| `yield` | 0.3 / 1.4 / 38 | 58 / 143 / 145 | 261 / 720 / 823 | 173 / 516 / 533 |
| `zero_timeout_wait` | 0.4 / 0.5 / 0.5 | 56 / 104 / 113 | 326 / 706 / 720 | 257 / 496 / 547 |
| `poll_sled` | 0.2 / 0.3 / 0.4 | 83 / 146 / 153 | 342 / 642 / 787 | 204 / 489 / 522 |

Reading it:

- **The phone's case.** A thread that works a few microseconds before each
  yield, beside unrelated events: a whole core with the first gate (not
  parked, and its latency is the unparked one), a tenth of a core with the
  adaptive gate. Alone, the same thread is at 0.074 against 0.325.
- **Every shape that can be parked is**, at 0.025 to 0.03 of a core under
  the events, with the adaptive gate. The first gate leaves three to eight
  times that, because it is woken by the events.
- **The price** is the latency column: a wait of 2 to 4 ms is answered
  about 0.3 ms late in the middle and 0.7 to 0.8 ms at worst, inside the
  bound (a quarter of the wait so far, never more than the longest park).
  Unparked it is under a microsecond. `max 500 us` brings the worst to
  0.55 ms for about a fifth more CPU.
- **Nothing else moved.** `yield_other_ready` 280 ns in every column;
  the wait and wake round trips 2.15 to 2.18 us on one CPU and 20.0 to
  20.4 us across two, parked or not; `yield_nothing_ready` with parks at
  0.008 of a core as before the rework.
- The rig's notes of "NOT MEASURED: this tree has no setting called ..."
  in this run are all for the baseline tree (`claude/integration-1113`),
  which has none of these settings.

### Bench run 15, commit `82aac63`

https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43649224652, green.
Cores busy for a waiter that is never answered, beside about 6,500 unrelated
events a second (`_noisy`), by set of settings:

| Waiter | defaults | `park_waits` (no poll sites) | first gate + poll sites | adaptive + poll sites | adaptive, max 500 us | adaptive + skew bound |
|---|---|---|---|---|---|---|
| `yield` | 1.02 | 0.025 | 0.079 | 0.026 | 0.032 | 0.026 |
| `zero_delay` | 1.02 | 0.026 | 0.084 | 0.026 | 0.032 | 0.027 |
| `zero_timeout_wait` | 1.01 | 0.037 | 0.212 | 0.036 | 0.051 | 0.037 |
| `poll_sled` | 1.02 | 1.02 | 0.067 | 0.025 | 0.030 | 0.025 |
| `poll_hint` | 1.01 | 1.02 | 0.068 | 0.025 | 0.031 | 0.026 |
| `poll_plain` | 1.01 | 1.02 | 0.068 | 0.025 | 0.030 | 0.025 |
| `spin_lock` | 1.02 | 1.01 | 0.069 | 0.024 | 0.030 | 0.026 |
| `poll_clock` | 1.02 | 1.02 | 1.02 | 1.02 | 1.02 | 1.02 |

Alone (`_quiet`) every parked shape is at 0.009 to 0.021 of a core with
either gate, and `yield_nothing_ready` is 0.009 as before the rework.

What the table says:

- **Poll sites.** Without them (`park_waits` alone) every poll-loop shape
  stays at a whole core. With them, and the recognition as fixed here, all
  but the deadline loop are parked. That is the recognition fix, and it
  does not depend on the gate.
- **The adaptive gate against the first.** Beside the events the first gate
  leaves 0.07 to 0.21 of a core busy and the adaptive one 0.025 to 0.037:
  the first gate's wake path wakes the parked thread's dispatch thread for
  the events, the adaptive one learns not to.
- **The first gate did park here.** That is the caveat of this run: on the
  runner a bare yield loop's looks are a couple of microseconds apart, so
  "no event since the last look" usually holds even at 6,500 events a
  second, and the first gate parks. On the phone it never held. The
  difference is that the phone's waiting thread does a little work on each
  pass, which puts its looks hundreds of microseconds apart. The scenario
  `yield_with_work` (a few microseconds of additions before each yield) was
  added after this run for that; under `qemu-aarch64` it shows the first
  gate at a whole core and the adaptive one parked. Its runner figures are
  in the next run.
- **The deadline loop** is left alone, on purpose.
- **Half the longest park** costs about a fifth more CPU (0.030 against
  0.025).

Latency, from the flag made true (a plain store, about 3 ms after the
waiter last took one) to the waiter seeing it, microseconds, `_answered_noisy`:

| Waiter | defaults, median / worst | adaptive + poll sites, median / 95th / worst | adaptive, max 500 us |
|---|---|---|---|
| `yield` | 0.3 / 38 | 63 / 930 / 1033 | 137 / 247 / 391 |
| `poll_sled` | 0.2 / 0.4 | 536 / 728 / 758 | 319 / 500 / 508 |
| `zero_timeout_wait` | 0.3 / 0.5 | 76 / 927 / 998 | 206 / 273 / 359 |
| `zero_timeout_wait`, answered with its own event | 0.3 / 0.5 | 11 / 14 / 17 | 11 / 12 / 19 |

- The worst case is the longest park, as designed: 1 ms, or 0.5 ms with
  `max 500 us`.
- A waiter that polls an object with a zero timeout is woken 11 us after
  that object is signalled, whatever the noise.
- **Two artefacts in this run's latency figures**, both the scenario's and
  both fixed for the next run. The fixed 3 ms delay fell into step with the
  1 ms parks (hence a median of 63 us beside a 95th percentile of 930: the
  flag came just before a deadline most of the time). And the waiter went
  straight from one wait to the next with no work between, so to the
  scheduler the whole run was one burst and every park was the longest;
  the rule "a quarter of the wait so far" was therefore not what was
  measured. The delay is now varied and the waiter works for 0.8 ms after
  each wait.
- An answer followed by an unrelated event (`_answered_with_event_noisy`)
  is no faster than a plain store with the adaptive gate (median 70 against
  63): the thread has learnt from the noise to ignore events. The first
  gate is faster there (10 us) because it wakes for every event, which is
  what costs it the CPU above.

## What a device run must show

Config for the run: the working config, `host_thread_state_slot`,
`guest_scheduler_lean_yield`, `guest_scheduler_park_waits = true`,
`park_pause_poll_loops = true`, **`log_memory_poll_park = true`**,
`guest_scheduler_stats = true`, `host_perf_log = true`. Fable II, same
scene as run A, cold cache, unplugged.

Read, in this order:

1. At start: `GuestScheduler: busy waits are parked after 200 us, for 100 us
   at first and 1000 us at most, and woken by events while that proves worth
   it (adaptive gate)`.
2. `MemoryPollPark:` lines with a guest address inside `82B9BF90`'s
   function. Either `pause-and-poll loop at guest 82B9BFxx: N block(s), N
   load(s), N pause(s)[, every pass the same unless memory changes]`, or
   one `is not a pause-and-poll loop: <reason>` per backward branch. If it
   is refused, the reason is the next piece of work and the rest of the run
   says nothing about that thread.
3. The per-second `GuestScheduler: busy waits:` line, now with three more
   fields at the end:
   - `parked yield spinners N, poll loops N`: both were 0. Expect poll
     loops in the hundreds to thousands a second if the loop is marked.
   - `N us parked in all`: the core time given back per second. For Guest
     CPU 3 to fall from 99% to under 20% this must be near 800,000 or more.
   - `parks ended by the deadline N, by an event N`.
   - `poll site looks N, registers changed N`: was 0, 0. `registers
     changed` close to `looks` means the loop keeps a counter and is
     rightly not parked: that would be the next piece of work.
   - `early wakes: still waiting N, wait over N`: the learning. Under the
     title's events `still waiting` should be a handful a second (the
     hold-off climbing, and the probe every 32nd park), not thousands.
   - `events inside a hold-off N`: events that cost nothing. Thousands a
     second is expected and is the point.
   - `looks that did not park: an event since the last look` must now be 0
     (it is only counted by the first gate).
4. `host perf`, run and wait per guest thread: G3 from 99% of a core to a
   few per cent if its loop is parked; G0 from 60 to 70% by a few per cent.
   The frequency limit over five minutes against run A's 3628 to 1670 MHz.
5. Frames a second: run A held 30 from 80 s on. Not below that, and the
   seconds with a frame over 100 ms not more than run A's 6 in the first
   two minutes.
6. `late wake` lines: run A had 11, longest 36.5 ms in play. Not more, not
   longer. A late wake on a thread that is parked as a busy waiter would be
   this change's fault.
7. No freeze: no `no guest frame presented` report. If the skew bound is
   on, its line as before.

### What to switch off if a title misbehaves

In this order, one at a time:

1. `guest_scheduler_park_wait_max_us = 500` (or 300): shorter parks, if
   frame time suffers.
2. `park_pause_poll_loops = false`: no poll sites, only yield and wait
   loops are parked. Needs a restart (read when a function is translated;
   delete the title's translated-code cache if one is kept).
3. `guest_scheduler_park_wait_adaptive = false`: the first gate, which on a
   real title parks next to nothing.
4. `guest_scheduler_park_waits = false`: everything here off.

### Which settings I judge safe to become defaults, and when

- `guest_scheduler_park_wait_adaptive = true`: already the default; it only
  acts under `guest_scheduler_park_waits`.
- `guest_scheduler_park_waits = true` and `park_pause_poll_loops = true`:
  **not yet.** Both after one device run that shows items 2 to 7 above on
  Fable II, and a second title started and played for a few minutes with
  them on (any title; the point is a different engine's waits). The design
  is meant for every title: nothing in it is keyed to a title, an address
  or a loop count. What is unproven is only what a phone's timers do with
  100 us sleeps and whether any title's frame pacing sits inside a busy
  wait.
- `guest_scheduler_lean_yield = true`: it is implied by the parks and ran on
  the device in launch 1 and run A without harm. Safe to default with them.

## Risks

- **Latency**, as bounded above. The one case that could cost frames is a
  title that paces its frame with a busy wait on the time base or on a
  memory word the GPU writes: each pass is then up to a park late. The
  time-base loop is not parked; a wait on the GPU's word is (the GPU's
  write-back reports an event, but the thread may have learnt to ignore
  events). `max_us` is the lever.
- **Timer slack.** A 100 us sleep is at the mercy of the phone's timer
  slack (50 us by default; far more for a process the system has put in the
  background). If parks come back late, `parked in all` is high and frame
  time suffers: lower the maximum, or check the process is foreground.
- **Learning the wrong thing.** A thread whose events are mostly noise and
  occasionally the real thing learns to ignore them and then pays the
  deadline (at most 1 ms) for the real one. That is the price of not being
  woken thousands of times a second; the bound holds regardless.
- **The learning table** has 64 entries; a 65th busy-waiting thread alive
  at once is parked without learning (events end its parks after 50 us).
- **More poll sites** than before: each is a call a pass. Only on loops
  that pause or are stateless.
- **Zero-timeout waits** now park, where before only yields did. A title
  that polls an object with a zero timeout in a tight loop as its frame
  pacing would be up to a park late per pass unless that object is what
  gets signalled (then it is woken at once).
- **x64 backend**: unchanged from `guest-waits.md`: do not switch
  `park_pause_poll_loops` on there.

## Upstream files changed

Under `x360/emulator-core/src/main/cpp/xenia/src/xenia/`, every change
marked `hakuX-Boosted:`. All in the files the brief gives me.

| File | Change |
|---|---|
| `kernel/guest_scheduler.h`, `.cc` | the setting; `WaitLearn` and its table; `TryWaitPark` (the gate, park length, learning); `WaitPark` takes the hold-off; `WakeParkedWaits` (events inside a hold-off); `NoteEmptyWait`, `WakePolledWaits`; the hold-off's bookkeeping in `BlockCurrentThread` and `RereadyBlocked`; entries freed in `NotifyThreadExited` and `ForgetThread`; three fields on the `busy waits:` line; the start line |
| `kernel/xboxkrnl/xboxkrnl_threading.cc` | `xeKeWaitForSingleObject` and `NtWaitForSingleObjectEx` report a zero-timeout wait that timed out |
| `cpu/compiler/passes/memory_poll_park_pass.cc` | candidates not merged; a site at every branch back to the head; `IsStatelessLoop`; reserved loads |

`cpu/backend/a64/a64_seq_memory.cc` is unchanged: the site's call is as it
was. `kernel/xthread.h` is unchanged: what a thread has learnt is kept in a
table in the scheduler, because that file is not mine.

`boosted/bench/`: `core/bench_sched.cc` (the scenarios), `core/core_main.cc`
(`BENCH_CORE_LOG`), `core/run-core.sh` (the rows), `tools/report.py` (the
metrics' names), `CMakeLists.txt` (the libstdc++ fallback's note no longer
breaks the build).

## For the merge: changes in files I do not own

None is needed for this branch to work. Two would help:

1. `kernel/xboxkrnl/xboxkrnl_threading.cc` is mine, but
   `NtWaitForMultipleObjectsEx` and `KeWaitForMultipleObjects` with a zero
   timeout are not reported (they name several objects; the table keeps
   one). Left for when a title shows such a loop.
2. `kernel/xboxkrnl/xboxkrnl_rtl.cc`: `RtlTryEnterCriticalSection` failing
   again and again is a busy wait too, and the section's address is what
   `RtlLeaveCriticalSection` could name. With
   `scheduler->NoteEmptyWait(<a stable pointer for the section>)` on the
   failed try and a matching targeted wake on the leave, a try-lock loop
   would be woken by its own lock instead of by its deadline. Not written:
   it needs the hook in `RtlLeaveCriticalSection` to pass the section.

## What the build and the rig could not verify

- Everything on a phone: that 100 us parks sleep that long, that the
  learning settles as it does on the rig, that the frame rate holds, that
  the freeze stays away.
- That Fable II's loop is now marked. The rig shows the defect and the fix
  on loops of the profile's shape; the title's own loop may be refused for
  another reason, which the log line will name.
- The zero-timeout wait under `qemu-aarch64` is too slow to count as
  spinning (82 us a call there); its park and its targeted wake were seen
  to work only with the cadence thresholds relaxed for the test. The
  runner's figures decide.
- The first gate's behaviour (`adaptive = false`) is as it was by reading;
  it shares `WakeParkedWaits` and `RereadyBlocked` with the new one.

## What is next

- Read the device's `MemoryPollPark:` lines for `82B9BFxx` and act on the
  reason if the loop is refused.
- If `registers changed` is high for that loop: it keeps a counter. The
  pass could tell the site which registers the loop only counts with, and
  the hash leave them out.
- Deadline loops (`mftb`): the same idea, leaving time-derived registers
  out of the hash, with the park clipped to the loop's own deadline if that
  can be read.
- The try-lock hook of "For the merge".
- A finer sleep than the condition variable for parks under 100 us, if the
  phone's slack allows.

## Build

- `82aac63` (the code but for the last fix): bench run 15, green,
  https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43649224652. App
  run 137, https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43649224679:
  "Xbox core" and "APK (both cores)" green; its "Translator tests" job
  (about 100 minutes) was still running when the next push replaced the
  run.
- `161b3e7` (the same code, the corrected scenarios): bench run 17, green,
  https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43651784030.
- **`9d9b8e0` (the final code, with the look after four yields): bench run
  18, green,**
  https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43653241495. App
  run 139, https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43653241496:
  result below when known. The commits after it change these notes only
  and are marked `[skip ci]`.

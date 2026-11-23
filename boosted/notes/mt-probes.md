# Our multi-threaded xsdk test titles

xsdk's own probes run on one hardware thread, so they cannot show the races
our evidence points at: ARM64 memory ordering (the Gears of War ring-buffer
crash that strict ordering removed in 5 of 5 loads) and a kernel wait-all
fault. These three titles are ours, written against xsdk's headers and
built in xsdk's tree with its compiler flags, link and `elf2xex.py`.

**None of the console results below has been verified on a console.** We
have no console. They are what the PowerPC architecture and the NT-style
kernel define, and where the architecture leaves room, what the brief
assumes of the in-order Xenon; each such case says so.

## Where they are and how they are built

- Sources: `boosted/xsdk-tests/` (`hakux_mt.h`, `hakux_waits.c`,
  `hakux_order.c`, `hakux_reserve.c`, `hakux.mk`).
- `.github/workflows/xsdk-probes.yml` (pushes to `xsdk/**` only) checks
  this repository out, copies the sources into xsdk's `tests/` and
  `hakux.mk` beside its Makefile, and runs
  `make -f Makefile -f hakux.mk TOOLCHAIN=... hakux_titles`. The step fails
  the run if a title does not build, but the artifact is still uploaded.
- The `.xex` files land in the `xsdk-probes` artifact as
  `xex/build/hakux_waits.xex`, `xex/build/hakux_order.xex`,
  `xex/build/hakux_reserve.xex`; the build log is `build-hakux.log`.
- They run under the same hands-off harness as xsdk's probes
  (`tools/run-xsdk.sh`); each writes its log where xsdk titles do.

## What every title does the same way

- Lines `PASS: <title>: ...` and `FAIL: <title>: ...` for checks;
  `INFO: ...` for counts the architecture allows and we only report.
- `<title>: step: ...` is logged before every blocking step, so a log that
  stops shows where.
- `<title>: N passed, M failed`, then xsdk's own `exit: status M`: the exit
  status is the failure count, 0 when all pass.
- Every kernel wait has a timeout (2 s for waits that should be satisfied
  soon), so a lost wake-up is a FAIL, not a hang. A worker that does not end
  in time aborts the title (`FAIL: ...: aborted`), as it may still touch the
  main thread's stack.
- A watchdog thread on hardware thread 5, at the highest priority, ends a
  title at its time limit (300 s each) with
  `FAIL: <title>: time limit of 300 s reached during: <step>`.
- The main thread runs on hardware thread 0.

## hakux_waits: kernel waits across hardware threads

| Step | What it checks | Console (expected, not verified) |
|---|---|---|
| Auto-event ping-pong, 5000 rounds, with hardware thread 1 (same core) and 4 (other core) | Two synchronization events, one each way: every wake-up arrives, each wake consumes its signal, none is left over | PASS; a round trip of some microseconds |
| Manual-event broadcast, 4 waiters on 1-4, 1000 rounds | A notification event wakes every waiter; two alternate so no waiter passes twice; arrivals counted through a semaphore | PASS, 4000 arrivals |
| Semaphore bounded buffer, 2 producers (1, 3), 2 consumers (2, 4), 5000 items each, 16 slots | Every item passes; the count never exceeds the limit (from `previous_count`); drained to 0; a release past the limit is refused | PASS; NT returns `STATUS_SEMAPHORE_LIMIT_EXCEEDED` (0xC0000047) |
| Mutant contention, 4 threads on 1-4, 2000 rounds, taken twice each (recursive) | A non-atomic read-modify-write under the mutant comes out exact with no overlap; the mutant ends free | PASS, total 8000 |
| Wait-any, single thread | With objects 1 and 3 signalled, wait-any returns 1, then 3, then times out, each consuming one auto event | PASS |
| Wait-any across 4 threads (1-4), 1000 rounds each | Every signal is seen exactly once, counted per index | PASS |
| Wait-all short of one object | Times out after 5 ms and consumes none of the signalled auto events | PASS (NT's wait-all is all or nothing) |
| Wait-all whose last object comes from hardware thread 3 after 20 ms | Completes, and consumes all three auto events | PASS |
| Mixed wait-all: semaphore (2), auto event, manual event, free mutant | Takes one semaphore count, clears the auto event, leaves the manual one set, owns the mutant | PASS |
| Wait-all across 3 threads (1, 2, 4), 1000 rounds | Completes every round; no event left signalled after it | PASS |
| Wait-all of a thread object and an auto event, 300 rounds on 1-5 | Times out while the thread runs (without taking the event); completes once the thread ends by return, by `ExTerminateThread`, or after a spin; every fourth round the thread has ended before the wait, otherwise the wait races its end; the thread stays signalled afterwards and the event is consumed | PASS; this is the step aimed at the wait-all fault |
| Wait-all of three threads ending together (1, 3, 5), 200 rounds | Completes, and a second wait-all on them returns at once | PASS |
| Signal-and-wait ping-pong with hardware thread 3, 5000 rounds | `NtSignalAndWaitForSingleObjectEx` never loses a wake-up; no signal left behind | PASS |
| Timeouts while hardware threads 1-5 spin | 1000 zero-timeout waits time out; 200 waits of 2 ms (single, wait-any, wait-all) time out within 1 ms to 200 ms; then a 500-round ping-pong with every hardware thread loaded | PASS; the console's timer tick adds about a millisecond, so a 2 ms wait takes 2 to 4 ms |

## hakux_order: memory-ordering litmus tests

Each test runs up to its iteration count or 10 seconds, whichever comes
first, and logs how many iterations it ran. Writers run on hardware thread 2;
readers on 4 (another core) or 3 (the same core). Shared words each have a
128-byte line of their own.

**The ring buffer** is modelled on Gears of War's: 16 slots of four words;
the writer waits for space by polling the read pointer, stores the four
words, `lwsync`, then advances the write pointer. The reader polls the write
pointer and reads the slot with no barrier (its address does not depend on
the pointer just loaded), then advances the read pointer. Each word encodes
the item's sequence number differently, so a slot read wholly or partly from
before the writer's stores is counted as **stale**; one already overwritten
by a later item is counted as **newer**.

| Test | Checked or reported | Console (expected, not verified) |
|---|---|---|
| Ring, reader with no barrier, threads 2 and 4 | FAIL if any stale slot | 0 stale. The architecture allows the reader's slot loads to pass its pointer load; the brief takes the in-order Xenon never to do it, which is what the game relies on. If the console did show stale slots the game would crash there too, so 0 is the working assumption. In our core on ARM64 any stale count is the reordering we are hunting |
| Ring, no barrier, threads 2 and 3 (same core) | FAIL if any stale | 0 |
| Ring, reader `lwsync` between pointer and slot, 2 and 4 | FAIL if any stale (a control: required by the architecture) | 0 |
| Ring, newer (overwritten before read) | Reported | Likely 0: needs the reader's read-pointer store to pass its own slot loads |
| Message passing, no barriers | Reported | Architecture allows stale reads; count unknown, possibly 0 on the in-order cores |
| Message passing, writer `lwsync`, reader none | Reported | Architecture allows stale reads (load-load reordering); expected 0 on the console by the same assumption as the ring |
| Message passing, writer `lwsync`, reader `lwsync` (other core and same core) | FAIL if any stale | 0 (required) |
| Message passing, writer `lwsync`, reader compare-branch-`isync` | FAIL if any stale | 0 (required) |
| Message passing, `sync` both sides | FAIL if any stale | 0 (required) |
| Store buffering, no barrier | Reported (both loads missing the other's store) | Allowed and expected to be seen: a store waits in the store queue while the later load reads the cache. Not verified |
| Store buffering, `lwsync` | Reported | Allowed: `lwsync` does not order a store before a later load |
| Store buffering, `sync` (other core and same core) | FAIL if both loads ever miss | 0 (required) |

How to read our core's results: a FAIL on any barrier-protected test means
the JIT drops or weakens that barrier on ARM64. A FAIL only on the
no-barrier ring, with the protected tests passing, means the core's ARM64
loads reorder where the in-order console's do not: what the Gears crash
suggests, and what the strict-ordering setting removed.

## hakux_reserve: lwarx/stwcx. across hardware threads

The increment loop and the reservation steps are inline assembly; the main
thread runs at the highest priority because an adder spins on hardware
thread 0 too.

| Test | Checked or reported | Console (expected, not verified) |
|---|---|---|
| 2 threads on one core (0, 1), 2 on two cores (0, 2), then 3, 4, 5 and 6 hardware threads, 200000 increments each | FAIL unless the counter is exactly threads times 200000; retries reported | PASS |
| 2 adders (0, 2) with 2 threads (1, 3) storing plainly to another word of the counter's 128-byte line | FAIL unless exact; retries reported | PASS, with many more retries, as each plain store to the line kills the reservation |
| Thread A (2) takes a reservation, thread B (4) stores to a word on another line, A's `stwcx.` | FAIL unless at least half succeed (a control) | Nearly all succeed |
| B stores a different value to the reserved word | FAIL if any `stwcx.` succeeds | 0 succeed (required) |
| B stores the same value to the reserved word | FAIL if any succeeds | 0 succeed: the architecture loses a reservation on any store to the granule, whatever the value. An emulator that implements `stwcx.` as compare-and-swap on the loaded value succeeds here, the ABA hole |
| B does its own `lwarx`/`stwcx.` of the same value | FAIL if any succeeds | 0 succeed (required) |
| B stores to another word of the same 128-byte line | Reported only | 0 succeed (the reservation granule is the line), but no game should depend on it |
| Different value and same value again with B on hardware thread 3 (same core) | FAIL if any succeeds | 0 succeed |

Between A's `lwarx` and `stwcx.` A only stores to a line of its own (its
handshake word) and spins on loads; it reads no clock there, as a kernel call
could clear the reservation. A handshake that waits more than 50 million
spins is given up and the step fails.

## Built

First built green in run 45235613650 (commit 77682b3), artifact
`xsdk-probes` id 14849463114. The `hakux_titles` target succeeds only when
all three `.xex` files are made; the artifact's contents were not inspected
from here (no token to download it).

## Not done

- Not run on the phone yet (the phone was busy with another batch). The
  first run should be the three titles with the default core settings, then
  `hakux_order` again with strict memory ordering on, to see the ring's
  stale count go to 0.
- The expected console column is reasoning, not measurement.

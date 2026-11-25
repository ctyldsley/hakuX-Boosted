# Spin probe (branch `claude/spin-probe`)

Built for the Gears of War freeze at the door (title 4D5307D5): the Main
XThread (`F8000008`) runs at 97% of a core with `lr=82613800`, about 560
zero-delay waits and 2.6 million empty yields a second, while the job
workers wait on events last set 60 to 100 s before. The thread dump only
gives `lr`; the probe says where the thread loops, what it calls and what it
reads. It works for a hang in any title.

## How to trigger it

With the title running and the hang showing:

    adb shell touch <storage root>/spin_probe

`<storage root>` is the folder that holds `xe.log` (the same place as the
`dump_threads` trigger, `.../files/compose/`). The scheduler's watchdog
sees the file within a second, deletes it, writes
`SpinProbe: recording for 3 s (frame N)`, records for `spin_probe_seconds`
(Kernel section, default 3, 1 to 60), then writes the report followed by the
usual thread dump ("thread dump on request"). Touch it again for another
window; a touch while one runs waits until it ends.

Nothing is recorded until the file appears. Not running costs one relaxed
flag test in the kernel call path (inside the stall watchdog's existing
trace branch) and nothing in translated code (see below).

## Reading the report

    SpinProbe: 3.0 s recorded (frame N), a sample per running thread every 1000 us. ...
      tid=F8000008 'Main XThread': 2950 safepoint samples, 1693000 kernel calls (564333/s)
        at 82613780: 2800 (94.9%), lr 82613800
        at 826135C0: 120 (4.1%), lr 82613800
        call KeDelayExecutionThread from 826137A0: 1650 (550/s); (00000001,00000000,7004F9A0,...)=00000000 ...
        call NtYieldExecution from 82613790: 1690000 (563333/s); (...)=40000024 ...
        loads of the loop at 82613780:
          82613784 lwz r11,0x10(r31): [40A21C10] = 00000000 (read 700x); values 00000000
          8261378C lbz r10,0x4(r11) ?: [00000004] = unreadable (read 700x, 700 unreadable); values none
    SpinProbe: end of report

(The numbers above are made up to show the format.)

- **`at XXXXXXXX: n (p%), lr L`**: guest addresses of the JIT safepoints the
  thread was stopped at. A safepoint is the head of a block that a backward
  branch can reach, so the top address is in practice the loop's head. `lr`
  is the guest LR at the first sample there (the loop's function's caller,
  unless the loop itself calls). Samples are taken only from threads that
  are running when the watchdog ticks (once per `guest_scheduler_quantum_us`,
  at least the watchdog's minimum period); a thread blocked in a wait has no
  `at` lines but may have calls.
- **`call NAME from SITE: n (r/s); (r3,r4,r5,r6,r7,r8)=result ...`**: every
  kernel or XAM export that returned during the window, per call site.
  `SITE` is the guest LR at the call, the return address (the `bl` is at
  SITE-4). Up to the first four calls are shown with their arguments and
  return value (0 for exports that return nothing). A call that blocks for
  the whole window and never returns is not counted (it is in the thread dump
  that follows).
- **`loads of the loop at PC`** (for each thread's three busiest sites): from
  PC the guest code is decoded to the loop's back edge (a `bc` to PC or
  earlier) or the first unconditional way out, and every integer load in it
  (`lwz`, `lbz`, `lhz`, `ld`, the indexed and update forms, `lwarx`, the
  byte-reversed loads) is read: `[EA]` is the address from the registers as
  they stood at the safepoint, then the last value read, how many reads, and
  up to four distinct values seen (`+n more` past those). A load is read at
  a site's first sample and every fourth after.
  - `?` after the instruction: its base (or index) register is written by an
    earlier instruction in the loop (or by a call), so the address is the
    loop-entry value of that register plus the offset: a guess. A load with
    no `?` read the exact address the guest reads on that iteration.
  - `address changed n x, first F`: the computed address moved between
    samples (a pointer that is reloaded, or a walk).
  - `unreadable`: the address is not mapped for the host (an MMIO range such
    as the GPU's registers, or a wrong guess). It is read through a pipe
    (`write()` returns EFAULT), so this never faults.
  - The flag the thread polls is usually the load whose value never changes
    and whose address is fixed; the thing that should set it is then found by
    searching for stores to that address (or by a memory watch on it).
- `Samples with no address`: samples where the safepoint had no guest
  address (code translated before this build would show here; none should).
  `entries dropped`: a table was full (4096 sites, 4096 call sites, 2048
  loads); the counts are then lower bounds.

## How it works

- `kernel/spin_probe.cc`, `.h` (new): the tables (fixed, open-addressed,
  lock-free; every key holds the guest thread id, so each slot has one
  writer), the loop decoder, the safe read and the report.
- `GuestScheduler::WatchdogLoop`: the trigger, beside `dump_threads`; while
  the window is open, every tick, for each CPU with a running fiber, sets
  `Cpu::spin_probe_owed` and raises the fiber's `preempt_requested`. At the
  end it restores `g_kernel_call_trace_enabled`, calls `SpinProbeEnd` (the
  report) and `ReportNoProgress(true)` (the thread dump).
- `PreemptCurrentFiber` (the safepoint handler) first calls
  `SpinProbeAtSafepoint`, which takes the CPU's mark and records the sample
  (`last_safepoint_pc`, LR, the loop's loads). If the probe alone raised the
  flag (`FlagOnlyForProbe`: nothing preempted, no terminate, suspend or
  shutdown, slice not over; the same test the skew bound's probe uses, now
  shared) the handler returns without yielding. Otherwise the yield goes on
  as it would have.
- `a64_emitter.cc`, `EmitPreemptCheck`: the cold path that calls the handler
  now first stores the check's guest address into `last_safepoint_pc` (w17
  with the extended register pool, else w9, as `log_safepoint_pc` does). The
  usual path (load the flag, branch) is unchanged, so translated code runs
  the same instructions; each check's out-of-line tail grows by 8 to 12
  bytes. A side effect: `last_safepoint` in every thread dump is now the last
  yield's address even with `log_safepoint_pc` off (before, it was 0).
- `stall_watchdog.cc`, `KernelCallTraceLeave`: when the probe runs, the
  finished call (name, LR, r3..r8 at entry, result) goes to
  `SpinProbeNoteCall`. The trace is the stall watchdog's (on by default); the
  probe forces it on for its window and restores it after, so it works with
  `stall_watchdog = false` too.
- `boosted/bench/core/CMakeLists.txt`: the bench library compiles
  `spin_probe.cc`, since `guest_scheduler.cc` calls it.

## Limits

- Exports reached some other way than the shim trampoline (none known) and
  guest code the scheduler handles inline (a poll or delay site skipped by
  `park_poll_site_backoff`) do not show as calls; the `at` lines still show
  where the thread is.
- A load's address uses the registers at the loop's head. Only loads with no
  `?` are certain.
- The probe stops running fibers more often (one extra safepoint call per
  tick per CPU, which returns at once). In a window that should not change
  what the title does, but a race that the hang depends on could in
  principle move.
- Not run on a device. Built by Actions only (see below).

## Builds

- Branch `claude/spin-probe` from `claude/merge-1124` (`f85f707`).
- Code commit `1a2306c`: app run 182 (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/45585988389), "Xbox core" and "APK (both cores)" green; artifact `hakuX-Boosted-debug-apk`, 36,235,833 bytes, digest sha256:e1ce97e2c2ddc7cb3c0dee9432e88dce60555701da9036b837ddec628d7b35fc. Bench run on the same commit green. "Translator tests" and the JIT difftest were still running when this was written.

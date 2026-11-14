# bench-harness: numbers without the phone

Branch `claude/bench-harness`, cut from `claude/integration-1113` at
`91b7a7a`. Files: `boosted/bench/**`, `.github/workflows/boosted-bench.yml`,
this file. No file of another session's row is touched; nothing in the app
changes.

## Done / in progress / next

- Done: a benchmark rig that builds the Xbox 360 core's own CPU, scheduler
  and kernel-object sources for ARM64 Linux (no Android, no changes to them)
  and runs on GitHub's hosted ARM64 runner (`ubuntu-24.04-arm`; QEMU was not
  needed). Three parts: the host primitives the profile names; the guest
  scheduler's real paths, driven by guest threads that call the real kernel
  exports; six synthetic PowerPC loops through the translator. Every setting
  the brief names is run off and on. The workflow measures a branch next to
  `claude/integration-1113` built and run in the same job on the same
  machine, prints both and the difference in the job summary, and uploads
  the raw numbers.
- Done: green on Actions with real numbers (see "First results").
- Done: `claude/guest-waits` measured against `claude/integration-1113`
  with the rig (see "First results"). `claude/translator-wide` did not exist
  yet when the rig last ran; it is in `boosted/bench/branches.txt` and is
  measured at the next push to this branch once it does.
- In progress: nothing.
- Next, in the order I would do them:
  1. Build the core itself against bionic in the Termux container that
     the primitives already run in (see "The bionic job"), so that section 2
     runs with Android's linker and the real `TLS_SLOT_APP` instead of the
     stand-ins described under "What differs from the app".
  2. Benchmarks for what the other profile lines name and the rig does not
     reach: `mprotect` and the memory watches (`claude/memory-watches`), the
     GPU command thread's `memmove`.
  3. A guest loop that register pinning accepts (see "Findings", 3): today
     none of the six does, so `pin_guest_registers` is measured as "changes
     nothing", which is true of these loops and says nothing of its worth.
  4. Record and replay of a title's kernel-call sequence (calls and
     timings only, no title code) to drive section 2 with a real mix
     instead of pure loops.

Nothing here has run on a device, and nothing here is a claim about the
phone. See "What an Actions runner can and cannot say".

## How to run it

On GitHub: every push to a `claude/**` branch that carries the workflow file
runs it for that branch. "Run workflow" (or the API) takes a branch name and
measures that branch with the rig of the branch the workflow is run from.
A push to `claude/bench-harness` also measures every branch listed in
`boosted/bench/branches.txt`, each in its own job: that is how a branch cut
before the rig existed is measured without carrying the workflow. To measure
another branch tonight, add its name to that file and push.

The workflow file is not on the default branch, so GitHub's web page does
not offer "Run workflow" for it; the API and `gh workflow run
boosted-bench.yml --ref claude/bench-harness -f branch=<name>` do. Once the
rig is merged to the default branch the button appears.

Locally, on any ARM64 Linux machine with clang, CMake and Ninja:

```
git submodule update --init --depth 1 -- $(sed -n 's/^\([a-z].*\)$/x360\/emulator-core\/src\/main\/cpp\/xenia\/third_party\/\1/p' boosted/bench/tools/submodules.txt)
cmake -S boosted/bench -B /tmp/bench -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C /tmp/bench
boosted/bench/run.sh /tmp/bench /tmp/results.jsonl 2
python3 boosted/bench/tools/report.py --target /tmp/results.jsonl
```

`-DBENCH_TREE=<a checkout or a tree made by tools/prepare-tree.sh>` builds
the same rig against another branch's sources; `report.py --baseline
<other results>` prints the difference. On an x86 machine the rig
cross-compiles with `clang --target=aarch64-linux-gnu` and runs under
`qemu-aarch64` (that is how it was developed in the cloud session); the
times are then meaningless but every benchmark runs and checks its results.

One job takes about five minutes from a cold cache (three of them the build
of about 400 source files), under the 20 the brief allows.

## How to read it

Each number is the lowest of several samples (noise on a shared machine only
adds time). "noise" is how far the middle sample sat above the lowest.
"change" is the measured branch against `claude/integration-1113`, negative
is cheaper; it is in bold only when it is over 5% and over twice the noise.
When the measured branch has the same core sources as the baseline (this
branch does) both columns are the same programs run turn about, and the
"change" column shows what this runner's noise looks like: use it to judge
the other reports.

- Section 1, primitives: nanoseconds for one call of a function that is not
  inlined. `call_empty` is the call and return alone; subtract it.
- Section 1b, `fiber_switch_to`: the core's `Fiber::SwitchTo`, per switch.
- Section 2, scheduler: nanoseconds per pass of a guest loop, one kernel
  call a pass. `loop_alone` (no call) and `kernel_call_that_does_nothing`
  (`KeGetCurrentProcessType`: translated code, the guest-to-host thunk, the
  export's shim and back) are what to subtract to see the scheduler's own
  share. "cores busy" is the whole process's CPU time over wall time during
  the run: 1.0 is one dispatch thread spinning. Parking a spinner shows
  there and not in the time per call (the time per call goes up, because
  the thread is asleep; that is the setting working, not a regression).
- Section 3, translator: nanoseconds and host instructions per guest
  instruction inside the loop (from two run lengths, so entry and exit
  cancel), host instructions emitted per guest instruction for the whole
  function, and microseconds to translate. "(translator counters)" are the
  core's own `translation_stats`, to show whether a setting did anything.

A row that says NOT MEASURED or WRONG RESULT is a finding, not a number: a
benchmark hung, a setting does not exist on that branch, or translated code
computed something else than the C++ model of the same loop.

## What an Actions runner can and cannot say

It can say: this code path costs this much relative to that one, on an
ARM64 server core; this change made this path cheaper or dearer by this
much; this setting changes the path at all; the translated code is still
right.

It cannot say:

- Anything about frame rate, or how much of a frame a path is. The profile
  on the phone says how often a path runs; the rig says what one run costs
  here. Multiply with care.
- What the phone's cores do. The runner is a server CPU (see the "Runner"
  rows of a report for the model) with other caches, other branch
  predictors and a higher, steady clock. Ratios between two paths carry
  over better than absolute times; neither is guaranteed.
- Heat. The phone lowers its frequency limit from 2.75 GHz to 1.44 GHz in
  five minutes; the runner never does. "cores busy" is the nearest thing
  here to a heat figure.
- The Android linker, except in the bionic job (below). Sections 1b, 2 and
  3 run on glibc. glibc's dynamic TLS resolver (`_dl_tlsdesc_dynamic`) is
  not bionic's (`tlsdesc_resolver_dynamic`); they do the same job (look up
  the module's block in the thread's vector) with different code.
- Scheduling on the phone. Six dispatch threads on the runner's four equal
  cores, no big and little cores, no frequency floor, nice values not
  applied (the runner's user cannot raise priority).
- Anything about the GPU, shaders, audio or file access.

## What is measured, and with whose code

### 1. Host primitives (`boosted/bench/prim`)

A small program of its own; the only core source it uses is the fcontext
assembly. Thread-local reads: through the TLS descriptor of a `-fPIC`
library loaded with `dlopen` whose thread-local block is too large for
glibc's spare static space, which forces the loader's dynamic resolver (the
program checks it got that: a block's offset from the thread pointer then
differs between threads); the same library loaded at start-up (static
resolver); initial-exec; a pthread key; a fixed slot (`mrs tpidr_el0; ldr
[..., #16]`, the instructions `host_thread_state_slot` uses); a pointer
already in a register. `clock_gettime`, `steady_clock::now` (what
`GuestScheduler::NowMicros` calls) and the counter register (what
`Clock::host_tick_count_raw` reads). An uncontended pthread, `std::mutex`
and recursive mutex. The two atomics the profile names, a 16-bit
compare-and-swap with acquire ordering (`__aarch64_cas2_acq`) and a relaxed
64-bit add (`__aarch64_ldadd8_relax`), through the outline helpers as an
`armv8-a` build gets them, as inline LSE instructions and as an inline
LL/SC loop. `jump_fcontext`, per switch.

### The bionic job

The same primitives program is also built inside a Termux container
(`termux/termux-docker:aarch64`, which carries Android's `/system`: bionic
and `linker64`) with Termux's clang, and run there, with the glibc build
run on the same runner beside it. That gives bionic's own
`tlsdesc_resolver_dynamic`, its pthread mutex, its `clock_gettime` and
compiler-rt's outline atomics, on the runner's CPU. The job only runs for
this branch and on manual dispatch, and is allowed to fail (it depends on
Docker Hub and Termux's package mirror) without failing the run.

### 1b and 2. The scheduler (`boosted/bench/core`, `bench_sched.cc`)

Built from the core's sources, unmodified, compiled with the app's flags
(`-O2 -DNDEBUG -fno-strict-aliasing -march=armv8-a -fPIC`, frame pointers
kept) into one shared library:

- all of `base/`, `cpu/`, `cpu/backend/a64/`, `cpu/compiler/`, `cpu/hir/`,
  `cpu/ppc/`, and `memory.cc`;
- from `kernel/`: `guest_scheduler.cc`, `xthread.cc`, `xobject.cc`,
  `xevent.cc`, `xsemaphore.cc`, `xmutant.cc`, `xtimer.cc`,
  `kernel_flags.cc`, `util/object_table.cc`, `util/native_list.cc`,
  `util/shim_utils.cc`, `xboxkrnl/xboxkrnl_threading.cc`,
  `xam/user_settings.cc`, `xam/user_data.cc`.

Guest threads are made with `XThread::Create` (so: a real guest stack, PCR,
KTHREAD, a fiber, `MarkReady`) and run PowerPC loops written for the rig.
The loops call `NtYieldExecution`, `KeDelayExecutionThread`, `NtSetEvent`,
`NtWaitForSingleObjectEx` and `KeGetCurrentProcessType` through import
thunks of the shape `cpu/xex_module.cc` writes for a title's imports
(`sc 2; blr`), bound to the exports' own trampolines. So a call goes through
translated code, the backend's guest-to-host thunk, the export's shim, the
kernel function and `GuestScheduler`, as in the app. Threads exit through
the real `XThread::Exit`.

Benchmarks: an empty `NtYieldExecution` (one thread on its CPU); a zero
delay; a yield with another fiber of the same priority ready on the same
CPU (two threads yielding to each other); an event handed back and forth
between two threads on one CPU, and on two CPUs (two dispatch threads).
Each runs for 60 ms a sample, five samples, with: defaults;
`guest_scheduler_lean_yield`; `guest_scheduler_park_yield_spinners`;
`host_thread_state_slot`; `guest_scheduler_skew_bound_us = 2000` (cap
multiple 100, as on the phone); lean yield with the slot; all of them.

### What differs from the app (section 2)

Exactly these, and nothing else that I know of:

1. `kernel/kernel_state.cc` is not built (it needs the system libraries,
   file systems, audio and graphics). `core/kernel_shim.cc` stands in: a
   `KernelState` whose memory is zeroed and of which only the members those
   sources read are constructed (the object table, the global lock, the
   thread map, the DPC list, the scheduler). `RegisterThread`,
   `UnregisterThread`, `OnThreadExit` and `kernel_state()` are copied in
   substance; `OnThreadExecute` does nothing (no DLL is loaded); the title
   process block is filled in as `InitializeProcess` fills it. Fifteen
   other functions those sources refer to on paths the benchmarks do not
   take stop the program if reached (none is).
2. The loader is glibc's. The library is loaded with `dlopen` and carries a
   64 KiB thread-local pad so that its thread-locals go through glibc's
   dynamic TLS resolver, as bionic's go through its own; each run reports
   which it got. A second launcher links the library in the ordinary way
   (static TLS, "not Android's") to show how much of a path is thread-local
   access at all.
3. `host_thread_state_slot`: `base/host_thread_state.h` compiles the slot
   code for Android only. The build puts a generated header before it on
   the include path which reads that one header with `__ANDROID__` defined
   and nothing else, and `android_get_device_api_level()` is answered with
   35. The slot itself, the word at the thread pointer plus 16, is bionic's
   `TLS_SLOT_APP` on the phone; on glibc the same address is the first word
   of the executable's static TLS block, and the launcher's only
   thread-local is put there. The instructions executed are the same.
4. `XE_PLATFORM_xendroid` is defined, so the fork's own code paths are the
   ones compiled (its JIT unwind table, its guest memory mapping, its
   POSIX threading branches). Two bionic calls those paths use are supplied
   for glibc in `core/compat`: `pthread_gettid_np` and
   `ASharedMemory_create` (a memfd).
5. Dispatch-thread priorities (`guest_threads_above_helpers`) are asked for
   and not granted on the runner.
6. Nothing else runs: no GPU command thread, no audio, no translation on
   the measured threads after their first pass. A title's yields are mixed
   with work; these loops are yields and nothing else.

What would be needed for the whole thing: the app's `libe.so` as it is
built, run under Android's linker. The nearest reachable step is next item
1: build this same library with Termux's clang in the container the
primitives already use. `__ANDROID__` is then defined for real, so items 2,
3 and 4 above fall away (bionic's linker, the real slot, no `core/compat`),
at the price of whatever the Android-only branches then ask for
(`android/log.h` is there; `libandroid.so` for shared memory is not and
would need the memfd stand-in).

### 3. The translator (`bench_xlat.cc`)

Six programs, written for the rig with `core/ppc_asm.h` (a small assembler
of its own; no title's code): integer arithmetic with a data-dependent
branch; double-precision floating point (fused multiply-add, add, multiply,
subtract, absolute value, negate); AltiVec single-precision vectors (load,
multiply-add, add, maximum, subtract, store); calls (a loop that calls a
function with a stack frame that calls a leaf twice); a memory copy (a word
loop with indexed loads and stores and a byte loop with update forms); and
calls out of translated code into a host function that does nothing (the
guest-to-host thunk). Each is translated twelve times at different
addresses (translation time), run at several lengths and checked against a
C++ model of the same loop, then timed. Settings: defaults;
`pin_guest_registers`; `simplification_skip_unchanged_blocks`;
`a64_thunk_fpcr_compare`; `a64_lean_extern_thunk`; all four.

### Robustness against other branches

The rig reads settings by name from the core's registry at run time: a
branch that lacks a setting reports "NOT MEASURED: this tree has no setting
called ..." for that row and measures the rest. The translator counters are
used only if a configure-time check finds them under the names the rig
knows. If a branch changes the core so that the stand-in no longer links,
the primitives still build and report and the job says which part failed
(only this branch's own job fails on that).

## First results

Run 43550785355 (commit `817ceab`, 2026-11-14 03:16 UTC), three jobs, all
green. Runner: Arm Neoverse N2 (`implementer 0x41 part 0xd49`, Azure's
Cobalt 100), 4 cores, kernel 7.0 (Azure), glibc 2.39, LSE atomics present,
the CPU's instruction counter available to the benchmark job. Lowest of the
samples; nanoseconds unless said.

### 1. Host primitives (glibc, the plain build in the bionic job)

| Primitive | ns a call |
|---|---:|
| an empty call and return (subtract from the rest) | 0.59 |
| thread-local read, TLS descriptor, dynamic resolver (`dlopen`) | 2.43 |
| thread-local write, the same | 2.46 |
| thread-local read, TLS descriptor, static resolver | 2.06 |
| thread-local read, initial-exec | 0.59 |
| `pthread_getspecific` | 1.92 |
| fixed slot (`mrs tpidr_el0; ldr [..., #16]`) | 0.59 |
| a load through a pointer already in a register | 0.30 |
| `clock_gettime(CLOCK_MONOTONIC)` | 30.9 |
| `std::chrono::steady_clock::now()` | 31.7 |
| the counter register (`mrs cntvct_el0`) | 13.4 |
| `pthread_mutex_lock` + `unlock`, uncontended | 15.5 |
| `std::mutex` / `std::recursive_mutex`, the same | 15.2 / 15.3 |
| 16-bit compare-and-swap, acquire: outline / inline LSE / inline LL/SC | 8.38 / 8.09 / 13.2 |
| relaxed 64-bit add: outline / inline LSE / inline LL/SC | 4.39 / 4.18 / 4.71 |
| `jump_fcontext`, one switch | 7.69 |

The same program in the Termux container, in that run, was built the way
Termux builds by default, which turned out to be with emulated
thread-locals (pthread keys behind `__emutls_get_address`), not the ELF TLS
the app uses: 4.1 to 4.4 ns for every thread-local read, whichever way the
library was loaded. Those rows are not the app's path. The job now builds
with `-fno-emulated-tls` first; see "The bionic job, second attempt" below
for what that gave. The rows that do not involve thread-locals were the same
as glibc's to within 2 ns (bionic's mutex 17.1 ns against 15.5).

### 1b. `Fiber::SwitchTo`, one switch

| defaults | `host_thread_state_slot` | static TLS (not Android's) |
|---:|---:|---:|
| 14.0 | 10.3 | 10.5 |

### 2. The scheduler (`claude/integration-1113` sources), ns a pass and cores busy

| Benchmark | defaults | lean yield | park spinners | slot | skew 2000 us | lean + slot | all | static TLS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| the loop alone | 1.77 | 1.77 | 1.77 | 1.77 | 1.77 | 1.77 | 1.77 | 1.77 |
| a kernel call that does nothing | 19.0 | 18.9 | 18.9 | 19.0 | 19.0 | 19.0 | 19.0 | 19.0 |
| `NtYieldExecution`, nothing else ready | 270 | 58.6 | 1,799 | 243 | 293 | 51.4 | 1,677 | 227 |
| ... cores busy | 1.00 | 1.00 | 0.042 | 1.00 | 1.00 | 1.00 | 0.042 | 1.00 |
| zero delay, nothing else ready | 63.8 | 64.1 | 1,952 | 60.1 | 66.9 | 59.9 | 1,920 | 56.5 |
| ... cores busy | 1.00 | 1.00 | 0.042 | 1.00 | 1.00 | 1.00 | 0.043 | 1.00 |
| yield, another fiber ready (per yield) | 275 | 275 | 273 | 246 | 297 | 249 | 273 | 229 |
| event round trip, one guest CPU | 110,966 | 110,634 | 110,997 | 110,751 | 110,979 | 110,597 | 110,989 | 110,759 |
| ... cores busy | 0.071 | 0.070 | 0.071 | 0.071 | 0.073 | 0.070 | 0.071 | 0.069 |
| event round trip, two guest CPUs | 15,631 | 15,828 | 15,395 | 15,365 | 15,672 | 15,440 | 15,529 | 15,408 |
| ... cores busy | 0.843 | 0.851 | 0.841 | 0.850 | 0.840 | 0.841 | 0.842 | 0.838 |

### 3. The translator (defaults)

| Program | ns per guest instruction | host instructions run per guest instruction | host instructions emitted per guest instruction | translate, us |
|---|---:|---:|---:|---:|
| integer | 0.559 | 5.52 | 9.21 | 31.6 |
| float | 0.966 | 7.78 | 17.9 | 46.5 |
| vector | 0.897 | 10.2 | 32.8 | 50.7 |
| calls | 0.256 | 3.96 | 10.1 | 60.1 |
| copy | 0.636 | 6.27 | 10.1 | 28.8 |
| host calls | 2.32 | 19.3 | 15.7 | 36.2 |

With the settings: `pin_guest_registers` changed no number (no register was
pinned in any of the 108 functions it considered, 72 of them with loops; see
Findings 3). `simplification_skip_unchanged_blocks` skipped 1,116 of 3,804
block walks with no mismatch and the translated code is identical; the
translation times moved by under 2%, which is inside the noise for functions
this small. `a64_thunk_fpcr_compare`: the host-calls loop runs 19.67 host
instructions per guest instruction instead of 19.33 (two more a call) and
takes 2.27 ns instead of 2.32 (about 0.3 ns a call less on this CPU).
`a64_lean_extern_thunk` changed no number (Findings 4).

### How much two runs of the same code differ on this runner

This branch's own job runs the same programs as "target" and "baseline".
Of about 330 numbers, the ones that differed by more than 3%: one
translation time (10%), the `calls` loop's time per guest instruction (6.5%,
it sits at either 0.256 or 0.273 ns from process to process) and the `copy`
loop's (17%, 0.544 or 0.636; the buffers were a multiple of 4 KiB apart,
fixed since). Every scheduler number agreed to within 3%, most within 1%.
Host instruction counts are exact and repeat to every digit: where a time
and a count disagree, believe the count.

### `claude/guest-waits` (at `fc5767e`) against `claude/integration-1113`

The same run, its own job. Everything that moved by more than 3%:

| Benchmark, setting | guest-waits | baseline | change |
|---|---:|---:|---:|
| `NtYieldExecution`, nothing ready, lean yield | 44.0 | 58.5 | -24.8% |
| the same, lean yield + slot | 41.5 | 51.3 | -19.1% |
| zero delay, nothing ready, lean yield | 54.8 | 64.0 | -14.5% |
| the same, lean yield + slot | 50.9 | 59.9 | -15.1% |
| `NtYieldExecution`, nothing ready, park spinners (cores busy 0.042 both) | 1,379 | 1,800 | -23.4% |
| zero delay, nothing ready, park spinners (cores busy 0.043 both) | 1,685 | 1,941 | -13.2% |
| zero delay, nothing ready, slot | 57.1 | 60.1 | -5.0% |
| `NtYieldExecution`, nothing ready, slot | 235 | 243 | -3.4% |
| yield, another fiber ready, slot | 238 | 246 | -3.3% |
| zero delay, nothing ready, defaults | 66.7 | 64.0 | +4.2% |
| zero delay, nothing ready, skew 2000 us | 70.1 | 66.8 | +4.8% |
| `NtYieldExecution` / zero delay / yield to another, static TLS | 237 / 61.4 / 238 | 227 / 58.0 / 228 | +4.4% / +5.9% / +4.2% |
| `Fiber::SwitchTo`, slot | 11.9 | 10.3 | +15.4% |

Unchanged: the empty `NtYieldExecution` at defaults (270 both), both event
round trips, the kernel call that does nothing, and the translator (its
`calls` and `copy` rows are in the list of the run but are the two-speed
loops described above; their instruction counts are identical).

So, on this runner: guest-waits makes the lean empty yield a quarter
cheaper and the lean zero delay a seventh; costs about 3 ns on the zero
delay with every new setting off; and makes `Fiber::SwitchTo` with the slot
on 1.6 ns dearer. Whether any of that is visible on the phone is for the
phone.

## Findings for the other sessions

Measured on the runner, with the code as it is on `claude/integration-1113`
unless said. Where a sentence is my reading of the source and not a
measurement it says so.

1. **An event handed between two threads on the same guest CPU takes 111
   microseconds a round trip, with the CPU 93% idle.** (`kernel/**`,
   `claude/guest-waits`.) Two threads on guest CPU 0, each `NtSetEvent` on
   the other's auto-reset event then `NtWaitForSingleObjectEx` on its own.
   One round trip is two sets and two waits. On two guest CPUs the same
   round trip takes 15.6 microseconds (0.84 cores busy); a yield that
   switches to another fiber on the same CPU takes 0.275 microseconds. So
   on one CPU each hand-over is waiting about 55 microseconds for something
   that is not CPU work. No setting changes it and guest-waits does not
   either. See "The one-CPU hand-over" below for what the system calls
   show.
2. **Thread-local access is a small part of an empty yield here.** The
   empty `NtYieldExecution` round trip is 270 ns; with every thread-local
   at a fixed offset (static TLS) it is 227 ns (-16%); with
   `host_thread_state_slot` 243 ns (-10%); with the lean yield 58.6 ns
   (-78%). A dynamic TLS descriptor read costs 1.8 ns more than a fixed
   slot on this CPU with glibc. The phone's profile has the resolver at
   8.2% of the thread; that is consistent with a cheap call made tens of
   millions of times a second (my arithmetic: 8.2% of a second over 2.4 ns
   is 34 million reads), not with a dear call.
3. **`pin_guest_registers` pins nothing in a tight loop, because every
   conditional branch is charged as a full write-back and reload.**
   (`cpu/compiler/passes/guest_register_pinning_pass.cc`,
   `claude/translator-wide`.) Measured: 0 of 108 functions pinned, 72 with
   loops. With temporary prints in a scratch copy of the pass (not
   committed), the `integer` loop (10 guest instructions, three blocks):
   the best candidate, r6, has benefit 34 and cost 38; in the `float` loop
   f1 has benefit 25 and cost 41. The cost comes from `sync_weight`: each
   `OPCODE_CONTEXT_BARRIER` inside the loop counts 8, and the frontend
   emits one before every instruction of type `kSync`, which includes the
   conditional branches (`ppc_hir_builder.cc`, "Synchronize the PPC context
   as required"). My reading: after context promotion a register is loaded
   and stored about once a block, so a loop's benefit is about 16 a block
   and its cost about 16 a branch plus the exits; blocks and branches come
   in equal numbers, so the net is at best about 16 - 1 - the exit weight,
   and it only clears the threshold of 8 when the function's return is
   outside the loop's block range and the register is touched in every
   block. That would explain 17 functions of 24,576 in Fable II. Whether a
   barrier really forces the pinned registers out and back (or only needs
   them written) is the question that decides how much can be pinned.
4. **`a64_lean_extern_thunk` does not change a call through an import
   thunk.** (`cpu/backend/a64/a64_emitter.cc`, `claude/translator-wide`.)
   The `host_calls` loop calls a host function through a thunk written as
   `cpu/xex_module.cc` writes a title's imports (`sc 2; blr`, `SetupExtern`).
   It runs 19.33 host instructions per guest instruction with the setting
   off and on (the count is exact); the lean thunk would save dozens of
   instructions a call. My reading: the condition at the call site
   (`instr && !instr->next`, "the last instruction of its block") is not
   met for the `sc 2` of an import thunk. Worth one look at what follows
   the `CALL_EXTERN` in that block.
5. **The clock.** `clock_gettime` costs 31 ns and the counter register
   13 ns here, against 15 ns for an uncontended mutex and 8 ns for a fiber
   switch. `GuestScheduler::NowMicros` is `steady_clock::now()` (32 ns).
   On this CPU a clock read is the dearest primitive in the yield path.
6. **Outline atomics cost the same as inline LSE here** (8.38 against
   8.09 ns for the compare-and-swap, 4.39 against 4.18 for the add): the
   helper's call and its check are nearly free on this CPU. Building the
   core for `armv8.1-a` would not buy much per operation; the profile's
   5% and 3% are counts.

## What was and was not measured

Measured, on a GitHub ARM64 runner (Neoverse N2), with real numbers in a
green run: the host primitives named in the brief (thread-local read by
dynamic TLS descriptor, static descriptor, initial-exec, pthread key, fixed
slot, held register; `clock_gettime`, `steady_clock`, the counter register;
uncontended mutexes; the outline atomics against inline LSE and LL/SC; the
fcontext switch and `Fiber::SwitchTo`); the scheduler's real paths from the
project's own sources (an empty `NtYieldExecution`, a zero delay, a yield
with another fiber ready, a wait and wake on one and on two dispatch
threads) with `guest_scheduler_lean_yield`,
`guest_scheduler_park_yield_spinners`, `host_thread_state_slot` and
`guest_scheduler_skew_bound_us` off and on; six synthetic PowerPC loops
through the translator with `pin_guest_registers`,
`simplification_skip_unchanged_blocks`, `a64_thunk_fpcr_compare` and
`a64_lean_extern_thunk` off and on, in host instructions and nanoseconds per
guest instruction; and `claude/guest-waits` against
`claude/integration-1113`.

Not measured: anything on the phone or on a phone's CPU; frame rate;
anything under heat; the scheduler under Android's linker (the core
benchmarks run on glibc, with the stand-ins listed under "What differs from
the app"); `kernel_state.cc` (stood in for); a title's real mix of calls;
register pinning's effect (it never applied to the loops); the GPU, audio,
memory watches and file access; `claude/translator-wide`,
`claude/shader-first-run`, `claude/defaults-config` and
`claude/memory-watches` (the first did not exist when the rig last ran; the
rig has no benchmark for what the others change). No speed-up on the phone
is claimed for anything.

## Files

- `.github/workflows/boosted-bench.yml`: the workflow (new; the two
  existing workflow files are untouched).
- `boosted/bench/CMakeLists.txt`, `common/` (timing, result lines, the
  instruction counter), `prim/` (primitives, `build-plain.sh` for the
  container), `core/` (the core library's build, `kernel_shim.cc`,
  `bench_env.*`, `ppc_asm.h`, `bench_sched.cc`, `bench_xlat.cc`,
  `core_main.cc`, `launcher.c`, `compat/`, `run-core.sh` with the settings
  matrix), `tools/` (`report.py`, `prepare-tree.sh`, `ci-build.sh`,
  `submodules.txt`), `run.sh`, `branches.txt`.
- No upstream file is changed. The app's build does not see
  `boosted/bench`.

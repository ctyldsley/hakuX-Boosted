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

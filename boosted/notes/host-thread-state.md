# host-thread-state: the current-thread lookups off the dynamic TLS resolver

Branch `claude/host-thread-state`, cut from
`claude/xbox360-emulator-architecture-eac8l7` at `5f8a0e9` (the base
`7648e9c` plus the two notes-only commits after it; no code differs from
`7648e9c`).

## Done / in progress / next

- Done: the handover's "step 0", written, behind `host_thread_state_slot`
  (default off, so a merge changes nothing that runs). A checking mode,
  `host_thread_state_verify`, for the first device run. The arm64 code of
  the readers read by eye (compiled here with the host's clang 18 for
  `aarch64`, not with the NDK): see "Why a read cannot be hoisted".
- Built green on Actions: run 58, commit `a1808c8` (see "Builds").
- In progress: nothing.
- Next (not mine to do, see "Left for the desktop session"): the
  scheduler's `t_current_cpu`, `base/mutex.cc`'s cached thread id, then
  the measurement on the phone.

Nothing here has run on a device. No speed-up is claimed: the profile
says where the time went, the phone has to say whether it comes back.

## What it is

`libe.so` is loaded with `dlopen`, so every read of a `thread_local` in it
is a call to the linker's `tlsdesc_resolver_dynamic` (bionic does not
allow the initial-exec model in a `dlopen`'ed library). In the run 43
profile that resolver was 2.4% of the six dispatch threads and 4.9% of
GameThread; earlier runs showed up to 9% of the busiest thread.

Five of those `thread_local`s are the "who am I" values that every kernel
call, wait and fiber switch asks for:

| Value | `thread_local` (kept as the fallback) | Readers and writers |
|---|---|---|
| current guest thread | `current_xthread_tls_`, `kernel/xthread.cc` | `XThread::GetCurrentThread`, `IsInThread(XThread*)`, `GetCurrentFiberThread`, `SetCurrentThread` |
| current CPU thread | `cpu::Thread::current_thread_`, `cpu/thread.cc` | `cpu::Thread::GetCurrentThread`, `IsInThread`, new `SetCurrentThreadPointer` |
| bound PPC context | `thread_state_`, `cpu/thread_state.cc` | `cpu::ThreadState::Get`, `GetThreadID`, `Bind` |
| current fiber | `current_fiber_`, `base/threading_fiber.cc` | `Fiber::GetCurrentFiber`, `SwitchTo`, `CreateFromThread` |
| logging thread id | `current_thread_id_`, `base/threading.cc` | `current_thread_id`, `set_current_thread_id` |

"The current fiber thread" (`XThread::GetCurrentFiberThread`) is the
current guest thread when it has a fiber, so it needs no field of its own.

With the setting on, a host thread keeps those five in one
`xe::threading::HostThreadState` (`base/host_thread_state.h`), and the
address of that struct sits in bionic's `TLS_SLOT_APP`. A read is then
`mrs x8, tpidr_el0; ldr x8, [x8, #16]; ldr x0, [x8, #field]` plus the test
of the mode byte, with no call into the linker.

### The slot, with its source

bionic, `libc/platform/bionic/tls_defines.h` (read from the AOSP mirror on
GitHub, `aosp-mirror/platform_bionic`, branch `master`, on 2026-11-13;
`android.googlesource.com` is not reachable from the cloud session):

- "TLS_SLOT_APP: Available for use by apps in API level 29 and later.
  (This slot was used for errno in API level 28 and earlier.)"
- in the `__arm__ || __aarch64__` branch:
  `#define TLS_SLOT_APP 2 // was historically used for errno`, between
  `TLS_SLOT_THREAD_ID 1` and `TLS_SLOT_OPENGL 3`.

`libc/platform/bionic/tls.h`: `__get_tls()` on aarch64 is
`mrs %0, tpidr_el0`, returned as `void**`. So slot 2 is the 8-byte word at
`tpidr_el0 + 16`. The app's minimum API level is 29; the code checks
`android_get_device_api_level() >= 29` as well before it uses the slot.

### Where the struct lives

In a `thread_local` of the library itself (`thread_state_storage_` in
`base/host_thread_state.cc`). The slot only caches its address, so the
linker is asked once per host thread instead of on every read. This was
chosen over `malloc` plus a pthread key destructor because it has the same
lifetime as the variables it replaces and needs nothing done at thread
exit (see "Thread exit").

## Why a read cannot be hoisted across a fiber switch

The run 22 crash: with a plain `thread_local`, the compiler works out the
thread's TLS address once per function and reuses it, so a function that
reads before and after a fiber switch (which can resume on another host
thread) reads the first host thread's copy the second time. The fix then
was to make the readers `XE_NOINLINE`.

Both protections are in place here, independently:

1. **Every reader that was `XE_NOINLINE` still is.** None was removed or
   made inline. A caller gets the value from a fresh call each time.
2. **The slot itself is read by an `asm volatile`** containing both
   instructions (`mrs` and the `ldr` of the slot), in `ReadSlot()` in
   `base/host_thread_state.h`. A volatile asm is treated by the compiler as
   having side effects it cannot see: two of them are never merged, one is
   never moved above a call or out of a loop, and one whose result is
   unused is still emitted. So even if a reader were made inline one day,
   the slot would still be read again after the switch.

Checked in the generated code, not only argued: a test function that calls
`HostThreadStateForRead()`, then an opaque function, then
`HostThreadStateForRead()` again, compiled at `-O3` for aarch64, keeps two
`mrs`/`ldr` pairs, one each side of the call, and a loop keeps the pair
inside the loop. In the same build `FcontextFiber::SwitchTo` shows the
compiler hoisting `mrs x21, TPIDR_EL0` to the top of the function for the
plain `thread_local` path: the hazard is real for `thread_local`, and
absent for the asm. (Host clang 18 with `--target=aarch64-linux-gnu`, not
the NDK's clang; the semantics of `asm volatile` are the same.)

The rule that remains for anyone writing new code: do not keep a
`HostThreadState*`, or a value read from it, in a local across anything
that can switch fibers (a wait, a yield, a guest call). Ask again. This is
the same rule the `XE_NOINLINE` readers already impose.

## The life of a state

- **The mode is fixed once**, by the first write of any of the five values
  on any thread (`DecideAndAcquire`), from the setting as it is then.
  Until that moment every thread's `thread_local`s still hold their
  initial values, so reads before it give the same answer either way.
  After it the mode never changes: a thread never has some values in one
  place and some in the other. The first writes happen when the guest
  scheduler's dispatch threads start or a guest thread's host thread
  starts, long after the config is loaded.
- **Thread creation.** Nothing is done at creation. A host thread gets its
  state on its first write (`Register`): for a dispatch thread that is
  `Fiber::CreateFromThread`, for a guest thread with its own host thread it
  is `set_current_thread_id` at the top of the thread's start routine, for
  any other thread it is whatever it writes first (`ThreadState::Bind` in
  `cpu/function.cc`, for instance).
- **Fiber creation from a thread** (`Fiber::CreateFromThread`): writes the
  current fiber, which registers the calling host thread if it is not yet.
- **A fiber moving between host threads.** The scheduler's dispatcher calls
  `XThread::SetCurrentThread(next)` on its own host thread before it
  switches to the fiber and `SetCurrentThread(nullptr)` after the fiber
  comes back, as before. Those writes now land in the state of the host
  thread that makes them. The fiber carries nothing: whatever it reads
  after resuming, it reads from the slot of the host thread it is now on.
- **Host threads that never registered** (GPU, audio, pipeline compilers,
  the UI): the slot is null, readers get a shared read-only state of
  initial values (`unregistered`: null pointers, thread id "the host
  thread's own"), which is what their untouched `thread_local`s would say.
  They pay no linker call either.
- **Thread exit.** Nothing to free. bionic's `pthread_exit`
  (`libc/bionic/pthread_exit.cpp`) runs, in order, the C++ `thread_local`
  destructors, the pthread cleanup handlers (where
  `XThread::OnHostThreadExitCleanup` clears the current thread, as
  before), the pthread key destructors, and only then, with signals
  blocked, frees the thread's dynamic TLS. The state is valid for all code
  of ours that can still run on the thread. The slot is left pointing at
  freed memory for the last instructions of a thread that runs nothing of
  ours any more.
- **Signal handlers** (the fault handlers call `GetCurrentThread`): the
  read is an `mrs` and loads, with no lock and no allocation.
- **Somebody else using the slot.** If the slot is not null on the thread
  that makes the first write, the setting is ignored for the whole run
  (`xe.log` says so) and the `thread_local`s are used. After that, a
  non-null slot is taken to be ours; there is no cheap way to tell. No
  source in this repository touches `tpidr_el0` or the slot; FEX
  (`external/fex`, a submodule not checked out in the cloud session) was
  not read, so check it before trusting this on a build where FEX and the
  360 core run in one process on the same threads.

## How to switch it on, and what `xe.log` says

Both settings are in the `CPU` group and are read once, at the first use
after the title starts:

```toml
host_thread_state_slot = true      # default false
host_thread_state_verify = false   # default false; see below
```

In the title's `config/<TITLE ID>.config.toml` (or the global config).
They are not in the app's settings screen (`SettingsSchema.kt` belongs to
another session).

Lines to look for:

- `HostThreadState: per-thread state through TLS_SLOT_APP (host_thread_state_slot is on)`:
  the slot path is in use. With verify: `... (host_thread_state_slot is on, verifying against the thread_local variables)`.
- `HostThreadState: thread_local variables (...)` with the reason: the
  setting is off, the device is below API 29, or the slot was taken.
- `HostThreadState: host thread <tid> registered (<n> so far)`: one per
  host thread that wrote a value. Expect the six `Guest CPU` threads and
  any guest thread that has its own host thread.
- `HostThreadState mismatch #<n>: ...` (verify only): the state and the
  `thread_local` disagree. The first 32 are logged, then every 4096th.
  There must be none.

## What a device run must check first

In this order, with a title that reaches gameplay (Fable II):

1. **Setting off (the default): nothing changed.** `xe.log` has
   `HostThreadState: thread_local variables (host_thread_state_slot is off)`
   and the run is as run 44 was. This is the merge check.
2. **Boot with `host_thread_state_slot = true` and
   `host_thread_state_verify = true`.** Verify keeps both copies and
   compares on every read, so it is slower than either path; it is there to
   prove the slot path, not to be measured. Check: the "is on, verifying"
   line; six or more "registered" lines; the title boots to the menu.
3. **Fiber migration, same settings.** Play into the first gameplay scene
   for a few minutes (370,000 fiber switches a second at the median, and
   threads that are not pinned move between dispatch threads). Check: no
   `HostThreadState mismatch` line at all, no
   "Attempting to use guest stuff from a non-guest thread" assert, no null
   current thread in `XObject::Wait` (the run 22 symptom). A mismatch line
   names the value and both pointers: send it back with the thread dump.
4. **Thread exit, same settings.** Titles create and end threads through
   loads: play through a load and quit the title from the menu. Check: no
   mismatch, no crash at exit.
5. **Then `verify` off, slot on**, and measure.

## How to measure it on the phone

The number is the share of `tlsdesc_resolver_dynamic` in a simpleperf
profile of the `Guest CPU` threads, same scene, setting off and then on.
Before: 2.4% of the six threads, 4.9% of GameThread (run 43,
`device-logs/run43-profile/perf-guest.data`, 1 kHz, 200 s). Take it the
way run 43 was taken; as a sketch (written from simpleperf's options, not
run from here):

```bash
PID=$(adb shell pidof <package>)
```

```bash
TIDS=$(adb shell "for t in /proc/$PID/task/*; do grep -q '^Guest CPU' \$t/comm && basename \$t; done" | paste -sd,)
```

```bash
adb shell simpleperf record --app <package> -t $TIDS -f 1000 --duration 200 -o perf-guest.data
```

```bash
adb shell simpleperf report --app <package> -i perf-guest.data --sort comm,dso,symbol | grep -E "tlsdesc_resolver|Overhead"
```

What to expect, honestly: the share should fall but not to zero, because
`thread_local`s remain on the hot paths that this branch was not allowed
to touch (next section), the scheduler's `t_current_cpu` first among them.
To see who still calls the resolver, record with `--call-graph fp` and
report with `--children` or `-g caller` on that symbol. Keep the change
only if the share and the frame time (`device-watch.py fps`) both move.

The slot path's own cost per read is the mode byte (`adrp`, `ldrb`,
compare), the `mrs`, two loads, a null test and the verify-flag test. The
readers still have a call and a small frame around that, because they are
`XE_NOINLINE` and still contain the fallback.

## Left for the desktop session (files I do not own)

1. **`kernel/guest_scheduler.cc`, `t_current_cpu`** (16 uses, read on
   every yield through `CurrentCpu()`). Not touched, as instructed.
   `HostThreadState` already has the field `int32_t scheduler_cpu = -1`
   for it. The change, when that file is free:

   ```cpp
   #include "xenia/base/host_thread_state.h"

   XE_NOINLINE static int CurrentCpu() {
     if (const auto* state = xe::threading::HostThreadStateForRead()) {
       return state->scheduler_cpu;
     }
     return t_current_cpu;
   }

   // and where RunLoop does `t_current_cpu = index;`:
   if (auto* state = xe::threading::HostThreadStateForWrite()) {
     state->scheduler_cpu = index;
   } else {
     t_current_cpu = index;
   }
   ```

   (Add the verify comparison as the other readers have it if wanted; the
   pattern is in `cpu/thread_state.cc`.) Every direct use of
   `t_current_cpu` outside `CurrentCpu()` has to go through one of the two.
2. **`base/mutex.cc`, `gettid()`**: caches the thread id in a function
   `static thread_local`, read on every lock of that mutex type. bionic's
   own `gettid()` reads the id from the thread's `pthread_internal_t`
   without the resolver, so on Android the cache can simply go:
   `return ::gettid();`.
3. **Others seen, by likely heat:** `alertable_state_` and the
   `t_wait_any_*` counters in `base/threading_posix.cc` (host waits; in
   my row but left alone: they are not "who am I" values and the wait path
   belongs to the scheduler work); `thread_local_string_buffer` in
   `kernel/util/shim_utils.cc` (export logging); `thread_log_buffer_` in
   `base/logging.cc`; the three `thread_local`s in
   `cpu/backend/a64/a64_seq_memory.cc` (the CPU translation session's).
   A profile with callers will rank them.
4. **`threading::Thread::GetCurrentThread`** (`current_thread_` in
   `base/threading_posix.cc`, the `PosixThread`): left on its
   `thread_local`. It is not on a hot path (four callers), and it is
   written at the start of every host thread, possibly before the config
   is read, which the "decide at the first write" rule would not survive.
5. **Once the phone agrees**, the readers' fallback could move out of line
   so that the common path needs no frame, and the hottest callers could
   use an inline reader. Not done: it would trade away the `XE_NOINLINE`
   layer, and that decision needs a device.

## Upstream files changed

All under `x360/emulator-core/src/main/cpp/xenia/src/xenia/`, each change
marked with a `hakuX-Boosted:` comment.

| File | Change | Why |
|---|---|---|
| `base/host_thread_state.h` (new) | `HostThreadState`, `HostThreadStateForRead/ForWrite`, the slot read, the verify helpers | the design |
| `base/host_thread_state.cc` (new) | the two settings, the mode decision, per-thread registration, the mismatch report | the design |
| `base/threading_fiber.cc` | `current_fiber_` read and written through `LoadCurrentFiber`/`StoreCurrentFiber` | the current fiber |
| `base/threading.cc` | `current_thread_id`, `set_current_thread_id` use the state when it is on | the logging thread id |
| `cpu/thread.h`, `cpu/thread.cc` | readers use the state; new protected `SetCurrentThreadPointer` replaces direct writes of `current_thread_` | the current CPU thread |
| `cpu/thread_state.cc` | `Bind`, `Get`, `GetThreadID` and the destructor go through `Load/StoreBoundThreadState` | the bound PPC context |
| `kernel/xthread.cc` | every use of `current_xthread_tls_` and `current_thread_` goes through `Load/StoreCurrentXThread` and `SetCurrentThreadPointer` | the current guest thread |

**Two files outside the letter of my row in `OVERNIGHT.md`**, for the
desktop session to accept or drop at the merge:

- `base/threading_fiber.cc`: the row says `base/fiber*`, which matches no
  file; this is the only fiber file in `base`, so I took it to be meant.
- `base/threading.cc`: the row lists `base/threading.h` and
  `base/threading_posix.cc` but not this file, where
  `current_thread_id`/`set_current_thread_id` are defined. The brief names
  the thread id as one of the values to move, no other session's row has
  the file, and the change is 15 lines in two functions that already carry
  a `hakuX-Boosted:` comment. If it is unwanted, revert that one file: the
  rest works without it (the id then stays on its `thread_local`, read
  through the resolver once per log line and written twice per fiber
  switch).

`base/threading.h`, `base/threading_posix.cc` and `kernel/xthread.h` are
in my row and were not changed. `kernel/guest_scheduler.*` was not touched.

## What the build cannot verify

- Anything about behaviour: that the slot is free on this phone's Android
  17, that a fiber migration reads the right state, that thread exit is
  clean, that the resolver's share falls. Those are the device checks
  above.
- The arm64 instruction sequences quoted here came from the host's clang
  18, not from the NDK build.
- With the setting off the only added work in each reader is one byte
  load and a compare before the old code. Not measured.

## Builds

- **Green: run 58, commit `a1808c8`** (all the code),
  https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43393536677.
  The APK to test is this run's artifact. The commit after it changes
  only this file.
- Runs 52, 55 and 57 (`795ac55`, `5a76ac1`, `a8df349`) were cancelled by
  the pushes that followed them, not failed.

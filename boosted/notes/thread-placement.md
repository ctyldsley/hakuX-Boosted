# Thread placement: what the others do, and what was built here

Branch `claude/thread-placement`, from `7648e9c` (built green as run 44).
Session of 2026-11-12 to 13. Nothing in this file was measured on a device:
the code was built by GitHub Actions and exercised on Linux x86-64 only
(section 7).

## Status

Done:
- The code: `base/host_perf.h`, `base/host_perf.cc`, and one call in
  `base/frame_stats.h`. Three settings, all off by default (section 1).
- Local checks of that code (section 7).
- The survey's reading: all four research passes finished and their source
  clones are cited by commit.

In progress:
- Writing the survey into sections 3 and 4 and the ranked list into
  section 5 (this commit has the code and how to use it; the survey text
  follows in the next).

Next (for whoever picks this up):
- Run the four runs of section 1 on the phone and fill in section 5's
  "measured" column.
- At merge: nothing to apply by hand. Section 8 lists what the desktop
  session may want to change in files this branch could not touch.

## 1. What to switch on, and what to compare

All settings are cvars in the `[Kernel]` table of the config file (the
global one, or the title's `<storage_root>/config/4D5307F1.config.toml`).
None is in the settings screen. With all of them off (the default) the only
code that runs is three loads and a return per presented frame.

| Setting | Default | What |
|---|---|---|
| `host_perf_log` | false | A line a second in `xe.log` (below) |
| `host_perf_hint` | false | An ADPF performance hint session over the six `Guest CPU` threads and `GPU Commands` |
| `host_perf_hint_target_fps` | 0 | The frame rate the session aims for; 0 works it out from the title's presents. Set 30 for Fable II if the "hint frame" figure in the log is not 33.3 |
| `host_perf_hint_report` | 0 | 0: reports the busiest session thread's CPU time against 0.9 of the frame time, and the whole interval when the frame was late. 1: always the present-to-present interval, against the frame time |
| `host_perf_core_policy` | 0 | 0 none; 1 two sets of cores; 2 one core per dispatch thread (section 2) |
| `host_perf_helper_cores` | 2 | Cores kept for the compile threads under a policy |
| `host_perf_helper_threads` | `Vulkan Pipeline,Shader Translat` | Which threads count as helpers, by the start of their names |

The lines, all starting `host perf`:

```
host perf: 8 cores, capacity 0:... 7:..., top MHz 0:... ; allowed cpu 0-7 (cpuset /top-app); performance hint available; thermal status available, headroom available
host perf: 93 threads in the process; G0 tid 1234 nice -8 cpu 0-7; ... GPU Commands tid 1240 nice -8 cpu 0-7; 4 helper(s) [tid 1250 nice 8 cpu 0-7] ...
host perf: core policy 1 asked, 1 in force: guest threads on cpu 2-7, helpers on cpu 0-1
host perf: 11 thread(s) given their cores, 0 let go
host perf: performance hint session over 7 thread(s) (6 dispatch, GPU Commands in), preferred update rate 16.6 ms
host perf 1000 ms | MHz now/limit 0:1440/3628 ... | thermal 2 moderate headroom 0.87 | run/wait ms, slices, @core: G0 880.1/41.2 1210 @7, G1 25.0/60.3 4100 @3, ... GPU 610.0/33.1 900 @4, helpers(4) 3100.0/800.2 | frames 30 longest 41.2 ms | hint frame 33.3 ms target 30.0 ms reports 30 mean 28.1 ms late 2
```

(The numbers above are made up to show the shape. `G0` to `G5` are the
dispatch threads of guest CPU 0 to 5. `run/wait` is time on a core and time
runnable but waiting for one, from `schedstat`, since the last line;
`slices` is how many times the thread was put on a core; `@core` the core it
was last on. `MHz now/limit` is `scaling_cur_freq` over `scaling_max_freq`:
a limit below the "top MHz" of the first line is the phone capping that
core. `helpers(4)` is the compile threads together.)

One change per run, same scene, same starting temperature if possible
(the phone was at 1.44 of 3.63 GHz and 39 C in run 43, so heat decides a
lot). Suggested runs, each a cold shader cache to the first gameplay scene
of Fable II and two minutes on:

1. **A, the baseline of run 44**: `host_perf_log = true` only.
   - The `threads in the process` line shows whether `7648e9c` took: `G0`
     to `G5` and `GPU Commands` at nice -8, the helpers at nice 8. If they
     read 0 the phone refused the nice values (then say so in the handover;
     nothing else in this list means much).
   - `G1` (the Fable II worker's thread): `wait` against `run`. Run 43 had
     wait 2.4 times run over 200 s. Note the worst second, and whether the
     bad seconds are the ones where `helpers` `run` is large.
   - `MHz now/limit` against the top MHz, and the thermal status: is the
     phone capping the cores during play, and which cluster first?
2. **B, sets of cores**: A plus `host_perf_core_policy = 1`.
   - The same `wait` figures for `G0` to `G5` and `GPU`, in seconds where
     `helpers` are busy. They should fall if queueing behind compile
     threads is what is left after the nice values.
   - The cost: `helpers` `wait` rises (four compile threads on two cores),
     so pipelines take longer on a cold cache. Compare the pipeline cache's
     own totals and what the owner sees pop in. If the cost offends, try
     `host_perf_helper_cores = 3`.
   - `given their cores` lines after the first: each is the system having
     wiped the masks (the app went to the background and back).
3. **C, one core each**: A plus `host_perf_core_policy = 2`.
   - As B. Also `@core` should now be fixed per thread, and the
     `one core each:` line shows who got the prime cores (re-ranked every
     30 s by time on a core).
   - If the phone reports severe throttling the log says the policy fell
     back to sets; that is by design, note how soon it happened.
4. **D, the hint session**: A plus `host_perf_hint = true`.
   - `performance hint session over 7 thread(s)` must appear; if instead
     `host_perf_hint is on but does nothing` or `gave no performance hint
     session`, the phone has no support and the run is run A.
   - `MHz now` against run A at the same thermal status: does the phone
     clock the cores higher with the session?
   - `hint frame` should read 33.3 ms for Fable II (16.7 with the 60 FPS
     patch); `late` per second against run A's count of frames over 36 ms
     (`longest`), and the owner's eyes and ears.
   - Headroom over the run against run A: a session that only heats the
     phone sooner is a loss.
   - Then `host_perf_hint_report = 1` as run E if D shows any effect.

For each run the scheduler's own lines stay the yardstick: the worker's
`late wake` lines, `waited ... for critical section`, and whether the title
stalls.

## 2. What was built

### Files

- `x360/emulator-core/src/main/cpp/xenia/src/xenia/base/host_perf.h`, new:
  one function, `xe::host_perf::OnGuestFramePresented()`.
- `.../base/host_perf.cc`, new: everything else.
- `.../base/frame_stats.h`, upstream (XenDroid's), changed: an include and
  one call at the top of `xe::RecordGuestPresent()`, marked
  `hakuX-Boosted:`.

The ownership table names the new files `base/host_perf_android.*`. They
are `base/host_perf.*` instead, because this build never compiles a
`*_android.cc` in `base/`: `xe_platform_sources`
(`xenia/cmake/XeniaHelpers.cmake`) drops every file with a platform suffix
and, for Android, adds back only `*_posix.*`. The few `_android.cc` files
that are built are listed by hand in
`x360/emulator-core/src/main/cpp/CMakeLists.txt`. A file with the suggested
name would have built green and done nothing. Named without a suffix it is
picked up by the glob and no build file changes. The body is under
`#if defined(__linux__)`; `XE_PLATFORM_ANDROID` is not defined in this fork
(`base/platform.h` defines `XE_PLATFORM_xendroid` and `XE_PLATFORM_LINUX`).

### The hook

`xe::RecordGuestPresent()` is already called once per presented guest frame
by `VulkanCommandProcessor` (`gpu/vulkan/vulkan_command_processor.cc`, its
only caller, on the `GPU Commands` thread). The new call sits inside it, so
no file owned by another session changes and there is nothing for the
desktop session to move. Should a second presentation path ever appear that
does not go through `RecordGuestPresent`, it needs the same one call.

The first call with any setting on starts a worker thread, `Host Perf`
(nice 0, no affinity of its own). Nothing is started at boot, so the log
begins with the first presented frame, not with the title's launch.

### How the threads are found

Once a second the worker reads `/proc/self/task/*/comm`:

- `Guest CPU 0` to `Guest CPU 5`: the dispatch threads.
- `GPU Commands`: the GPU command thread.
- Names starting `Vulkan Pipeline` or `Shader Translat`: helpers. The
  kernel keeps 15 characters of a name and `PosixThread::set_name` cuts to
  that, so `Vulkan Pipelines` is `Vulkan Pipeline` in `comm`.

A new thread carries its creator's name until it is renamed, so the four
pipeline threads are `GPU Commands` for a moment and anything a guest
thread starts is `Guest CPU n`. Of two threads with a wanted name the one
that started first (field 22 of its `stat`) is taken. A thread that is
created again is found on the next tick; masks and the session follow.

### (a) The performance hint session

- `libandroid.so` is opened with `dlopen` and the functions looked up by
  name, so the app still loads on API 29. No session below API 33; the log
  says why.
- Created with `APerformanceHint_createSessionUsingConfig` where the phone
  has it (API 36), otherwise `APerformanceHint_createSession`. The reason:
  in Android 16's source the older call returns a pointer it never set
  when the service refuses (section 4).
- Threads: the six dispatch threads and `GPU Commands`, updated with
  `APerformanceHint_setThreads` (API 34) or by making the session again.
- Target: the title's frame time. With `host_perf_hint_target_fps = 0` it
  is the quicker quarter of the last 64 present-to-present intervals,
  rounded down to a whole number of 60 Hz refreshes (one to four), taken
  up when two estimates in a row agree; 33.3 ms until then. Rounded down
  so that a title held below its pace keeps its real target: 45 ms frames
  still read as a 30 FPS title. A title that sits at exactly 20 FPS for
  seconds will be read as a 20 FPS title; set the rate by hand then.
- Reported once per presented frame, from the presenting thread, with
  `APerformanceHint_reportActualWorkDuration`:
  - mode 0: the CPU time of the busiest thread in the session since the
    last present (per-thread CPU clocks), against a target of 0.9 of the
    frame time; when the frame was more than 10% late, the whole interval
    instead. Reasoning: a guest that waits for the vertical blank shows
    the same present interval whether its threads were 50% or 99% busy,
    and only the second needs more speed.
  - mode 1: always the interval, against the frame time itself.
  - Caveat for both: a guest thread that spins (the profile's
    `sub_82B9BF90`, 52.7% of guest CPU 3) is "busy" to the kernel. In
    mode 0 it will keep the session asking for more. That is one more
    reason to park that wait, and why mode 1 exists.
- The presenting thread only ever tries the lock it shares with the
  worker; a frame's report is skipped rather than waited for.
- `APerformanceHint_reportActualWorkDuration2` is deliberately not used
  (section 4: a no-op for apps the system tags as games in the Android 16
  source read).

### (b) Core placement

Each core's capacity is `/sys/devices/system/cpu/cpuN/cpu_capacity`; where
that cannot be read, `cpufreq/cpuinfo_max_freq` scaled to 1024; where
neither, all equal. The cores the process may use are the worker's own
affinity mask (its cpuset less what is offline), read every tick.

- **Policy 1, two sets.** The helpers get every small core (less than half
  the biggest's capacity) and never fewer than `host_perf_helper_cores`,
  taken lowest capacity first and, of equals, lowest numbered. The six
  dispatch threads and `GPU Commands` get the rest. On the SM8850, if
  cpu0-5 are the performance cores and cpu6-7 the prime ones (not checked:
  the first log line says), that is helpers on cpu0-1, guest threads on
  cpu2-7. A thread may move within its set.
- **Policy 2, one core each.** As policy 1 for the helpers. Each dispatch
  thread gets one core of the guest set to itself; the busiest threads
  (time on a core over the last 30 s) get the cores worth most at present
  (capacity scaled by `scaling_max_freq`, so a throttled prime core stops
  counting as the best). Threads on cores of equal worth are never
  swapped. `GPU Commands` is left unrestricted: with six of eight cores
  given out it would otherwise share the helpers' two.
- Threads that are neither (audio, the frame limiter, the VFS and I/O
  threads, the app's own) are never touched.
- Masks are checked every tick and put back if they differ.

**The risk of hard masks, and how the policy lets go.** A mask is a promise
the kernel keeps even when it hurts:

- *Thermal core limits.* A vendor's thermal engine may cap a cluster's
  frequency, take cores offline or isolate them. A thread pinned to a
  capped prime core runs slower than it would have on a performance core
  the kernel was free to choose. If every core in a mask goes offline the
  kernel breaks the mask itself. So: the allowed set and the frequency
  limits are re-read every tick; the sets are rebuilt when the allowed set
  changes; policy 2 ranks by limited capacity; policy 2 falls back to
  policy 1 when Android reports thermal status severe or worse (and stays
  there until the status is light or none) or when fewer than six guest
  cores remain; any policy lets go entirely (all masks back to every core)
  when fewer than four cores would be left for the guest's threads.
- *The cpuset moving.* When the app leaves the foreground Android moves
  it to another cpuset and the kernel rewrites every thread's mask
  (section 4). The worker sees the allowed set shrink, usually below the
  four-core floor, and lets go; back in the foreground it applies the
  masks again. Each re-application is logged.
- *Fewer cores for compiling.* Under either policy four compile threads
  share `host_perf_helper_cores` cores. On a cold cache that is slower
  compiling, so longer pop-in, in exchange for the guest's threads not
  queueing. That trade is the thing to measure.
- *Policy 2 and light threads.* A dispatch thread that mostly sleeps
  (guest CPU 1 ran 6.3 s in 200 s) keeps a core to itself that others
  could have used. On the console that is how it is; here it costs the
  GPU command thread and the helpers room.
- *A refused mask.* `sched_setaffinity` failing is logged once per thread
  and the thread is left alone.
- With every setting off again the masks are cleared and the session
  closed, without a restart.

Google's own advice is against manual affinity (section 4). This is why
the policy is off by default and why policy 1 (sets, not pins) is the
first to try.

### (c) The log line

Section 1 shows it. Thermal status is read every second, headroom every
ten (Android's guidance; more often may return NaN) and shown as last
read. Frequencies need `cpufreq/scaling_cur_freq` to be readable by an
app, which is the vendor's choice: a `?` in the line means it is not.

## 6. Upstream files changed

| File | Change | Why |
|---|---|---|
| `xenia/base/frame_stats.h` | `#include "xenia/base/host_perf.h"` and a call to `host_perf::OnGuestFramePresented()` at the top of `RecordGuestPresent()` | The one place that already runs once per presented guest frame and that no other session owns |

New files (not upstream's): `xenia/base/host_perf.h`,
`xenia/base/host_perf.cc`, this file. `boosted/docs/ARCHITECTURE.md` and
the settings table in `boosted/HANDOFF.md` need the row above and the seven
settings of section 1 added at merge; this branch does not touch them.

## 7. What was and was not verified

Verified here, on Linux x86-64 (four cores, no cpufreq), with a harness
that includes `host_perf.cc` itself, stand-ins for `cvar.h` and
`logging.h`, threads named as the emulator names them, and a stand-in
`libandroid.so` that prints each call:

- Compiles with clang 18 `-Wall -Wextra -Werror` and gcc 13.
- 31 checks of the pure parts: the placement for a 2+6, a 1+3+4 and a
  four-core layout; three helper cores; a background cpuset; a prime core
  offline; severe throttling; the busiest threads getting the prime cores;
  no swaps among equal cores; throttled prime cores going to the idlest
  threads; the frame time estimate at 16.9, 33.3, 45 and 50 ms, hitching
  and uncapped; the `stat` parser; the per-thread CPU clock.
- Live: the six dispatch threads, `GPU Commands` and four helpers found by
  name; an unrenamed child of `GPU Commands` correctly left out; masks
  applied (policy 1, with the four-core floor lowered for the test) and
  read back from the kernel; masks released and the session closed when
  the settings were turned off, and all of it back when turned on; the
  session created through both the API 36 and the older call; the target
  set to 30.0 ms (0.9 of 33.3) from a 30 FPS present cadence; one report
  per frame; `schedstat` deltas that add up to the load the test threads
  were given; the path with no `libandroid.so` at all.
- gcc's thread sanitizer: no report on the frame lock or the session. It
  does report the worker reading the settings while the test writes them;
  the settings are plain globals read without a lock everywhere in Xenia.

Not verified, because nothing here can run on a device:

- Everything about behaviour on the phone: whether the vendor's power HAL
  does anything with a hint session, whether masks reduce queueing,
  whether either changes the frame rate, the stall, the heat or the sound.
- Whether an app on this phone may read `scaling_cur_freq` and
  `scaling_max_freq`, and whether `cpu_capacity` exists there.
- Which cores are the prime ones (assumed cpu6-7 in the examples only; the
  code reads capacities and assumes nothing).
- The real `libandroid.so`: the prototypes are copied from the NDK headers
  (section 4) but no call has been made on Android.
- How the vendor's thermal engine limits cores on this phone (caps,
  offlining or isolation), so which of the fallbacks above it will meet.
- That the Actions build is green: see the last section of this file.

## 8. For the desktop session at merge

To come with the survey: the changes this branch would have made in files
it does not own, in order of expected worth.

## Build

To be filled in from the Actions run of this branch.

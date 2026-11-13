# Thread placement: what the others do, and what was built here

Branch `claude/thread-placement`, from `7648e9c` (built green as run 44).
Session of 2026-11-12 to 13. Nothing in this file was measured on a device:
the code was built by GitHub Actions and exercised on Linux x86-64 only
(section 7).

## Status

Done:
- The code: `base/host_perf.h`, `base/host_perf.cc`, and one call in
  `base/frame_stats.h`. Three settings, all off by default (sections 1
  and 2).
- Local checks of that code (section 7) and a green Actions build
  (section 9).
- The survey: eleven projects and this repository's own code, read from
  source (section 3, appendices A to C), and Android's own guidance
  (section 4, appendix D).
- The ranked list of what to adopt (section 5) and what the desktop
  session may want to change in files this branch could not touch
  (section 8).

In progress: nothing.

Next (for whoever picks this up):
- Run the runs of section 1 on the phone, one change at a time, and put
  what they show beside section 5's rows. Nothing here has been on a
  device.
- At merge: nothing to apply by hand for this branch to work. Add the
  rows of section 6 to `boosted/docs/ARCHITECTURE.md` and the settings of
  section 1 to `boosted/HANDOFF.md`.

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
host perf: other threads not at nice 0: Audio Worker -12, XMA Decoder -12, ...
host perf: core policy 1 asked, 1 in force: guest threads on cpu 2-7, helpers on cpu 0-1
host perf: 11 thread(s) given their cores, 0 let go, 0 that had inherited a mask freed
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
   - The `other threads not at nice 0` line: the audio threads should be
     at -12. Anything else at -8 is a thread a dispatch thread or the GPU
     command thread started, which took the value with it (a new thread
     inherits its creator's nice): decide whether each should have it.
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
   - XenDroid already keeps a hint session over the dispatch and audio
     threads with the audio deadline as its target (`apu_performance_hint`,
     default true; section 3). Run D is therefore two sessions over the
     same six threads. If D changes nothing, or to tell the two apart,
     repeat A and D with `apu_performance_hint = false` in `[APU]` and
     listen for the sound.
   - Then `host_perf_hint_report = 1` as run E if D shows any effect.
5. **F, fewer compile threads** (no code, section 5 item 2): A plus
   `vulkan_pipeline_creation_threads = 2` in `[Vulkan]`.
   - `helpers(2)` run and wait, the guest threads' wait in the seconds
     where the helpers are busy, and how long the cold cache takes to
     settle.

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
  threads, the app's own) are never given a mask.
- Masks are checked every tick and put back if they differ.
- A new thread takes its creator's mask with it. Whatever a placed thread
  starts (the I/O thread a guest thread starts, say) would otherwise be
  held to that mask for good, under policy 2 to a dispatch thread's own
  core. So every tick any other thread found with exactly a mask this
  code handed out is given every core back; a mask some other code chose
  is not touched. The log counts them ("that had inherited a mask
  freed"). RPCS3 and ARMSX2 clear the inherited mask in the new thread
  for the same reason (appendices A and B).

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

## 3. The survey, in brief

Every row was read from source in this session (shallow clones, commit and
date given), not recalled. The detail, with a file and function for each
claim and the search terms behind each "nothing found", is in appendices A
to C. A dozen of the claims this section leans on were opened again by
hand after the research passes reported (Eden's nice ladder and its
ADPF-before-affinity rule, ARMSX2's compile thread priorities, PPSSPP's
hint session, RPCS3's `set_native_priority`, Android's `setrlimit nice`,
Google's sentence on affinity, the `createSession` return path); none was
found wrong. Function names the passes could not confirm are flagged in
the appendices.

| Project (commit, date) | Guest CPUs to host threads | Priorities | Core placement | Compile threads | Frame pacing | Android performance APIs |
|---|---|---|---|---|---|---|
| **Eden**, Switch (`e9661b2`, 2026-11-12; GitHub mirror, `git.eden-emu.dev` refused by the proxy) | Four emulated cores are four host threads; guest threads are fibers on them. The same shape as this fork's guest scheduler | On Android, `setpriority` per thread: guest core threads, GPU command thread and Vulkan submit worker nice -16; vsync and timing -8; present, fencing and audio -4; compile and decode workers +10 (`src/common/thread.cpp:49-53`, `:478-486`) | A "performance" set (at least 50% of the fastest core's `cpu_capacity`, at least four cores), applied as one mask, never one core. Only when a thread cannot join the ADPF session (`thread.cpp:542-550`). A no-op where there are no small cores | Two on Android by default (2 to 8), nice +10, in an ADPF "background" session that prefers power efficiency, not pinned | Its own 60 Hz vsync thread and a present thread; a debounced frame rate hint to the window; no Swappy, no Choreographer | Two ADPF sessions (render, background), target per vsync, measured frame interval reported; game mode in the manifest; no thermal API. Nice and affinity re-applied on every resume |
| **ARMSX2**, PS2 (`37ed35b`, 2026-11-12) | EE, GS and VU threads | Compile workers nice 5, background recompiler nice 10 (`pcsx2/GS/GSCompileQueue.cpp:159-166`). A nice -1 for the emulation threads sits in a branch that does not appear to be built for Android | Emulation threads confined to the fast tier as a group, by `cpu_capacity` then `cpuinfo_max_freq`, never when the tier has fewer cores than threads. Per-core pinning rejected in a comment citing a measured 1.4 times slowdown (`pcsx2/VMManager.cpp:5345`). Compile workers clear the mask they inherit | `clamp(cores/2 - 1, 1, 4)` | Sleep-based limiter; `Surface.setFrameRate` | An ADPF session over EE, GS and MTVU, off by default |
| **PCSX2** upstream (`3ff6b1b`, 2026-11-12) | The same three threads | None, but for a network thread | One core each for EE, VU and GS by `cpuinfo` frequency, Linux and Windows only, off by default | Compiles on the GS thread | Limiter on the emulation thread, gives up lost time by whole frames | None |
| **PPSSPP** (`cc2aab3`, 2026-11-12) | One emulation thread, one render thread | None | None (no affinity code at all) | One scheduler thread feeding a pool of core-count workers; pipeline tasks ordered first | Limiter in the display HLE, bounded catch-up, `Surface.setFrameRate` | **ADPF on by default**: one session for the emulation thread, target half a 60 Hz frame, thread CPU time reported, functions found with `dlsym` (`android/jni/PerformanceHint.cpp:29-97`) |
| **Dolphin** (`db2afcb`, 2026-11-11) | A CPU thread and a GPU thread | None | None: `SetThreadAffinity` is compiled out on Android and has no callers | One by default; automatic is `clamp(cores - 3, 1, 4)`; the queue orders what is needed now ahead of speculative work | Limiter on the emulation thread, 100 ms catch-up bound | None |
| **RPCS3** (`9b8974d`, 2026-11-12) | A host thread per PPU and SPU thread, but its own LV2 scheduler lets only two PPU threads run at once, in guest priority order | A deliberate order: GPU (RSX) and audio threads +1, every compile worker and the log writer -1, guest threads untouched. Done with `pthread_setschedparam` in the current policy (`Utilities/Thread.cpp:3862-3893`), so in practice Windows only | "Thread Scheduler" defaults to the operating system. The other modes use a table of desktop Ryzen and Intel layouts; any ARM CPU gets all cores; affinity is compiled out on Android | Two in game on an eight-thread host, every core only during the boot-time cache preload, all at -1 | Its own timer thread to absolute deadlines | None. The discontinued Android fork added manual per-core classes, off by default |
| **Vita3K** (`c7d90f5`, 2026-11-12) | A host thread per guest thread | None | None | Three on an eight-thread host; deferred pipelines so a draw never waits | Emulator timer | None (an Adreno "turbo" switch through libadrenotools, off by default) |
| **xenia-canary** (`4f04bc7`, 2026-11-12) and **xenia** master (`95a5c3e`, 2026-02-18) | A host thread per guest thread | Canary maps guest priority to nice when `SCHED_FIFO` is refused (`nice = 16 - priority`) and emulates quantum decay and wake boost; master is `SCHED_FIFO` or nothing. In neither do the GPU command thread, the audio threads or the pipeline threads have a priority | Guest CPU n to host CPU n, off by default | Canary 75% of the cores (six of eight); master's Vulkan path compiles on the command thread | A vblank timer thread and `framerate_limit` | None |
| **FEX** (`ab135df`, 2026-11-11) | One pthread per guest thread | Passes the guest's own `setpriority`, `sched_setaffinity` and `sched_setscheduler` to the host. Sets nothing itself but nice 19 on its cache writer | None | The JIT compiles on the thread that runs the code | Not applicable | None |
| **Winlator** (Ludashi fork `94a4009`, 2026-11-11; the original publishes no current source) | Wine's threads | None | A CPU list per container chosen by the user, applied as a Windows affinity mask; the default for 32-bit programs is the upper half of the CPU numbers; no detection of core types | DXVK's own | DXVK frame rate limit; the fork adds Choreographer frame timelines | A one-thread ADPF session for its presenter (fork only) |

### XenDroid as it is in this repository (`7648e9c`)

Read here, not surveyed by a pass. Paths under
`x360/emulator-core/src/main/cpp/xenia/src/xenia/`.

- **Guest CPUs to host threads.** Six dispatch threads, `Guest CPU 0` to
  `Guest CPU 5`, one per hardware thread of the console; guest threads are
  fibers on them (`kernel/guest_scheduler.cc`, `EnsureStarted`).
- **Priorities.** `PosixThread::set_priority` tries `SCHED_FIFO`, is
  refused, and falls back to `nice = 16 - priority`
  (`base/threading_posix.cc:1421-1464`). Since `7648e9c`
  (`guest_threads_above_helpers`, default on, untested): dispatch threads
  and `GPU Commands` at kAboveNormal, nice -8
  (`kernel/guest_scheduler.cc:264-270`, `gpu/command_processor.cc:655-662`);
  pipeline threads at kBelowNormal, nice 8
  (`gpu/vulkan/vulkan_pipeline_cache.cc:386-392`). XenDroid's own: audio
  worker and XMA decoder at nice -12 by raw `setpriority`
  (`apu/audio_system.cc:211-222`, `apu/xma_decoder.cc:214`); the vblank
  thread `GPU Frame limiter` at kNormal, nice 0, raised from kLowest
  because it "was starved under load" (`gpu/graphics_system.cc:260-265`).
- **Core placement.** None in force. `XThread::SetActiveCpu` pins a guest
  thread to the host core of the same number only when the guest thread
  has a host thread of its own, which under the guest scheduler it does
  not (`kernel/xthread.cc:1089-1094`).
- **Compile threads.** The cvar's default is 75% of the cores
  (`vulkan_pipeline_cache.cc:371-380`); the app ships
  `vulkan_pipeline_creation_threads = 4`
  (`x360/emulator-core/src/main/assets/config/default_config.toml:337`).
- **Frame pacing.** The guest's vblank comes from the `GPU Frame limiter`
  thread (`gpu/graphics_system.cc:155-260`); `framerate_limit` throttles
  at the swap (`CommandProcessor::ThrottlePresentation`,
  `gpu/command_processor.cc:597`). The surface is told 60 Hz (50 in 50 Hz
  mode) with `Surface.setFrameRate(..., FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)`
  (`x360/app/src/main/java/xendroid/compose/EmulatorHostActivity.kt:515`).
  No Swappy, no Choreographer.
- **Android performance APIs.** One already, and it matters for run D:
  `apu_performance_hint` (APU, **default true**) makes an ADPF session
  over every thread whose name starts `Guest CPU`, plus `Audio Worker` and
  `XMA Decoder`, with a target of 5.333 ms (the audio pump interval), and
  reports how long each guest audio callback took
  (`apu/audio_system.cc:64-67`, `:127-204`, `:367`). So the dispatch
  threads are already in a hint session, one that is told its work takes a
  small part of its target nearly every time. `host_perf_hint` adds a
  second session over the same six threads with the frame as its target.
  The combined app's manifest declares `android:appCategory="game"`
  (`android/app/src/main/AndroidManifest.xml:28`). No thermal API anywhere
  before this branch.

### What stands out

1. **Nobody who is smooth on Android leaves the guest's threads and the
   compile threads at the same priority.** Eden and ARMSX2, the two
   projects with current Android-specific scheduling code, both do it with
   per-thread nice values through `setpriority`, which Android permits
   (section 4). RPCS3 has the same order on the desktop. This is the
   direct answer to "6.3 s on a core, 15.1 s waiting", and `7648e9c`
   already is it. Eden goes further than this fork does (-16 against -8
   for the guest's threads, +10 against +8 for compiling) and also raises
   its vsync thread.
2. **They run fewer compile threads than this fork while the guest
   runs.** Eden two, RPCS3 two, Dolphin one, Vita3K three, ARMSX2's rule
   gives three on eight cores. This fork runs four beside seven busy
   guest-side threads on eight cores. RPCS3 uses every core only for the
   cache preload at boot, when the guest is not running.
3. **Where they place threads at all, it is by set, not by core, and
   they prefer ADPF to masks.** Eden masks only if the hint session is
   unavailable; ARMSX2 measured a 1.4 times slowdown from pinning and
   says so in its source; upstream PCSX2's one-core-each is off by
   default; Dolphin, PPSSPP, Vita3K and RPCS3 on ARM do nothing. On a
   phone with no small cores (this one) Eden's and ARMSX2's sets are the
   whole machine, so their placement does nothing there. Nobody keeps
   compile threads off particular cores: they use nice for that.
4. **ADPF is in real use and is small.** PPSSPP ships it on by default
   with CPU time as the measure; Eden and ARMSX2 put several emulation
   threads in one session. All three look the functions up at run time.
5. **Nobody uses Swappy or Choreographer** to pace an emulator. All pace
   from their own timer thread to the guest's clock and tell Android the
   frame rate. XenDroid already does both.
6. **Nobody reads the thermal state.** With this phone at 1.44 of
   3.63 GHz during run 43, that is the largest thing no surveyed project
   helps with, and the first thing `host_perf_log` shows.

## 4. Android's own guidance, in brief

Appendix D has the detail, the exact prototypes and the sources (the
NDK headers, Android 16 `frameworks/base`, `system/core`, `bionic`,
`system/sepolicy`, the Android common kernel and kernel documentation,
read through GitHub mirrors because Google's own source hosts were not
reachable; and developer.android.com pages fetched on 2026-11-13). Android
17, which the phone runs, was not visible in any mirror.

- **Nice values.** An app may give its own threads any nice value down to
  -20 with raw `setpriority`: `init.rc` runs `setrlimit nice 40 40`, the
  kernel then needs no capability, SELinux allows `setsched` on self and
  the call is not blocked by seccomp. The framework's constants give the
  scale: display -4, urgent display -8, video -10, audio -16, urgent audio
  -19, background 10. Vendor hooks exist on this path and were not
  readable; hence the nice values in the first log line. Since Android 13
  a positive nice no longer moves a thread into a background group.
- **Real-time classes.** Not available: apps hold no capabilities.
  `SCHED_FIFO` will always be refused, as XenDroid's fallback assumes.
- **Affinity.** Allowed on the app's own threads; the kernel intersects
  the mask with the app's cpuset and fails with `EINVAL` if nothing is
  left. When the app's cpuset changes (foreground to background and back)
  the kernel rewrites every thread's mask: from kernel 6.2 it keeps what
  the thread asked for where it can, before that the thread's mask
  becomes the whole new cpuset. So a mask must be checked and put back,
  which `host_perf` does every second. Google's guidance: "it's generally
  best to avoid manually setting CPU affinities"
  (developer.android.com/agi/sys-trace/threads-scheduling).
- **uclamp.** An app cannot set its own utilisation clamps on the Android
  common kernel (`EPERM` without `CAP_SYS_NICE` on `android16-6.12`).
  ADPF is the supported way to get that boost: the Pixel reference HAL
  turns a session's reports into a per-thread uclamp minimum. What
  Qualcomm's and Xiaomi's HAL do is not in any source read.
- **ADPF hint sessions.** A session is a set of thread ids, a target
  duration and a report per cycle of work; the system "will attempt to
  adjust the core placement of the threads ... and/or the frequency of
  the core on which they are run". Points that shaped the code:
  - threads must be in the calling process and should be long-lived;
    a dead or foreign tid makes the service refuse;
  - on Android 16, `APerformanceHint_createSession` returns a pointer it
    never initialised when the service refuses, where
    `APerformanceHint_createSessionUsingConfig` (API 36) sets it to null
    first (checked by hand in `performance_hint.cpp`). `host_perf` uses
    the newer call where it exists. **XenDroid's audio session uses the
    older call** (section 8);
  - session calls are not thread safe (one lock in `host_perf`);
  - sessions only act while the app is in the foreground;
  - a report at or over target is sent at once, reports under target are
    batched by the library, so one call per frame is cheap;
  - the system sets `SCHED_RESET_ON_FORK` on session threads, so threads
    they start do not inherit a raised priority, and a later
    `sched_setscheduler` on them from the app is refused (`setpriority`
    still works);
  - for an app the system tags as a game, the Android 16 tree read makes
    `APerformanceHint_reportActualWorkDuration2` (the call with separate
    CPU and GPU durations) return without reporting. `host_perf` uses the
    single-duration call, which has no such test.
- **Thermal.** `AThermal_getCurrentThermalStatus` (API 30; none, light,
  moderate, severe, critical, emergency, shutdown) and
  `AThermal_getThermalHeadroom` (API 31; 1.0 is the point of severe
  throttling; the header says more often than about once a second is no
  use and may return NaN, Android's games guide says once in ten seconds;
  `host_perf` asks every ten). Listeners exist; `host_perf` polls instead, to stay off
  the binder callback threads.
- **Core capacities.** `/sys/devices/system/cpu/cpuN/cpu_capacity` (1024
  for the biggest), `cpufreq/cpuinfo_max_freq`, `scaling_cur_freq`,
  `scaling_max_freq`. The generic SELinux policy lets every app read the
  directory; whether a vendor relabels single files was not verifiable,
  so every read in `host_perf` is optional.
- **Frame pacing.** Swappy (the Android Frame Pacing library),
  `AChoreographer` and `ANativeWindow_setFrameRate` are what Google
  offers. Swappy paces an app's own swaps to the display; an emulator's
  pace is the guest's vblank, which is why none of the surveyed projects
  uses it. The frame rate hint is the part that applies, and XenDroid
  sends it.
- **Game Mode.** `android:appCategory="game"` is set. The platform's
  game mode interventions (downscaling, FPS override) can be refused in
  the manifest with `android:allowGameDownscaling="false"` and
  `android:allowGameFpsOverride="false"`; neither is set here. For the
  UI session, not this branch.

## 5. What this fork should adopt, in order

Ranked by how directly the evidence above answers what was measured, and
by how little can go wrong. "Built" means the code is on this branch or
already on the base; nothing in the last column is a result.

| # | What | Why, against the measurements | State | Measure with |
|---|---|---|---|---|
| 1 | **Finish the nice ladder and prove it took.** Guest dispatch threads and `GPU Commands` above everything but audio; the vblank thread above normal with them; compile threads well below | Run 43: guest CPU 1 on a core 6.3 s, waiting 15.1 s, with every thread at nice 0. Eden (-16/-8/-4/+10), ARMSX2 and RPCS3 all order threads this way and Android permits it. The vblank thread is the guest's clock and sits at nice 0 under threads at -8: Eden puts its vsync thread at -8 | Dispatch, GPU and compile done in `7648e9c`, untested. The vblank thread is `gpu/graphics_system.cc`, not this branch's (section 8) | Run A: the `nice` values in the thread lines, then `wait` against `run` for `G1` |
| 2 | **Fewer compile threads while the guest runs: two.** | Eleven or more runnable threads on eight cores. Eden and RPCS3 run two beside the guest, Dolphin one. Cost: Turnip takes a median 56 ms a pipeline, so a queue drains more slowly | Config only: `vulkan_pipeline_creation_threads = 2` in `[Vulkan]` | Run A against the same with two: `helpers` run and wait, the guest threads' wait in the same seconds, pipeline totals, the owner's eyes |
| 3 | **The per-second log, kept.** | Nothing surveyed watches frequency limits or thermal state, and on this phone they decide the result (1.44 of 3.63 GHz, 39 C). Every other item here is judged from this line | Built: `host_perf_log` | It is the instrument |
| 4 | **An ADPF session over the guest's threads with the frame as its target.** | The cores were far below their top frequency while the title missed its pace. PPSSPP ships this on by default; Eden prefers it to masks. Unknown: whether this phone's power HAL acts on it, and how it combines with the audio session already there | Built: `host_perf_hint` | Run D: `MHz now` and `late` against run A at the same thermal status; headroom over time |
| 5 | **Compile threads kept to their own cores (policy 1).** | Only if queueing remains after 1 and 2. Nobody surveyed does this (they rely on nice), Google advises against masks, and on this SoC it costs compile throughput. Its case is that nice changes who wins a core, not how long a waking thread waits for one | Built: `host_perf_core_policy = 1` | Run B |
| 6 | **One core per dispatch thread (policy 2).** | The console's own arrangement and what `HANDOFF.md` named as the next step, but against the evidence: ARMSX2 measured pinning as slower and upstream PCSX2 leaves it off. Kept as an experiment because Fable II's stall is a race the console's exclusive cores always win | Built: `host_perf_core_policy = 2` | Run C |
| 7 | **Compile on every core only while the guest is not running** (the stored pipelines at boot), two at run time | RPCS3 does exactly this. The fork's `pipeline_storage_precreate` is the boot phase | Not built; `gpu/vulkan/vulkan_pipeline_cache.cc` is another session's | After 2 |

Not worth adopting, with reasons: Swappy or Choreographer pacing (the
guest's vblank is the pace; nobody surveyed uses them); `SCHED_FIFO`
(refused for apps); setting uclamp directly (refused on the Android
common kernel); RPCS3's affinity tables (desktop CPUs only); thermal
listeners (polling once a second is enough and simpler).

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
  were given; the path with no `libandroid.so` at all; a thread started
  by a placed dispatch thread, which began with its creator's mask and had
  every core back a second later.
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
- Whether two hint sessions over the same threads (this one and the
  audio one) add up, and how.

## 8. For the desktop session at merge

Nothing below is needed for this branch to work. These are the changes
this branch would have made in files it does not own, in order of expected
worth. None was built.

1. **The vblank thread with the guest's threads.**
   `gpu/graphics_system.cc:264` sets `GPU Frame limiter` to
   `ThreadPriority::kNormal` (nice 0). With `guest_threads_above_helpers`
   on it is now below the threads it is the clock for. Suggested:
   `kAboveNormal` when the setting is on, as `gpu/command_processor.cc`
   does for itself. Eden has its vsync thread at -8 beside guest threads
   at -16. To see whether it matters first: `host_perf_helper_threads`
   only selects threads for masks, so use
   `cat /proc/<pid>/task/*/schedstat` for that thread, or add its name to
   the log (below).
2. **XenDroid's audio hint session** (`apu/audio_system.cc:127-204`,
   upstream's code, `apu_performance_hint` default true):
   - it calls `APerformanceHint_createSession`, which on Android 16
     returns an uninitialised pointer when the service refuses (section
     4). `CreateHintSession` in `base/host_perf.cc` is the pattern: use
     `APerformanceHint_createSessionUsingConfig` where `dlsym` finds it;
   - it takes every thread whose name starts `Guest CPU`, so it also
     takes threads a dispatch thread started that were never renamed;
   - it reports a guest audio callback's duration against a 5.333 ms
     target for six dispatch threads that do far more than audio. If
     `host_perf_hint` is kept, the audio session should probably hold
     only `Audio Worker`, `XMA Decoder` and the dispatch thread the mixer
     lives on, or go. Run D with and without it decides.
3. **Compile threads by phase** (`gpu/vulkan/vulkan_pipeline_cache.cc`,
   the pixel stand-in session's file): every core for
   `pipeline_storage_precreate` at boot, two once the title runs
   (section 5 items 2 and 7). Try the config-only half first.
4. **Nice values a new thread inherits.** Since `7648e9c` anything a
   dispatch thread or `GPU Commands` starts begins at nice -8. The pipeline
   threads are then set to 8; others keep -8. The
   `other threads not at nice 0` line lists them. If one should not have
   it (`Guest I/O`, say), reset it where the thread starts
   (`kernel/guest_scheduler.cc`).
5. **If `host_perf` is kept**, two small things outside its files:
   - start it at launch, not at the first presented frame, so a title
     that never presents is still logged: export a `Start()` from
     `base/host_perf.h` and call it at the end of
     `Emulator::CompleteLaunch` (`emulator.cc`, after `on_launch`);
   - the scheduler's once-a-second counters and this line could share a
     timestamp if the "late wake" totals were printed from the same tick.
6. **A wider log, if wanted.** The line follows the six dispatch threads,
   `GPU Commands` and the helpers. The audio threads and the vblank thread
   are the obvious next ones; they need a few lines in
   `Worker::SampleScheduling` and `Worker::LogLine` (this branch's own
   file, left out to keep the line readable).

## 9. Build

- `eeaa3b7` (the code, first version): Actions run 56, green. Both jobs:
  "Xbox core (hakuX)" and "APK (both cores)", the latter compiling the 360
  core with the NDK's clang and warnings as errors and linking `libe.so`.
  The link is the proof that `host_perf.cc` is in the build: the call in
  `frame_stats.h` would not resolve otherwise.
- The commit after it (inherited masks freed, the `other threads` line,
  this file completed): BUILD_PENDING

## Appendix A: RPCS3, Vita3K, Xenia (upstream)

As written by the research pass that read the source, unedited but for heading levels. Paths are relative to each repository at the commit named. "(session scratch)" was the session's working directory, which is not kept: the citations name the repository, commit and path, which is enough to find each one.

### Host thread priority, core placement and frame pacing: RPCS3, Vita3K, Xenia

Source survey. Every claim below was read from a shallow clone made for this survey and is cited as `path:line` with the enclosing function. Anything not read in source is labelled as such. Line numbers refer to the commits named at the top of each section.

Reading guide for our case (six guest-CPU host threads, one GPU command thread, four Vulkan pipeline compile threads, Snapdragon 2 prime + 6 performance cores, everything at nice 0): the questions are who gets a higher or lower host priority, who is kept away from whom, and what the emulator does while it waits.

---

#### 1. RPCS3

- Repository: https://github.com/RPCS3/rpcs3
- Commit read: `9b8974d32e73c897a5fcd6bdf69413ca7e436b01`, 2026-11-12 (committer date 2026-11-12T08:51:01+03:00)
- Android, what was reachable:
  - https://github.com/RPCS3-Android/rpcs3-android at `e2f87a7f14890ac6ba332343b3b811ca1f14e2d1`, 2025-04-09. Its `README.md:5-9` says the project is discontinued and was merged into RPCSX. It is a Kotlin shell plus a submodule (`.gitmodules`: `app/src/main/cpp/rpcs3` pointing at `DHrpcs3/rpcs3`).
  - The pinned core commit https://github.com/DHrpcs3/rpcs3 `1ede37d5d486c73c1bb78aaec50f779c99fe5784`, 2025-03-29, was fetched and read. Cited below as "Android fork".
  - https://github.com/RPCSX/rpcsx-ui-android at `1e588cbe04f7d9335787da6a2c4ecb505f7031cf`, 2026-08-29, UI only. Its core submodule (`RPCSX/rpcsx`) was not cloned: not checked.

##### Guest CPUs/threads to host threads

- One host thread per guest PPU thread. `_sys_ppu_thread_create`, `rpcs3/Emu/Cell/lv2/sys_ppu_thread.cpp:535-545`:
  ```cpp
  const u32 tid = idm::import<named_thread<ppu_thread>>([&]()
  ...
  return stx::make_shared<named_thread<ppu_thread>>(p, ppu_name, prio, 1 - static_cast<int>(flags & 3));
  ```
- One host thread per guest SPU thread: `rpcs3/Emu/Cell/lv2/sys_spu.cpp:829`, `idm::make_ptr<named_thread<spu_thread>>(group.get(), spu_num, thread_name, tid, false, option)` (enclosing function name not checked).
- The host threads are not left to run freely. RPCS3 runs its own LV2 scheduler on top of them and lets only "PPU Threads" of them execute guest code at once. The setting is `rpcs3/Emu/system_config.h:18`:
  ```cpp
  cfg::_int<1, 8> ppu_threads{ this, "PPU Threads", 2 }; // Amount of PPU threads running simultaneously (must be 2)
  ```
  This models the two hardware threads of the PS3 PPU. The analogue for us is six.
- Every new PPU thread starts suspended and waits for the scheduler: `ppu_thread::ppu_thread`, `rpcs3/Emu/Cell/PPUThread.cpp:2864-2865` (`// Trigger the scheduler`, `state += cpu_flag::suspend;`).
- The run queue is a singly linked list `g_ppu` kept in guest priority order with FIFO order inside a priority. `lv2_obj::awake_unlocked`, `rpcs3/Emu/Cell/lv2/lv2.cpp:1771-1776`:
  ```cpp
  // Use priority, also preserve FIFO order
  if (!next || (push_first ? next->prio.load().prio >= ...->prio.load().prio : next->prio.load().prio > ...))
  ```
- Threads past the first `ppu_threads` entries of that list are told to stop. `lv2_obj::awake_unlocked`, `lv2.cpp:1825-1832`:
  ```cpp
  for (usz thread_count = g_cfg.core.ppu_threads; target; target = target->next_ppu, i++)
  {
      if (i >= thread_count && cpu_flag::suspend - target->state)
      {
          ...
          ensure(!target->state.test_and_set(cpu_flag::suspend));
  ```
  The first `ppu_threads` entries are woken by `lv2_obj::schedule_all`, `lv2.cpp:1904-1916`.
- A guest yield rotates the caller to the back of its own priority band and only counts as a context switch if that moves it past the running window: `lv2_obj::awake_unlocked`, `lv2.cpp:1727-1737`.
- Guest priority drives only this queue. `sys_ppu_thread_set_priority`, `rpcs3/Emu/Cell/lv2/sys_ppu_thread.cpp:331-364`, calls `lv2_obj::set_priority` and nothing else. There is no mapping from guest priority to a host priority (see the complete list of host priority call sites in the next sub-heading).
- When a thread wakes a higher priority thread and a "hardware thread" is free, the waker is delayed on purpose so that the woken thread gets ahead, as on the console. `lv2_obj::awake_unlocked`, `lv2.cpp:1858-1880` adds `hw_sleep_time` (100 us on non-Windows, line 1872), and `cpu_thread::check_state`, `rpcs3/Emu/CPU/CPUThread.cpp:1111-1116` sleeps it with `thread_ctrl::wait_for_accurate(usec)`.

##### Priorities and nice values

- One primitive with three levels. `thread_ctrl::set_native_priority`, `Utilities/Thread.cpp:3862-3893`:
  ```cpp
  if (priority > 0)
      native_priority = THREAD_PRIORITY_ABOVE_NORMAL;
  if (priority < 0)
      native_priority = THREAD_PRIORITY_BELOW_NORMAL;
  ```
  on Windows (`SetThreadPriority`), and elsewhere:
  ```cpp
  pthread_getschedparam(pthread_self(), &policy, &param);
  if (priority > 0)
      param.sched_priority = sched_get_priority_max(policy);
  if (priority < 0)
      param.sched_priority = sched_get_priority_min(policy);
  if (int err = pthread_setschedparam(pthread_self(), policy, &param))
  ```
  It keeps the current policy. Under the default Linux/Android policy (`SCHED_OTHER`) the minimum and maximum static priority are both 0, so this call changes nothing there and RPCS3's priorities are effectively a Windows (and macOS) feature. That last sentence is from the Linux `sched(7)` manual, not from RPCS3 source: from memory, not verified in this session.
- It is always used through an RAII guard that returns to 0 on scope exit: `thread_ctrl::scoped_priority`, `Utilities/Thread.h:349-364`.
- Complete list of call sites (search terms: `set_native_priority`, `scoped_priority`, whole tree without `3rdparty`):
  - Raised (+1): the RSX (GPU) thread, `rsx::thread::on_task`, `rpcs3/Emu/RSX/RSXThread.cpp:866-867` (`// Raise priority above other threads`); the audio mixer, `cell_audio_thread::operator()`, `rpcs3/Emu/Cell/Modules/cellAudio.cpp:891`; the RSX audio data thread, `rsxaudio_data_thread::operator()`, `rpcs3/Emu/Cell/lv2/sys_rsxaudio.cpp:787`.
  - Lowered (-1): Vulkan pipeline compilers, `vk::pipe_compiler::operator()`, `rpcs3/Emu/RSX/VK/VKPipelineCompiler.cpp:37`; OpenGL pipeline compilers, `rpcs3/Emu/RSX/GL/GLPipelineCompiler.cpp:38`; PPU LLVM module workers, `ppu_precompile`, `rpcs3/Emu/Cell/PPUThread.cpp:4422` and `:4647`, and `ppu_initialize` (`thread_op::operator()`), `PPUThread.cpp:5979`; SPU cache workers, `spu_cache::initialize`, `rpcs3/Emu/Cell/SPUCommonRecompiler.cpp:980`; the log writer, `logs::file_writer::file_writer`, `rpcs3/util/logs.cpp:527`; a Vulkan device enumeration thread in the Qt UI, `rpcs3/rpcs3qt/render_creator.cpp:33`.
  - PPU and SPU threads: no call. They run at the default priority.
- So the policy is: GPU thread and audio above the guest CPUs, every compile and housekeeping thread below them, guest CPUs in the middle. This is exactly the ordering our measurement says we lack.
- macOS: every RPCS3 thread is created with a QoS class and a real-time policy attribute. `thread_base::start`, `Utilities/Thread.cpp:2793-2804`:
  ```cpp
  sp.sched_priority=99;
  ...
  pthread_attr_set_qos_class_np(&attrs, QOS_CLASS_USER_INTERACTIVE, 0);
  pthread_attr_setschedpolicy(&attrs, SCHED_RR);
  ```
  This is the only heterogeneous-core measure in the tree: on Apple Silicon the QoS class is what the OS uses to choose performance cores (that consequence is from memory, not verified).
- `setpriority` and `nice` are not used: search for `setpriority` outside `3rdparty` returned nothing.
- Linux only: timer slack is set to 1 ns so that sleeps are exact. `run_rpcs3`, `rpcs3/rpcs3.cpp:575-576` (and again at `:1044-1046`):
  ```cpp
  // Set timerslack value for Linux. The default value is 50,000ns. Change this to just 1 since we value precise timers.
  prctl(PR_SET_TIMERSLACK, 1, 0, 0, 0);
  ```
- Linux only, opt-in: Feral GameMode. `rpcs3/Emu/system_config.h:397` (`"Enable GameMode"`, default false), `enable_gamemode`, `rpcs3/gamemode_control.cpp:14-27`, requested at boot in `rpcs3/Emu/System.cpp:2884-2886`.

##### Core placement

- The "Thread Scheduler" setting is `thread_scheduler_mode { os, old, alt }`, `rpcs3/Emu/system_config_types.h:322-327`, default `os`, `rpcs3/Emu/system_config.h:25`. UI names: "Operating System", "RPCS3 Scheduler", "RPCS3 Alternative Scheduler", `rpcs3/Emu/system_config_types.cpp:642-644`.
- `os`: nothing is pinned. Every affinity call is guarded by `g_cfg.core.thread_scheduler != thread_scheduler_mode::os`.
- `old` and `alt`: each thread class gets a mask.
  - PPU and SPU threads: `cpu_thread::operator()`, `rpcs3/Emu/CPU/CPUThread.cpp:658-661`:
    ```cpp
    if (g_cfg.core.thread_scheduler != thread_scheduler_mode::os)
    {
        thread_ctrl::set_thread_affinity_mask(thread_ctrl::get_affinity_mask(get_class()));
    }
    ```
  - RSX thread: `rsx::thread::on_task`, `rpcs3/Emu/RSX/RSXThread.cpp:869-872`. RSX offload thread: `dma_manager::offload_thread::operator()`, `rpcs3/Emu/RSX/RSXOffload.cpp:36-39`.
  - Everything else (compile workers, audio) is class `general` and gets all cores. Because Linux threads inherit the creator's mask, new threads reset it: `thread_base::initialize`, `Utilities/Thread.cpp:2843-2848`, comment: "A new thread inherits its creator's affinity mask (e.g. compile workers spawned by a pinned PPU thread): reset it to the process mask". Worth copying if we ever pin: a pipeline thread created from a pinned thread would silently inherit the pin.
- How the layout is detected: by CPU brand string only. `thread_ctrl::detect_cpu_layout`, `Utilities/Thread.cpp:3594-3651`: "Ryzen" gives `amd_ccx`; "Intel" on Windows with an SMT core gives `intel_ht`; on other systems Intel logs `"Thread scheduler is not implemented for Intel and this OS"` (line 3648); everything else stays `generic`.
- The table, `thread_ctrl::get_affinity_mask`, `Utilities/Thread.cpp:3653-3860`:
  - `generic` returns all cores (`:3664-3667`). This is the path any ARM CPU takes.
  - `amd_ccx` with 8 threads or fewer does nothing (`:3670-3674`, "Single CCX or not enough threads, do nothing").
  - Otherwise it is keyed by family, model and logical thread count, with separate PPU, SPU and RSX masks. Example, Zen 2 with 16 threads (`:3709-3714`):
    ```cpp
    ppu_mask = 0b0000000011110000;
    spu_mask = 0b1111111100000000;
    rsx_mask = 0b0000000000001111;
    ```
    The purpose stated in the comments is to keep classes inside one CCX (shared L3) and apart from each other ("Much improved inter-CCX latency", `:3691`).
  - `old` and `alt` differ only in a few rows: Zen/Zen+ 16 threads (`:3754-3768`), Zen 3 16 threads (`:3795-3807`) and 12 threads (`:3809-3821`). For `intel_ht`, `alt` with 12 or more threads uses every second logical CPU, "Potentially improves performance by mimicking HT off" (`:3850-3855`); `old` does nothing on Intel.
  - UI tooltip: "Changing the thread scheduler is not supported on CPUs with less than 12 threads." `rpcs3/rpcs3qt/tooltips.h:94`.
- How the mask is applied, `thread_ctrl::set_thread_affinity_mask`, `Utilities/Thread.cpp:3919-3967`: `SetThreadAffinityMask` on Windows, `thread_policy_set(THREAD_AFFINITY_POLICY)` on macOS ("Supports only one core"), `pthread_setaffinity_np` on Linux and BSD. Android is compiled out explicitly (`:3934`):
  ```cpp
  #elif !defined(ANDROID) && (defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__))
  ```
  so on an Android build of upstream the function body is empty, and `thread_ctrl::get_thread_affinity_mask` (`:3969-4020`) falls to its `return -1` branch.
- ARM and heterogeneous cores in upstream: nothing. No big/little detection of any kind. Search terms with no hit outside `3rdparty`: `cpu_capacity`, `cpuinfo_max_freq`, `sched_setaffinity`.
- Android fork (`DHrpcs3/rpcs3` at `1ede37d5`): it does not detect core types either, it asks the user. `thread_ctrl::get_affinity_mask`, `Utilities/Thread.cpp:2943-2972`, has an `#ifdef ANDROID` block that builds the mask from eight per-core settings:
  ```cpp
  if (affinities[i] == group || affinities[i] == thread_class::general)
  {
      mask |= 1ull << i;
  }
  ```
  The settings are `Core/Affinity/CPU0` to `CPU7`, each a `thread_class` (general, ppu, spu, rsx) defaulting to `general`, `rpcs3/Emu/system_config.h:55-69`. The mask is applied with `sched_setaffinity(::gettid(), ...)`, `Utilities/Thread.cpp:3281-3282`, and still only when the scheduler mode is not `os` (`rpcs3/Emu/CPU/CPUThread.cpp:637`). So the Android port's answer was "let the user reserve cores per thread class", off by default.

##### Spin, yield and wait policy

- `busy_wait` is TSC based and was written for x86. On ARM it is rescaled, with a comment that names our hardware class. `utils::busy_wait` and `init_arm_timer_scale`, `rpcs3/util/asm.hpp:189-220`:
  ```cpp
  // For instance, on the snapdragon 8 gen 2, the hardware timer runs at 19.2mhz.
  // This means that a busy wait that would have taken nanoseconds on x86 will run for
  // many microseconds on many arm machines.
  ```
  `utils::pause` uses `isb` on ARM64 (`asm.hpp:178-187`). Any spin loop of ours counted in "timer ticks" has the same hazard.
- Sleeping: `thread_ctrl::wait_for`, `Utilities/Thread.cpp:3090-3170`, uses a per-thread `timerfd` on Linux for non-alertable waits. `thread_ctrl::wait_for_accurate`, `Thread.cpp:3200-3247`, is a plain `wait_for` on Linux; on other systems it sleeps in multiples of a 500 us quantum, then `std::this_thread::yield()`, then `busy_wait(100)`.
- Guest timed waits: `lv2_obj::wait_timeout`, `rpcs3/Emu/Cell/lv2/lv2.cpp:2154-2273`. Host quantum assumed 10 us on Linux and 500 us elsewhere (`:2220-2227`). Below the quantum it uses `tpause`/`mwaitx` on x86 (`:2247-2260`) and otherwise yields (`:2262-2266`, "May cause long wake latency but helps weaker CPUs a lot by alleviating resource pressure"). The `Sleep Timers Accuracy` setting chooses how much of this applies (`:2230`); its default is "as host" on Linux and macOS, "usleep" elsewhere, `rpcs3/Emu/system_config.h:89-93`.
- "Preferred SPU Threads" (`system_config.h:34`, default 0 = off) limits how many SPU threads may execute the same code address at once; the others sleep 1 ms at a time or yield, for at most "SPU delay penalty" ms (`system_config.h:35`, default 3). Call site: `spu::scheduler::concurrent_execution_watchdog`, `rpcs3/Emu/Cell/SPUThread.cpp:612-614`, calling `acquire_pc_address` defined directly above it in the same namespace. This is a deliberate throttle for hosts with fewer cores than guest threads.
- "SPU loop detection" (`system_config.h:36`, default off): a guest that polls the decrementer is made to yield. `spu_thread::get_ch_value`, `SPUThread.cpp:5702-5707`:
  ```cpp
  //Polling: We might as well hint to the scheduler to slot in another thread since this one is counting down
  if (g_cfg.core.spu_loop_detection && out > spu::scheduler::native_jiffy_duration_us)
  {
      state += cpu_flag::wait;
      std::this_thread::yield();
  ```
- "Max SPURS Threads" (`system_config.h:37`, default 6) caps how many SPURS worker threads of a group run: `spu_thread::get_ch_value`, `SPUThread.cpp:5974-5988` ("Hack: don't run more SPURS threads than specified.").
- "Max CPU Preempt Count" (`system_config.h:86`, default 0 = off) is a frame-pacing-driven guest CPU throttle. `rsx::display_engine::evaluate_cpu_usage_reduction_limits`, `rpcs3/Emu/RSX/Core/RSXDisplayEngine.cpp:451-545`, keeps a 60 frame history and raises the number of forced preemptions per frame by 4 while frame times hold a known target (`:484-493`) and cuts it by 10 or more when they slip (`:495-509`). `lv2_obj::schedule_all`, `lv2.cpp:1979-2021`, then sets `cpu_flag::preempt` on a running PPU thread at that frequency, and `cpu_thread::check_state`, `rpcs3/Emu/CPU/CPUThread.cpp:974-999` and `:1099-1127`, turns it into a short sleep. It stops yielding near the end of the frame (`RSXDisplayEngine.cpp:534-538`). The idea: when the game already holds its frame rate, give the spare CPU time back instead of spinning.
- Reservation spin tuning exists as separate options (`SPU Reservation Busy Waiting`, `SPU GETLLAR Busy Waiting Percentage`, `system_config.h:28-31`, used at `SPUThread.cpp:4464-4481` and `:5846-5849`): details not checked.

##### Shader and pipeline compile threads

- Setting: `Shader Compiler Threads`, 0 to 16, default 0 = automatic, `rpcs3/Emu/system_config.h:175`.
- Automatic count, `vk::decay_num_worker_threads`, `rpcs3/Emu/RSX/VK/VKPipelineCompiler.cpp:267-306`: 24 or more host threads gives 12 workers, 16 or more gives 8, more than 12 gives 6, more than 8 gives 4, exactly 8 gives 2, fewer gives 1.
  ```cpp
  else if (hw_threads == 8)
  {
      num_worker_threads = 2;
  }
  else
  {
      num_worker_threads = 1;
  }
  ```
  On an 8-core phone RPCS3 would run two pipeline workers, not four.
- Two phases. During boot, while the shader cache is preloaded and no guest code runs, the pool is created with every host thread: `VKGSRender::VKGSRender`, `rpcs3/Emu/RSX/VK/VKGSRender.cpp:596-601` ("We spawn the initial pipe compiler with all threads during the boot sequence by default."). After preload it is cut to the in-game count: `VKGSRender::on_init`, `VKGSRender.cpp:1300-1304`, through `vk::resize_pipe_compiler`, `VKPipelineCompiler.cpp:337-352`, which keeps the threads but stops handing them work ("Just lie about how many compilers we have").
- Priority: each worker lowers itself before its loop, `vk::pipe_compiler::operator()`, `VKPipelineCompiler.cpp:35-37`:
  ```cpp
  void pipe_compiler::operator()()
  {
      thread_ctrl::scoped_priority low_prio(-1);
  ```
  (subject to the Linux caveat above).
- Affinity: none. The workers are not `cpu_thread`s, so no class mask applies; with a non-`os` scheduler they are reset to the whole process mask (`Utilities/Thread.cpp:2843-2848`).
- The default shader mode avoids waiting for a pipeline at all: `Shader Mode` defaults to `async_with_interpreter`, `system_config.h:131` (enum at `system_config_types.h:301-307`). The interpreter draws until the compiled pipeline is ready (behaviour stated in the comment at `rpcs3/Emu/RSX/rsx_cache.h:290`; the interpreter itself was not read).

##### Ahead-of-time compilation and on-disk caches

- PPU: `LLVM Precompilation` defaults to true (`system_config.h:24`). At boot `ppu_initialize()`, `rpcs3/Emu/Cell/PPUThread.cpp:5040-5072`, queues the firmware directory and all game directories and calls `ppu_precompile`, which compiles every module found with a "SPRX Worker" thread group at low priority (`PPUThread.cpp:4417-4422`). Worker count is the smaller of `Max LLVM Compile Threads` and one less than the host thread count on hosts with more than 8 threads (`PPUThread.cpp:4368-4369`). A global semaphore sized the same way limits concurrent LLVM instances (`jit_core_allocator`, `PPUThread.cpp:3877-3888`).
- PPU cache: object files in `<cache>/ppu-<sha1>-<module name>/` (`PPUThread.cpp:5217-5220`), named `v9-kusa-<hash>-<settings>-<cpu>.obj` (`:5816`), reloaded with `jit_compiler::load_object` (`:5850`). The guest never waits for the JIT in game.
- SPU cache: `spu_cache::initialize`, `rpcs3/Emu/Cell/SPUCommonRecompiler.cpp:835-980`, precompiles recorded SPU functions with "SPU Worker" threads at low priority (`:980`); count from `rpcs3::utils::get_max_threads`, `rpcs3/Emu/system_utils.cpp:24-30`.
- Shader cache: `rsx::shaders_cache`, `rpcs3/Emu/RSX/rsx_cache.h:231-235` (`shaders_cache/` unless `Disable On-Disk Shader Cache`), raw programs as `raw/<hash>.fp` and `.vp` (`:325-326`), pipeline states under `pipelines/<class>/<version>/` (`:360`). At boot `load` rebuilds every stored pipeline with one worker per host thread on Vulkan (`:297`) and, with the interpreter mode, lets the user skip after 60 s (`:290-293`).

##### Frame pacing

- Limiter: `Frame limit`, default `auto`, `system_config.h:128`; values none, 30, 50, 60, 120, display rate, auto, PS3 native, infinite, `system_config_types.h:218-229`. `rsx::display_engine::handle_emu_flip`, `rpcs3/Emu/RSX/Core/RSXDisplayEngine.cpp:127-168`, computes a target flip time and sleeps the RSX thread until then:
  ```cpp
  const auto delay_us = target_rsx_flip_time - time;
  lv2_obj::wait_timeout(delay_us, nullptr, false);
  ```
  `auto` means the guest vblank rate (`:138`). A `Second Frame Limit` can cap lower (`:145-149`). The `_ps3` mode defers the flip to the next emulated vblank instead (`:169-190`).
- Guest vblank: a dedicated "VBlank Thread", `rsx::display_engine::start_vblank_thread`, `RSXDisplayEngine.cpp:331-411`. Rate from `Vblank Rate` (default 60, `system_config.h:178`) with an NTSC period correction (`:343`). It schedules against absolute deadlines computed from a start time so that error does not accumulate (`:354`), sleeps with `thread_ctrl::wait_for` (`:389`) and yields for the last fraction (`:393`). It is independent of the host display.
- Host presentation: `VSync Mode`, default off, `system_config.h:133`. `vk::swapchain_WSI::init`, `rpcs3/Emu/RSX/VK/vkutils/swapchain.cpp:235-250`: off prefers IMMEDIATE, then MAILBOX, then FIFO_RELAXED; adaptive prefers MAILBOX, then FIFO_RELAXED; full uses FIFO.
- No Swappy, no Choreographer (see next sub-heading for the search terms). Pacing is the emulator's own timer, not the display's.

##### Android performance APIs

- Upstream RPCS3: none. Search terms, whole tree excluding `3rdparty` and `.git`: `APerformanceHint`, `PerformanceHint`, `AThermal`, `thermal`, `Swappy`, `Choreographer`, `sched_setaffinity`, `setpriority`, `cpu_capacity`, `cpuinfo_max_freq` returned no file. `GameMode` matches only the Linux Feral GameMode client described above. `QOS_CLASS` matches only the macOS line in `Utilities/Thread.cpp:2801`.
- Android fork core (`1ede37d5`): same terms. Only `sched_setaffinity` matched (`Utilities/Thread.cpp:3282`, the user-driven affinity above). No ADPF, no thermal API, no Swappy.
- Android shell (`rpcs3-android` at `e2f87a7f`), Kotlin and manifest. Search terms: the list above plus `setThreadPriority`, `THREAD_PRIORITY`, `affinity`, `priority`, `setFrameRate`, `sustainedPerformance`, `PowerManager`, `WakeLock`. Hits:
  - `app/src/main/AndroidManifest.xml:26`, `android:isGame="true"`, and `:36-38`, a `android.game_mode_config` meta-data entry.
  - `app/src/main/res/xml/game_config.xml:2-7`:
    ```xml
    android:supportsBatteryGameMode="true"
    android:supportsPerformanceGameMode="true"
    android:allowGameDownscaling="false"
    android:allowGameFpsOverride="false"/>
    ```
    That declares support for the system Game Mode and forbids the platform from downscaling or overriding the frame rate. No code reads the selected mode.
  - Ahead-of-time compilation is run as an Android foreground service so it survives the app going to the background: `PrecompilerService`, `app/src/main/java/net/rpcs3/PrecompilerService.kt:19-31` (`context.startForegroundService(intent)`).
- `rpcsx-ui-android` (`1e588cbe`): same picture, only `game_config.xml:4-5` matched. Its native core was not checked.

---

#### 2. Vita3K

- Repository: https://github.com/Vita3K/Vita3K
- Commit read: `c7d90f505cbfaf685ea0c09ba863b0109b223ef6`, 2026-11-12 (committer date 2026-11-12T20:12:10+01:00)
- The Android code is in the same repository (`android/` for the Kotlin app, `vita3k/android/jni/` for the native glue). No separate repository was needed.
- All searches below exclude `external/` (third-party submodules) and `.git`.

##### Guest CPUs/threads to host threads

- One detached host thread per guest thread, created through SDL. `KernelState::create_thread`, `vita3k/kernel/src/kernel.cpp:160-190`:
  ```cpp
  SDL_DetachThread(SDL_CreateThread(&thread_function, thread->name, &params));
  ```
  The host thread body is `thread_function`, `kernel.cpp:63-91`, which calls `thread->run_loop()` (`:77`).
- `ThreadState::run_loop`, `vita3k/kernel/src/thread.cpp:191-205`, keeps the host thread for the whole life of the guest thread. Between runs and while blocked, the host thread parks on a condition variable in `ThreadState::run_guest`, `thread.cpp:257-260` ("Park until the thread runs: dormant between starts, or busy elsewhere for a call that another host thread makes").
- There is no emulator-side scheduler that limits how many guest threads run at once. The "core number" handed to each guest CPU (`corenum_allocator.new_corenum()`, `kernel.cpp:162`) is an index for the Dynarmic exclusive monitor, with `MAX_CORE_COUNT = 150` (`vita3k/cpu/include/cpu/common.h:39`, used at `vita3k/cpu/src/dynarmic_cpu.cpp:283`). It is not a host core.
- Other long-lived host threads found (search terms: `std::thread`, `std::jthread`, `SDL_CreateThread`, `std::async`, `thread_pool`): the render thread (`vita3k/renderer/src/batch.cpp:291`), the vblank thread (`vita3k/display/src/display.cpp:97`), a GXM display-queue thread (`vita3k/modules/SceGxm/SceGxm.cpp:2927`), a Vulkan GPU request wait thread (`vita3k/renderer/src/vulkan/creation.cpp:62`), the pipeline compile workers (below), plus network, input overlay, logging and debugger threads.

##### Priorities and nice values

- Guest priority is stored and used only for guest-visible behaviour. `ThreadState::change_priority`, `vita3k/kernel/src/thread.cpp:716-720`:
  ```cpp
  tls_slot(TLS_CURRENT_PRIORITY) = new_priority;
  return std::exchange(priority, new_priority);
  ```
  It orders waiters in the kernel sync primitives (for example `vita3k/kernel/src/sync_primitives.cpp:357`, `waiter.priority = thread->get_priority();`, and `:655`). The guest calls are `sceKernelChangeThreadPriority2` and `sceKernelChangeThreadPriority`, `vita3k/modules/SceKernelThreadMgr/SceThreadmgr.cpp:1281-1299`.
- No host priority is set for any thread. Search terms with no hit: `set_thread_priority`, `pthread_setschedparam`, `SetThreadPriority`, `setThreadPriority`, `setpriority`, `nice(`, `QOS_CLASS`, `SCHED_`, `sched_param`, `SDL_SetThreadPriority`, `SDL_SetCurrentThreadPriority`, `SDL_THREAD_PRIORITY`. Every Vita3K thread, guest CPU, renderer, vblank and pipeline compiler alike, runs at the default priority.

##### Core placement

- Nothing is done, on any platform including ARM and Android. Search terms with no hit: `sched_setaffinity`, `pthread_setaffinity`, `SetThreadAffinityMask`, `cpu_capacity`.
- The guest affinity mask is recorded only. `ThreadState::change_affinity_mask`, `vita3k/kernel/src/thread.cpp:722-726`, writes the TLS slot and the field; the guest call is `sceKernelChangeThreadCpuAffinityMask`, `SceThreadmgr.cpp:1268-1279`.
- `cpuinfo_max_freq` is read, but only to print the CPU clock in the log and compatibility report. `util::get_cpu_clock_mhz`, `vita3k/util/src/sysinfo.cpp:240-252`:
  ```cpp
  // cpu0 is usually a little core on big.LITTLE SoCs, so take the max across all cores.
  ```
  Its only users are `util::get_system_info` (`sysinfo.cpp:273`), logged from `vita3k/android/jni/native_bootstrap.cpp:98` and `vita3k/main.cpp:211`.

##### Shader and pipeline compile threads

- Count, `PipelineCache::init`, `vita3k/renderer/src/vulkan/pipeline_cache.cpp:236-247`:
  ```cpp
  const int nb_logical_threads = SDL_GetNumLogicalCPUCores();
  // took this from RPCS3 (slightly modified)
  if (nb_logical_threads > 12)
      nb_worker_threads = 6;
  else if (nb_logical_threads > 8)
      nb_worker_threads = 4;
  else if (nb_logical_threads >= 8)
      nb_worker_threads = 3;
  ```
  then 2 for six or seven logical CPUs and 1 below that. An 8-core phone gets three workers.
- Creation: plain `std::thread`s, only when asynchronous compilation is enabled. `PipelineCache::set_async_compilation`, `pipeline_cache.cpp:256-284` (`worker_threads.emplace_back(&PipelineCache::compiler_thread, ...)` at `:269`).
- Worker loop: `PipelineCache::compiler_thread`, `pipeline_cache.cpp:863-889`, blocks on a `moodycamel` queue (`wait_dequeue`, `:869`) and compiles one pipeline per request. No priority change, no affinity.
- The draw path does not wait. `PipelineCache::retrieve_pipeline`, `pipeline_cache.cpp:1088-1110`, enqueues the request, marks the slot as compiling and returns `nullptr` to the caller. Requests that are unsafe to defer are compiled inline (`:1111-1113`, "can't wait, compile it right now").
- The driver pipeline cache is written back to disk on a delay after compiles (`next_pipeline_cache_save`, `pipeline_cache.cpp:881-882`); the save path was not read further.

##### Frame pacing

- Guest vblank is a free-running 60 Hz host thread. `vblank_sync_thread`, `vita3k/display/src/display.cpp:40-94`, with `TARGET_FPS = 60` (`:34`). It aligns to wall-clock multiples of the frame period:
  ```cpp
  const auto time_left = TARGET_MICRO_PER_FRAME - (time_ms % TARGET_MICRO_PER_FRAME);
  std::this_thread::sleep_for(std::chrono::microseconds(time_left));
  ```
  (`:90-92`). Guest threads waiting for vblank are woken from there (`:86-88`). This is the only frame limiter: the guest paces itself on emulated vblanks.
- "FPS hack": `display_wait`, `vita3k/modules/SceDisplay/SceDisplay.cpp:36-39`, forces a requested wait of two vblanks down to one ("a game can use a vcount of 2 to render as 30fps"). Config `fps-hack`, default false, `vita3k/config/include/config/config.h:203`.
- Vulkan present mode: `ScreenRenderer::setup`, `vita3k/renderer/src/vulkan/screen_renderer.cpp:197-216`, comment "preferred order : mailbox > fifo_relaxed > fifo > whatever", starting from immediate. The `v-sync` setting (default true, `config.h:155`) is consumed through `pending_vsync`, whose only reader found is the OpenGL renderer (`vita3k/renderer/src/gl/renderer.cpp:759-760`); the Vulkan choice above does not consult it.
- No Swappy, no Choreographer, no `setFrameRate` (search terms: `Swappy`, `Choreographer`, `setFrameRate`, `ANativeWindow_setFrameRate`: no hit).

##### Android performance APIs

- None of the platform performance APIs are used. Search terms with no hit: `APerformanceHint`, `PerformanceHint`, `AThermal`, `Swappy`, `GameMode`, `game_mode`, `sched_setaffinity`, `setpriority`, `cpu_capacity`, `Choreographer`, `sustainedPerformance`.
- `thermal` has one hit, a settings string, not an API call (see turbo mode below). `cpuinfo_max_freq` has the display-only hit described above.
- The manifest declares the app as a game: `android/app/src/main/AndroidManifest.xml:97-98`, `android:appCategory="game"` and `android:isGame="true"`. There is no `game_mode_config`.
- The one performance lever is on the GPU side: "GPU turbo mode" through libadrenotools. `VKState::set_turbo_mode`, `vita3k/renderer/src/vulkan/renderer.cpp:1849-1856`:
  ```cpp
  if (!support_custom_drivers())
      return;

  adrenotools_set_turbo(set);
  ```
  It is limited to Qualcomm GPUs (`vendorID == 0x5143`, `renderer.cpp:1844-1847`), is off by default (`turbo-mode`, `config.h:149`) and is described to the user as "Provides a way to force the GPU to run at the maximum possible clocks. Thermal constraints will still be applied." (`android/app/src/main/res/values/strings.xml:411`).

Summary for Vita3K: it relies entirely on the OS scheduler. Its smoothness comes from never blocking a draw on a pipeline compile and from a small worker count, not from priorities or pinning.

---

#### 3. Upstream Xenia (Canary and master)

- Canary: https://github.com/xenia-canary/xenia-canary, commit `4f04bc7cc76b89bfcf7327b597d9770eaaddf935`, 2026-11-12 (committer date 2026-11-12T19:28:06+00:00). Cited as "canary".
- Master: https://github.com/xenia-project/xenia, commit `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`, 2026-02-18 (committer date 2026-02-18T22:40:20+03:00). Cited as "master".
- Paths are relative to each repository root. Where only one tree is cited, the other was not read for that point.

##### Guest CPUs/threads to host threads

- One host thread per guest thread, no emulator-side scheduler. `XThread::Create`, canary `src/xenia/kernel/xthread.cc:414-419`:
  ```cpp
  params.create_suspended = true;

  params.stack_size = 16_MiB;  // Allocate a big host stack.
  thread_ = xe::threading::Thread::Create(params, [this]() {
  ```
  Master: `XThread::Create`, `src/xenia/kernel/xthread.cc:378`.
- The six guest hardware threads exist only as a number stored in guest structures. `GetFakeCpuNumber`, canary `xthread.cc:157-170`: a thread created without a processor mask inherits its parent's CPU number, otherwise `next_cpu = (next_cpu + 1) % 6;`. `XThread::SetActiveCpu`, canary `xthread.cc:833-856` (master `:747-766`), writes that number into the PCR and KTHREAD. Any number of guest threads may run at once on any host core.
- Emulator service threads are `XHostThread`s created with creation flags 0, so with no priority request:
  - "GPU Commands": canary `src/xenia/gpu/command_processor.cc:224-233`, master `:73-79`.
  - "Audio Worker": canary `src/xenia/apu/audio_system.cc:78-88`, master `:76-83`.
  - "XMA Decoder": canary `src/xenia/apu/xma_decoder.cc:176-190`, master `:144-150`.
  - Guest vblank timer: "GPU Frame limiter" in canary (`src/xenia/gpu/graphics_system.cc:150-286`), "GPU VSync" in master (`src/xenia/gpu/graphics_system.cc:97-118`).

##### Priorities and nice values

- Host priority levels. Canary `src/xenia/base/threading.h:410-424`: on Windows `kLowest = -2` to `kHighest = 2`; on other platforms
  ```cpp
  static constexpr int32_t kLowest = 1;
  static constexpr int32_t kBelowNormal = 8;
  static constexpr int32_t kNormal = 16;
  static constexpr int32_t kAboveNormal = 24;
  static constexpr int32_t kHighest = 32;
  ```
  These are meant as `SCHED_FIFO` priorities. (Master: only the Windows values at `threading.h:393-397` were read.)
- POSIX implementation in master: real-time or nothing. `PosixCondition<Thread>::set_priority`, master `src/xenia/base/threading_posix.cc:659-665`:
  ```cpp
  param.sched_priority = new_priority;
  if (pthread_setschedparam(thread_, SCHED_FIFO, &param) != 0)
    assert_always();
  ```
  An unprivileged Linux or Android process is refused `SCHED_FIFO` (from memory, not verified), so in master host priorities do nothing there.
- POSIX implementation in canary: real-time first, nice as fallback. `set_priority`, canary `src/xenia/base/threading_posix.cc:872-903`:
  ```cpp
  // Fall back to nice values under SCHED_OTHER.
  // Map SCHED_FIFO range (1-32) to nice range (19 to -19).
  int nice_val = 16 - new_priority;
  ...
  setpriority(PRIO_PROCESS, tid_, nice_val);
  ```
  That gives kLowest nice 15, kBelowNormal nice 8, kNormal nice 0, kAboveNormal nice -8, kHighest nice -16 (computed from the two snippets above). The return value of `setpriority` is not checked (`:900-902`); whether Android lets an app thread go below nice 0 this way was not verified.
- A thread created with a non-zero `initial_priority` is created directly as `SCHED_FIFO` (canary `threading_posix.cc:697-708`, master `:501-512`). Almost every caller passes `{}`.
- Guest priority to host priority, canary. `GuestPriorityToHost`, canary `src/xenia/kernel/xthread.cc:696-708`: guest 24 and above is kHighest, 17 and above kAboveNormal, 10 and above kNormal, 5 and above kBelowNormal, lower is kLowest. `XThread::SetPriority`, `xthread.cc:710-723`:
  ```cpp
  if (!cvars::ignore_thread_priorities) {
    thread_->set_priority(GuestPriorityToHost(clamped));
  }
  ```
  Reached from `KeSetBasePriorityThread_entry` (canary `src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:390-398`) and `KeSetPriorityThread_entry` (`:1900-1924`).
- Guest priority to host priority, master. `XThread::SetPriority`, master `xthread.cc:719-736`: thresholds on the raw increment (above 0x22 kHighest, above 0x11 kAboveNormal, below -0x22 kLowest, below -0x11 kBelowNormal).
- The cvar that gates it differs between the trees. Master `xthread.cc:31-32`: `DEFINE_bool(ignore_thread_priorities, true, ...)`. Canary `xthread.cc:28-30`:
  ```cpp
  DEFINE_bool(ignore_thread_priorities, false,
              "Ignores game-specified thread priorities.", "Kernel");
  UPDATE_from_bool(ignore_thread_priorities, 2026, 4, 9, 12, true);
  ```
  So canary now applies guest priorities by default; the `UPDATE_from_bool` line reads as a migration from an earlier default of true (the macro itself was not read).
- Canary also emulates the console's dynamic priorities on top of that:
  - Decay: `XThread::CheckQuantumAndDecay`, canary `xthread.cc:725-766`, lowers a non-real-time thread by one level per 20 ms of run time, never below its base ("We approximate this by decaying 1 priority level per 20ms of elapsed wall-clock time."). It is driven for every running guest thread from the kernel tick, canary `src/xenia/kernel/kernel_state.cc:1308-1317`.
  - Boost: `XThread::BoostOnWake`, canary `xthread.cc:768-822`, raises a thread when a wait is satisfied, called from `src/xenia/kernel/xobject.cc:240`, `:275` and `:370`.
  - Both end in `thread_->set_priority(GuestPriorityToHost(...))` whenever the guest value changes (`xthread.cc:763`, `:817`), without first checking that the host level changed. On the nice fallback each such call is a `setpriority` system call.
- A point to check in any POSIX fork. `XThread::Create`, canary `xthread.cc:456-458` (master `:414-416`), not gated by the cvar:
  ```cpp
  if (creation_params_.creation_flags & 0x60) {
    thread_->set_priority(creation_params_.creation_flags & 0x20 ? 1 : 0);
  }
  ```
  The literals 1 and 0 fit the Windows scale (above normal, normal). On the POSIX scale 1 is kLowest, and with canary's fallback 1 becomes nice 15 and 0 becomes nice 16. If that reading is right, guest threads created with those flags are pushed to the bottom on Linux and Android. The meaning of flags 0x20 and 0x40 was not checked.
- Emulator threads. Search terms `set_priority(`, `initial_priority`, `ThreadPriority::k` over `src/xenia`, excluding `base/` and `xthread.cc`:
  - Master: no hit. No emulator thread has a priority.
  - Canary: two hits. The frame limiter thread, `GraphicsSystem::Setup`, `src/xenia/gpu/graphics_system.cc:287-298`, is kLowest on Windows and kNormal elsewhere, with the reason stated:
    ```cpp
    // nothing and costs wake-up latency: kLowest maps to nice 15 against nice 0
    // for every other thread in the process, and any other busy program then
    // delays the guest's vblanks.
    ```
    And a profiler thread at kLowest behind a debug cvar (`src/xenia/cpu/backend/x64/x64_backend.cc:358-368`).
  - So in both trees the GPU command thread, the audio worker, the XMA decoder and the pipeline creation threads all run at the default priority, the same as guest threads at guest "normal". This is the situation we measured.

##### Core placement

- Guest affinity is ignored by default in both trees: `DEFINE_bool(ignore_thread_affinities, true, ...)`, canary `xthread.cc:31-32`, master `xthread.cc:33-34`.
- If enabled, it is a literal one-to-one pin of guest CPU n to host CPU n. `XThread::SetActiveCpu`, canary `xthread.cc:847-850` (master `:761-764`):
  ```cpp
  if (xe::threading::logical_processor_count() >= 6) {
    if (!cvars::ignore_thread_affinities) {
      thread_->set_affinity_mask(uint64_t(1) << cpu_index);
  ```
  Entry points: `XThread::SetAffinity` (canary `xthread.cc:824-826`) from `KeSetAffinityThread_entry` (canary `xboxkrnl_threading.cc:349`, master `:240-254`).
- POSIX implementation, both trees: `sched_setaffinity(pthread_gettid_np(thread_), ...)` on Android and `pthread_setaffinity_np` elsewhere, canary `src/xenia/base/threading_posix.cc:833-852`, master `:626-645`.
- No knowledge of core types. Host CPUs 0 to 5 are used whatever they are; on a phone that numbering may or may not start with the slowest cores. `logical_processor_count` is `std::thread::hardware_concurrency()` (canary `src/xenia/base/threading.cc:15-21`). Search terms with no hit in either tree (excluding `third_party`): `cpu_capacity`, `cpuinfo_max_freq`. Nothing is done specifically for ARM or Android.
- No emulator thread (GPU, audio, pipelines) is ever given an affinity: the only `set_affinity_mask(` call outside `base/` is the one above (same search as in the previous sub-heading).

##### Shader and pipeline compile threads

- Canary, Vulkan. `vulkan_pipeline_creation_threads`, default -1, canary `src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc:54-60`: "-1 to calculate automatically (75% of logical CPU cores)". `VulkanPipelineCache::Initialize`, `vulkan_pipeline_cache.cc:280-306`:
  ```cpp
  creation_thread_count =
      std::max(logical_processor_count * 3 / 4, uint32_t(1));
  ...
  xe::threading::Thread::Create({}, [this]() { CreationThread(); });
  ...
  creation_thread->set_name("Vulkan Pipelines");
  ```
  On 8 logical CPUs that is six threads. They are created with default parameters: no priority, no affinity.
- Canary does not block the draw on them. `async_shader_compilation`, default true (canary `src/xenia/gpu/gpu_flags.cc:105-112`). `VulkanPipelineCache::ConfigurePipeline`, `vulkan_pipeline_cache.cc:778-790`, builds a placeholder pipeline with a simple pixel shader at once and queues the real one ("This reduces stutter from pipeline compilation."). `VulkanPipelineCache::CreationThread`, `:928-945`, takes work from `creation_queue_.top()`, so the queue is ordered (the ordering key was not read).
- D3D12 uses the same 75% rule in both trees: `d3d12_pipeline_creation_threads`, canary `src/xenia/gpu/d3d12/pipeline_cache.cc:51` and `:165-194`, master `src/xenia/gpu/d3d12/pipeline_cache.cc:54-60` and `:159-176`.
- Master, Vulkan: no creation threads at all. `creation_thread` does not occur in master `src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc` (count 0); `vkCreateGraphicsPipelines` is called at `:2169`, which by elimination is on the GPU command thread (the call path was not traced).
- Canary also has a shader storage write thread, `src/xenia/gpu/shader_storage.h:370`, default priority.

##### Frame pacing

- Cvars: `vsync`, default true (canary `src/xenia/gpu/gpu_flags.cc:27`, master `:21`). `framerate_limit` exists only in canary (`gpu_flags.cc:29-33`, default 0, "Defaults to 60, when set to 0, and VSYNC is enabled.").
- The guest vblank is a host timer thread, not the display. Master, `GraphicsSystem::Setup`, `src/xenia/gpu/graphics_system.cc:97-112`, polls every millisecond:
  ```cpp
  uint64_t vsync_duration = cvars::vsync ? 16 : 1;
  ...
  if (elapsed >= vsync_duration) {
    MarkVblank();
    last_frame_time = current_time;
  }
  xe::threading::Sleep(std::chrono::milliseconds(1));
  ```
- Canary replaces that on non-Windows hosts with absolute-deadline pacing that delivers missed beats up to a cap. `GraphicsSystem::Setup`, canary `graphics_system.cc:240-272`:
  ```cpp
  next_vblank_deadline += period;
  if (next_vblank_deadline > now) {
    threading::NanoSleep(...);
  } else if (now - next_vblank_deadline > kMaxVblankCatchUp) {
  ```
  The comment above it gives the reason: a flat sleep adds wake-up latency to every period and "none of it is ever made up".
- Host presentation is deliberately decoupled and prefers low latency. Canary `src/xenia/ui/vulkan/vulkan_presenter.cc:1222-1251` (enclosing function not checked) tries IMMEDIATE, then MAILBOX, then FIFO_RELAXED, then FIFO, each allowed by a cvar that defaults to true (`:34`, `:40`, `:45`). Master is the same (`vulkan_presenter.cc:1200-1219`, cvars at `:39`, `:45`, `:50`). Stated reason: "it's better to use modes that allow as quick acquisition as possible to avoid interfering with GPU command processing".
- The GPU command thread spins when idle. Canary `command_processor.cc:376-400` (enclosing function not checked): up to 500 rounds of `xe::threading::MaybeYield()`, then 2 ms waits on an event ("We spin here waiting for new ones, as the overhead of waiting on our event is too high."). Master `command_processor.cc:211-227` is the same with a 5 ms wait. On POSIX `MaybeYield` is `sched_yield()` (canary `src/xenia/base/threading_posix.cc:150-153`).
- No Swappy and no Choreographer in either tree (search terms below).

##### Android performance APIs

- Nothing in either tree. Search terms, whole repository excluding `.git` and `third_party`: `APerformanceHint`, `PerformanceHint`, `AThermal`, `thermal`, `Swappy`, `GameMode`, `Choreographer`, `cpu_capacity`, `cpuinfo_max_freq`, `isGame`, `appCategory` returned no file.
- `sched_setaffinity` matches only `src/xenia/base/threading_posix.cc` in both trees. `setpriority` matches only canary's `threading_posix.cc`. `game_mode` matches only an unrelated profile header in canary (`src/xenia/kernel/xam/xdbf/spa_info.h`).

---

#### What an Android Xenia fork can take from this

- Adopt RPCS3's three-level ordering, which is the direct answer to the run-queue measurement: GPU command thread and audio one step above the guest CPU threads, pipeline compile threads (and any JIT, cache or log worker) one or two steps below. RPCS3 does exactly this (`RSXThread.cpp:867`, `cellAudio.cpp:891`, `VKPipelineCompiler.cpp:37`). On Android it has to be done with per-thread nice values: RPCS3's own call is ineffective under the default Linux policy, upstream Xenia master only tries `SCHED_FIFO`, and canary's nice fallback (`threading_posix.cc:889-902`) is the working model. With that mapping a pipeline thread at kBelowNormal is nice 8 and at kLowest nice 15.
- Run fewer compile threads while the guest is running, and more only while it is not. For 8 host threads RPCS3 uses 2 in game (`VKPipelineCompiler.cpp:292-295`) but every core during the boot-time cache preload (`VKGSRender.cpp:596-601`, `:1300-1304`); Vita3K uses 3. Canary's 75% rule gives 6 and our 4 is still more than either of the others.
- Keep the draw path from waiting on a compile, so that low-priority compile threads cost only a short visual substitute and not a stall: RPCS3's interpreter mode, canary's placeholder pipeline (`vulkan_pipeline_cache.cc:778-790`), Vita3K's deferred pipeline (`pipeline_cache.cpp:1088-1110`). Pair it with on-disk caches rebuilt at boot (RPCS3 `rsx_cache.h:290-297`) so that most compile work never competes with the guest at all.
- Do not expect a ready-made core placement policy. None of the three detects big and little cores. RPCS3 pins only for known desktop Ryzen and Intel layouts and defaults to "Operating System"; its Android fork offered a manual per-core class table, off by default; Xenia's pin is one guest CPU to one host CPU of the same number and is off by default. If we pin, copy RPCS3's reset of the inherited mask in new threads (`Utilities/Thread.cpp:2843-2848`), otherwise worker threads created from a pinned thread inherit the pin.
- Audit what we inherit from Xenia before adding anything: the unconditional `set_priority(1 or 0)` for creation flags 0x60 (`xthread.cc:456-458`), the per-change `setpriority` calls from quantum decay and wake boost if guest priorities are enabled, and the GPU command thread's `sched_yield` spin (`command_processor.cc:376-400`), which at equal nice competes with guest CPU threads for the same cores.
- Timing details worth copying: absolute-deadline vblank pacing (RPCS3 `RSXDisplayEngine.cpp:354`, canary `graphics_system.cc:240-272`), `prctl(PR_SET_TIMERSLACK, 1)` (RPCS3 `rpcs3.cpp:576`), and rescaling of counter-based busy waits on ARM (RPCS3 `asm.hpp:189-220`). None of the three uses ADPF, thermal headroom, Swappy or Choreographer, so anything we do with those APIs is new work without a reference implementation here. The only Android declarations found are manifest level (`isGame`, a `game_mode_config` in the RPCS3 Android shell) and Vita3K's Adreno turbo switch.

#### Caveats

- Statements about what the Linux or Android kernel permits (no effect of `pthread_setschedparam` under `SCHED_OTHER`, refusal of `SCHED_FIFO` and of negative nice for an app) are from memory and are marked where they occur. They should be confirmed on the device with `ps -T -o tid,ni,comm`.
- The reading of the `creation_flags & 0x60` path in Xenia is from the code shown; the flag meanings and whether any title uses them were not checked.
- "Not checked" items: the RPCSX core used by the current RPCS3 Android UI, RPCS3's shader interpreter, RPCS3 reservation spin options, the ordering key of canary's pipeline creation queue, master's non-Windows `ThreadPriority` values, and several enclosing function names noted in place.

## Appendix B: Dolphin, PPSSPP, PCSX2 and ARMSX2

As written by the research pass that read the source, unedited but for heading levels. Paths are relative to each repository at the commit named. "(session scratch)" was the session's working directory, which is not kept: the citations name the repository, commit and path, which is enough to find each one.

### Survey: host thread priority, core placement and frame pacing in Dolphin, PPSSPP and PCSX2

Method: every statement below comes from source opened in this session (shallow clones, HEAD of the default branch on the day of reading). Citations are `path:line` relative to the repository root, with the function name where it was read. "Not found" statements list the search terms used. Anything not read is marked "not checked" or "from memory, not verified".

Short version for readers in a hurry: none of the three upstream emulators sets a host thread priority or nice value for its emulation, GPU or shader compile threads. Dolphin and PPSSPP also do no core pinning. PCSX2 has opt-in pinning by core frequency (Linux and Windows only, off by default). The only Android performance API in use is in PPSSPP: an ADPF performance hint session on the emulation thread, plus `Surface.setFrameRate` and an optional sustained performance mode.

---

#### 1. Dolphin

Repository: https://github.com/dolphin-emu/dolphin
Commit read: `db2afcbdf8f5175a6b8414d9f5e21c978775f651`, committed 2026-11-11T14:08:27-06:00.

##### Guest CPUs/threads to host threads

- The guest has one CPU, and one host thread runs it. `EmuThread` (`Source/Core/Core/Core.cpp:509`) is started as a `std::thread` at `Source/Core/Core/Core.cpp:249` (enclosing function not read), boots the title, then becomes the CPU thread: `// Become the CPU thread.` followed by `cpu_thread_func(system, savestate_path, delete_savestate);` (`Core.cpp:673-674`).
- `CpuThread` (`Core.cpp:321-327`) names itself by mode:
  ```cpp
  if (system.IsDualCoreMode())
    Common::SetCurrentThreadName("CPU thread");
  else
    Common::SetCurrentThreadName("CPU-GPU thread");
  ```
- Dual core mode adds one GPU thread. `GetInitializedVideoGuard` (`Core.cpp:442-500`) spawns `std::thread gpu_thread`, names it `"Video thread"`, initialises the video backend on it and runs `system.GetFifo().RunGpuLoop()` (`Core.cpp:469-478`). In single core mode the backend is initialised on the CPU thread and there is no GPU thread (`Core.cpp:497-500`).
- Dual core is the Android default in this revision: `DEFAULT_CPU_THREAD = true` under `#if defined(ANDROID)` with the comment "Currently enabled by default on Android because the performance boost is really needed", and `false` otherwise (`Source/Core/Core/Config/MainSettings.cpp:59-65`).
- On Android the host side is a plain Java thread: `Thread({ ... NativeLibrary.Run(paths, riivolution) ... }, "NativeEmulation")` (`Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/fragments/EmulationFragment.kt:218-260`, `runWithValidSurface`). The JNI `Run` (`Source/Android/jni/MainAndroid.cpp:557-600`) calls `BootManager::BootCore` and then only waits on an event and dispatches host jobs (`MainAndroid.cpp:590-594`).

How the CPU thread waits for, or runs ahead of, the GPU thread:

- The GPU thread loop is `Common::BlockingLoop` (`Source/Core/Common/BlockingLoop.h`). `Wakeup()` (`BlockingLoop.h:35-49`) is lock free in the common case and only signals an event if the worker is asleep. `Run()` (`BlockingLoop.h:126-198`) busy loops (re-runs the payload without sleeping) until some thread has allowed sleep, then waits on an event:
  ```cpp
  if (m_may_sleep.TestAndClear())
  { ... } else {
    // Busy loop.
    break;
  }
  ```
  (`BlockingLoop.h:165-175`). `RunGpuLoop` passes a 100 ms sleep timeout (`Source/Core/VideoCommon/Fifo.cpp:293-396`).
- `Common::Event` is a flag plus mutex plus condition variable, with no spinning (`Source/Core/Common/Event.h:25-52`, `Set`, `Wait`).
- By default the CPU thread runs ahead freely: it writes the FIFO and calls `FifoManager::RunGpu` (`Fifo.cpp:420-428`), which only does `m_gpu_mainloop.Wakeup()`.
- SyncOnSkipIdle (default `true`, `MainSettings.cpp:67`): when the guest idles, the CPU thread blocks until the GPU thread has drained. `CoreTimingManager::Idle` (`Source/Core/Core/CoreTiming.cpp:574-582`) calls `FlushGpu()`, which is `m_gpu_mainloop.Wait()` (`Fifo.cpp:398-404`), a condition variable wait (`BlockingLoop.h:53-73`).
- SyncGPU (default `false`, `MainSettings.cpp:220`, distances +200000 and -200000 ticks at `:221-222`): a tick budget shared by both threads. `FifoManager::WaitForGpuThread` (`Fifo.cpp:542-564`) blocks the CPU thread when it is too far ahead: `if (now >= m_config_sync_gpu_max_distance) m_sync_wakeup_event.Wait();`. The GPU thread sets that event once it has caught up (`Fifo.cpp:363-371`, inside `RunGpuLoop`).
- At each guest frame, `VideoBackendBase::Video_OutputXFB` (`Source/Core/VideoCommon/VideoBackendBase.cpp:95-112`) calls `SyncGPU(SyncGPUReason::Swap)` and queues the swap to the GPU thread together with its intended host presentation time.
- One place explicitly stops the GPU thread spinning: `VideoBackendBase::DoState` calls `GpuMaySleep()` "so we're not spinning if paused after loading a state" (`VideoBackendBase.cpp:284-286`).

##### Priorities and nice values

- None. `Source/Core/Common/Thread.h:19-20` declares only affinity and naming helpers. There is no priority helper.
- Searched `Source/` for: `setpriority`, `pthread_setschedparam`, `SetThreadPriority`, `SetPriorityClass`, `THREAD_PRIORITY`, `SCHED_FIFO`, `SCHED_RR`, `SCHED_BATCH`, `SCHED_IDLE`, `nice(`, `QOS_CLASS`, `pthread_set_qos`, `set_qos_class`, `Process.setThreadPriority`, `android.os.Process`. No hits.
- The only scheduler related call found is Windows only: `SetProcessInformation(..., ProcessPowerThrottling, ...)` with `StateMask = 0`, commented "This adjusts behavior on CPUs with "performance" and "efficiency" cores", plus `timeBeginPeriod(1)` (`Source/Core/Common/Timer.cpp:96-112`, enclosing function not read).

##### Core placement

- Nothing is done on ARM or Android. `Common::SetThreadAffinity` exists (`Source/Core/Common/Thread.cpp:49` for Windows, `:117` for POSIX) but its POSIX body is compiled out on Android: `#elif (defined __linux__ || ...) && !(defined ANDROID)` (`Thread.cpp:121-122`).
- The helper also has no callers. A search of `Source/` for `SetThreadAffinity`, `SetCurrentThreadAffinity`, `sched_setaffinity`, `pthread_setaffinity` returns only the definitions in `Thread.cpp` and the declarations in `Thread.h`.
- Core types are not detected. `CPUInfo::Detect` on ARM sets `num_cores = std::max(static_cast<int>(std::thread::hardware_concurrency()), 1);` (`Source/Core/Common/ArmCPUDetect.cpp:226`). `cpu_capacity` and `cpuinfo_max_freq`: no hits in `Source/`.

##### Shader and pipeline compile threads

- Count at run time: `VideoConfig::GetShaderCompilerThreads` (`Source/Core/VideoCommon/VideoConfig.cpp:266-275`) returns the configured value, default 1 (`GFX_SHADER_COMPILER_THREADS`, `Source/Core/Core/Config/GraphicsSettings.cpp:112`), or for a negative value the automatic count `std::clamp(cpu_info.num_cores - 3, 1, 4)` (`GetNumAutoShaderCompilerThreads`, `VideoConfig.cpp:252-256`).
- Count before the game starts (precompile, when waiting for shaders): `std::max(cpu_info.num_cores - 2, 1)` (`GetNumAutoShaderPreCompilerThreads`, `VideoConfig.cpp:258-264`), with the comment "leave two logical cores for the dolphin UI and the rest of the OS". `ShaderCache::InitializeShaderCache` resizes the pool up for precompile and back down for run time (`Source/Core/VideoCommon/ShaderCache.cpp:57-79`).
- Priority and affinity: none. `AsyncShaderCompiler::WorkerThreadEntryPoint` only names the thread `"AsyncShaderCompiler Worker"` (`Source/Core/VideoCommon/AsyncShaderCompiler.cpp:206-225`). `WorkerThreadRun` (`:227-252`) sleeps on a condition variable and takes the first item of an ordered queue.
- The "priority" in this code is queue order only: `COMPILE_PRIORITY_ONDEMAND_PIPELINE = 100`, `COMPILE_PRIORITY_UBERSHADER_PIPELINE = 200`, `COMPILE_PRIORITY_SHADERCACHE_PIPELINE = 300` (`Source/Core/VideoCommon/ShaderCache.h:190-192`), so a pipeline needed now is compiled before background cache work.
- Stutter is avoided by design, not by scheduling: the compilation modes are `Synchronous`, `SynchronousUberShaders`, `AsynchronousUberShaders`, `AsynchronousSkipRendering` (`Source/Core/VideoCommon/VideoConfig.h:43-49`). Finished pipelines are collected once per frame (`ShaderCache::RetrieveAsyncShaders`, `ShaderCache.cpp:52-53` and `:105-108`).

##### Frame pacing

- The limiter is on the CPU thread and is driven by guest time. `CoreTimingManager::Throttle` (`CoreTiming.cpp:436-495`) maps the guest cycle to a host deadline and calls `SleepUntil(target_time)`. It is called at the end of each video field unless Immediate XFB is on (`Source/Core/Core/HW/VideoInterface.cpp:894-896`) and again just before each controller poll, "so user input is taken just before needed" (`VideoInterface.cpp:952-953`). Function names in `VideoInterface.cpp` were not read.
- Falling behind is forgiven, not chased: if the deadline is more than `MaxFallback` (100 ms, `MainSettings.cpp:52`) in the past, the reference time is moved (`CoreTiming.cpp:478-490`, log text "Core can not keep up with timings!").
- Sleep precision: `CoreTimingManager::SleepUntil` (`CoreTiming.cpp:410-434`) uses `std::this_thread::sleep_until`, or with "precision frame timing" a `PrecisionTimer`. `PrecisionTimer::SleepUntil` (`Source/Core/Common/Timer.cpp:149-188`) sleeps to about 1 ms before the deadline and then spins with `std::this_thread::yield()` on non Windows systems.
- Precision frame timing is off by default on Android: "Currently disabled by default on Android for concern of increased power usage while on battery" (`MainSettings.cpp:234-240`).
- Presentation is paced separately on the GPU thread: `Presenter::Present` sleeps until the intended present time and then calls `g_gfx->PresentBackbuffer()` (`Source/Core/VideoCommon/Present.cpp:957-973`). `Presenter::GetUpdatedPresentationTime` (`Present.cpp:989-1015`) presents immediately when Immediate XFB or Rush Frame Presentation is on, and with Smooth Early Presentation it filters the offset ("Adjust slowly backward in time but quickly forward in time").
- VSync is off by default (`GFX_VSYNC`, `GraphicsSettings.cpp:16`) and is disabled whenever the speed is not 100 percent (`IsVSyncActive`, `VideoConfig.cpp:43-48`). In the Vulkan swap chain, vsync selects `VK_PRESENT_MODE_FIFO_KHR`, otherwise `IMMEDIATE` is preferred, then `MAILBOX` (`Source/Core/VideoBackends/Vulkan/VKSwapChain.cpp:246-271`, function name not read).
- Audio follows emulation: `Mixer::MixerFifo::Mix` scales the input sample rate by the configured emulation speed unless pitch is preserved (`Source/Core/AudioCommon/Mixer.cpp:64-76`), with a default 80 ms buffer and gap filling on (`MainSettings.cpp:77-78`).
- No Swappy and no Choreographer (see next heading for the search terms).

##### Android performance APIs

- Manifest: `android:isGame="true"` (`Source/Android/app/src/main/AndroidManifest.xml:46`) and `<profileable android:shell="true">` (`:52-54`). There is no `android:appCategory` and no game mode configuration file (`Source/Android/app/src/main/res/xml/` contains only `backup_rules.xml` and `backup_rules_api_31.xml`).
- Nothing else. Searched `Source/` and the top level `CMakeLists.txt` for: `APerformanceHint`, `PerformanceHint`, `AThermal`, `thermal` (only guest CPU thermal registers and an arcade card reader matched), `Swappy`, `swappy`, `GameMode`, `game_mode`, `appCategory`, `cpu_capacity`, `cpuinfo_max_freq`, `Choreographer`, `setFrameRate`, `ANativeWindow_setFrameRate`, `sustainedPerformance`, `PowerManager`, `preferredDisplayModeId`, `preferredRefreshRate`, `games-frame-pacing`, `androidx.games`, `sched_setaffinity`, `setpriority`. No hits.

---

#### 2. PPSSPP

Repository: https://github.com/hrydgard/ppsspp
Commit read: `cc2aab3ceb6cffaba5c0b3d04fd736514987906d`, committed 2026-11-12T23:24:05+01:00.

##### Guest CPUs/threads to host threads

- The guest has one CPU and all guest threads run on one host thread, the emu thread. The frame function ends up in `PSP_RunLoopWhileState` (`UI/EmuScreen.cpp:1953`, `Core/System.cpp:1085-1095`), then `Core_RunLoopUntil` and `mipsr4k.RunLoopUntil(globalticks)` (`Core/Core.cpp:408-438`).
- On Android, Java starts one native thread per surface: `g_renderLoopThread = std::thread(RenderLoopThread, wnd, graphicsContext);` (`android/jni/app-android.cpp:1779`, `Java_org_ppsspp_ppsspp_PpssppActivity_runRenderLoop`). `RenderLoopThread` (`app-android.cpp:1797-1857`) builds the per frame callback (`NativeFrame`, then `perfHint.ReportFrame()`, `:1829-1836`) and calls `RunGraphicsLoop`.
- `RunGraphicsLoop` (`Core/EmuThread.cpp:132-181`) picks the arrangement. With Vulkan, the calling thread is the emu thread: "The backend spawns its own render thread, so this thread is simply where emulation runs" and `SetCurrentThreadName("EmuThread")` (`EmuThread.cpp:133-138`). With OpenGL the calling thread becomes `"RenderThread"` and a separate emu thread is spawned (`EmuThread.cpp:156-176`, `EmuThreadFunc` at `:42-77`).
- Vulkan helper threads are created in `VulkanRenderManager::StartThreads` (`Common/GPU/Vulkan/VulkanRenderManager.cpp:466-485`): a submission thread `"VulkanRenderMan"` if multithreading is on (`RenderThreadFunc`, `:877-910`), a pipeline scheduling thread `"ShaderCompile"` always (`CompileThreadFunc`, `:802-875`), and optionally `"PresentWait"` (`PresentWaitThreadFunc`, `:912-933`).
- The submission thread is on by default (`MultiThreading` default `true`, `Core/Config.cpp:1053`) unless the frame buffer depth is 1: `if (g_Config.iInflightFrames == 1) useMultiThreading = false;` (`Common/GPU/Vulkan/VulkanGraphicsContext.cpp:261-266`). `InflightFrames` defaults to 2 (`Core/Config.cpp:1046`), maximum 3 (`Common/GPU/Vulkan/VulkanContext.h:445`).
- The emu thread waits for the submission thread on a condition variable, not a spin: `while (!frameData.readyForFence) frameData.fenceCondVar.wait(lock);` (`VulkanRenderManager.cpp:977-985`, `BeginFrame`). The submission thread itself waits on `pushCondVar_` (`:885-888`).
- A shared worker pool exists beside these: `ThreadManager::Init` (`Common/Thread/ThreadManager.cpp:205-225`) creates `min(32, cores * logical)` compute threads plus `min(32, max(4, compute) * 2)` I/O threads. It is initialised with the detected core count (`UI/NativeApp.cpp:625`), so an 8 core phone gets 8 compute and 16 I/O workers.

##### Priorities and nice values

- None for any host thread. `Common/Thread/ThreadUtil.h:9-29` offers naming and JNI attach helpers only. Pool workers only set a name, `"PoolW %d"` or `"PoolW IO %d"` (`WorkerThreadFunc`, `ThreadManager.cpp:119-126`).
- `TaskPriority { HIGH, NORMAL, LOW }` (`Common/Thread/ThreadManager.h:14-20`) is queue order inside the pool (`ThreadManager.cpp:148-166`), not an OS priority.
- Searched `Common Core GPU UI android Windows SDL ios headless Qt` (C++, Objective C, Java, Kotlin) for: `setpriority`, `pthread_setschedparam`, `SetThreadPriority`, `SetPriorityClass`, `SCHED_FIFO`, `SCHED_RR`, `SCHED_BATCH`, `SCHED_IDLE`, `nice(`, `setThreadPriority`, `THREAD_PRIORITY_`, `QOS_CLASS`, `pthread_set_qos`. The only host side hit is an unrelated iOS dispatch queue (`ios/ExternalDisplaySceneDelegate.mm:145`, `QOS_CLASS_UTILITY`). All other hits are guest (PSP kernel) thread priorities.

##### Core placement

- Nothing is done, on any platform. Same search for `sched_setaffinity`, `pthread_setaffinity`, `SetThreadAffinityMask`: no hits.
- Core types are not detected. On Linux and Android the core count comes from `/sys/devices/system/cpu/present` (`GetCoreCount`, `Common/ArmCPUDetect.cpp:211-242`). `cpu_capacity` and `cpuinfo_max_freq`: no hits.
- Placement is instead influenced through ADPF, see "Android performance APIs".

##### Shader and pipeline compile threads

- One scheduling thread plus the shared compute pool. `CompileThreadFunc` (`VulkanRenderManager.cpp:802-875`) drains the compile queue, groups pipelines by vertex and fragment shader pair ("Those with the same pairs of shaders should be on the same thread, at least on NVIDIA", `:827-848`) and enqueues one `CreateMultiPipelinesTask` per group on the global pool (`:856-857`). It then sleeps 1 ms "to allow bunches of pipelines to collect" (`:869-870`).
- Count: up to the number of compute workers, which is the core count (see above). There is no cap that reserves cores for the emu thread.
- Priority: OS priority unchanged. Inside the pool the task declares `TaskType::CPU_COMPUTE` and `TaskPriority::HIGH` (`VulkanRenderManager.cpp:278-284`), so pipeline creation goes ahead of other pool work.
- Affinity: none.

##### Frame pacing

- Design note at the top of `Core/FrameTiming.cpp:1-20`: wait before the frame starts, run CPU, submit GPU commands, wait for the right time to present, present. "To minimize latency, we should *maximize* 1 and *minimize* 5".
- The limiter is `DoFrameTiming` (`Core/HLE/sceDisplay.cpp:562-629`). The next frame time advances by a fixed step but may not lag more than 5.5 frames: `nextFrameTime = std::max(lastFrameTime + scaledTimestep, time_now_d() - maxFallBehindFrames * scaledTimestep);` (`:579-581`).
- The sleep is deferred until after the frame has been handed to the render thread, "for some more overlap": `g_frameTiming.DeferWaitUntil(nextFrameTime, &curFrameTime);` (`sceDisplay.cpp:616-619`), executed by `g_frameTiming.PostSubmit()` between `EndFrame` and present (`UI/NativeApp.cpp:1488-1492`; `FrameTiming::PostSubmit`, `Core/FrameTiming.cpp:70-79`).
- Sleep precision: `WaitUntil` calls `sleep_precise` (`Core/FrameTiming.cpp:43-47`). On Android and Linux that is a plain `usleep(seconds * 1000000.0)` with no spin (`Common/TimeUtil.cpp:331-388`, the non Windows branch at `:385-386`). The sleep then spin loop exists only for Windows.
- Frame skipping: fixed (`FrameSkip`, default 0) or automatic when behind (`AutoFrameSkip`, default off) (`Core/Config.cpp:966-968`; logic at `sceDisplay.cpp:589-605`).
- Present mode: `FrameTiming::ComputePresentMode` (`Core/FrameTiming.cpp:81-142`). VSync off selects IMMEDIATE if supported. "Low latency present" (default off, `Core/Config.cpp:1031`) selects MAILBOX if supported. Otherwise FIFO, with flips skipped during fast forward.
- Display refresh rate comes from Java, `display.getRefreshRate()` passed to `NativeApp.setDisplayParameters` (`android/src/org/ppsspp/ppsspp/NativeSurfaceManager.java:76`, stored at `android/jni/app-android.cpp:1626-1641`), and is used to drop flips above the display rate (`sceDisplay.cpp:864-882`).
- The app asks Android for a 60 Hz display mode: `applyFrameRate(mSurface, 60.0f);` just before the render loop starts (`android/src/org/ppsspp/ppsspp/PpssppActivity.java:1083`). `applyFrameRate` (`PpssppActivity.java:921-950`) calls `surface.setFrameRate(...)` with a compatibility mode chosen by the `AndroidFramerateMode` setting (default 1, `Core/Config.cpp:944`).
- No Choreographer and no Swappy: searched `android Common Core GPU UI Windows` and the build files (`android/build.gradle.kts`, `android/jni/Android.mk`, `CMakeLists.txt`) for `Choreographer`, `Swappy`, `swappy`, `games-frame-pacing`, `androidx.games`. No hits.

##### Android performance APIs

- ADPF performance hints, on the emu thread only. `android/jni/PerformanceHint.h:24-27` states the reason: "Emulation does a burst of work each frame and then waits, so the scheduler sees a mostly idle thread and moves it to a slow core at low clocks, where the next burst then runs."
- Implementation (`android/jni/PerformanceHint.cpp`): the API is resolved with `dlsym` from `libandroid.so` because the build targets API 21 and the functions are API 33 (`HintApi::Load`, `:33-50`). The session is created lazily for the calling thread only: `const int32_t tid = gettid(); session_ = ... api->createSession(manager, &tid, 1, TARGET_WORK_NS)` (`PerformanceHintSession::ReportFrame`, `:77-92`).
- The target is half a 60 Hz frame, `TARGET_WORK_NS = 1000000000LL / 60 / 2`, "this leaves the other half as headroom, so the scheduler keeps us on a core fast enough to finish well before the frame is due" (`PerformanceHint.cpp:29-31`).
- The reported duration is thread CPU time, not wall time: `clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)` so that "sleeping in the frame limiter or blocking on vsync or the GPU doesn't count as work" (`ThreadCpuTimeNs`, `:59-67`; reported at `:93-97`). The render thread and pool workers are not in the session.
- Sustained performance mode (Android 7 and later): detected with `powerManager.isSustainedPerformanceModeSupported()` (`PpssppActivity.java:434-440`) and applied with `getWindow().setSustainedPerformanceMode(enable != 0)` (`updateSustainedPerformanceMode`, `PpssppActivity.java:577-588`). It is a per game setting, default off (`Core/Config.cpp:1011`; `app-android.cpp:611-612`).
- Manifest: `android:appCategory="game"` and `android:isGame="true"` (`android/AndroidManifest.xml:50-51`), plus `<meta-data android:name="isGame" android:value="true" />` (`:106`).
- Not present: thermal APIs, game mode configuration, Swappy. Searched for `AThermal`, `thermal`, `Thermal`, `GameMode`, `game_mode`, `Swappy`, `ANativeWindow_setFrameRate`, `WakeLock`, `sched_setaffinity`, `setpriority`, `cpu_capacity`, `cpuinfo_max_freq`. `GameMode` matches only PSP ad hoc networking code. `PowerManager` is otherwise used only to observe battery saver mode (`android/src/org/ppsspp/ppsspp/PowerSaveModeReceiver.java:26`).

---

#### 3. PCSX2 (upstream) and its Android port ARMSX2

Repository: https://github.com/PCSX2/pcsx2
Commit read: `3ff6b1bf18bdb607944585b4f3717dc625d565a1`, committed 2026-11-12T11:30:12-05:00.

Android port: https://github.com/ARMSX2/ARMSX2
Commit read: `37ed35b88fe9a0164369c8042f2c9aa707e33667`, committed 2026-11-12T11:58:44-08:00.

Upstream PCSX2 has no Android code: a search of `common`, `pcsx2`, `pcsx2-qt`, `cmake` and `CMakeLists.txt` for `__ANDROID__`, `ANDROID`, `APerformanceHint`, `Swappy`, `AThermal`, `Choreographer` returns no files. Everything Android specific below is from ARMSX2 and is labelled so. Paths are the same in both trees unless a line says "ARMSX2".

##### Guest CPUs/threads to host threads

- Upstream uses up to three emulation threads. The EE (main CPU, also running the IOP and, without MTVU, the vector units) runs on the thread that boots the VM: `s_vm_thread_handle = Threading::ThreadHandle::GetForCallingThread();` (`pcsx2/VMManager.cpp:1379`, enclosing function not read).
- The GS thread (MTGS) is started by `s_thread.Start(&MTGS::ThreadEntryPoint);` (`pcsx2/MTGS.cpp:126`) and named `"GS"` (`MTGS::ThreadEntryPoint`, `MTGS.cpp:144-146`). Its loop sleeps in `s_sem_event.WaitForWork()` (`MTGS::MainLoop`, `MTGS.cpp:314-340`).
- The optional VU1 thread (MTVU) is started at `pcsx2/MTVU.cpp:100-101` and named `"MTVU"` (`VU_Thread::ExecuteRingBuffer`, `MTVU.cpp:128-138`), sleeping in `semaEvent.WaitForWork()`. MTVU is enabled by default only with three or more cores: `if (core_count >= 3) ... si.SetBoolValue("EmuCore/Speedhacks", "vuThread", true);` (`VMManager::SetHardwareDependentDefaultSettings`, `VMManager.cpp:3639-3666`).
- Waiting between threads uses `Threading::WorkSema`, an atomic state machine over a kernel semaphore with states `STATE_SPINNING`, `STATE_SLEEPING`, `STATE_RUNNING_0` (`common/Threading.h:151-212`). `NotifyOfWork()` only makes a system call if the worker is asleep.
- MTGS and MTVU use the variant without spinning (`WaitForWork`, `MTGS.cpp:338`, `MTVU.cpp:134`). The EE waits for the GS with `s_sem_event.WaitForEmpty()` (`MTGS::WaitGS`, `MTGS.cpp:613-659`) and for VU1 with `semaEvent.WaitForEmpty()` (`VU_Thread::WaitVU`, `MTVU.cpp:435-439`).
- When the GS ring buffer is full, `MTGS::GenericStall` (`MTGS.cpp:705-792`) sleeps the EE on a semaphore for large packets and spins for small ones: "Sleeping the EEcore is a waste of time, and we get better results using a spinwait" (`MTGS.cpp:743-745`), using `Threading::SpinWait()` (`:777-779`), which is `pause` on x86 and `isb` on ARM64 (`common/Linux/LnxThreads.cpp:40-49`).
- Bounded spinning is reserved for the software renderer workers: `m_sema.WaitForWorkWithSpin();` (`pcsx2/GS/GSJobQueue.h:35`, `ThreadProc`). The spin budget is 50 microseconds, `return 50 * 1000; // 50µs`, overridable with the `WAIT_SPIN_MICROSECONDS` environment variable (`GetSpinTime`, `common/HostSys.cpp:101-113`), after which the thread sleeps (`WorkSema::WaitForWorkWithSpin`, `common/Semaphore.cpp:63-92`).
- ARMSX2 adds more threads: a "GS back thread" (GS multi-threading) and a separate IOP thread, both visible in its affinity code (ARMSX2 `pcsx2/VMManager.cpp:5410-5419`). Their implementation was not read.

##### Priorities and nice values

- Upstream: none for the EE, GS, MTVU or software renderer threads. `Threading::ThreadHandle` offers `SetAffinity` only (`common/Threading.h:76`). Searched `common pcsx2 pcsx2-qt pcsx2-gsrunner` for `setpriority`, `pthread_setschedparam`, `SetThreadPriority`, `SetPriorityClass`, `SCHED_FIFO`, `SCHED_RR`, `SCHED_BATCH`, `SCHED_IDLE`, `nice(`, `QOS_CLASS`, `pthread_set_qos`, `thread_policy_set`, `ThreadPriority`.
- The only hit is the network adapter receive thread: `SetThreadPriority(rx_thread.native_handle(), THREAD_PRIORITY_HIGHEST);` on Windows, and on POSIX `param.sched_priority = sched_get_priority_max(policy); pthread_setschedparam(...)` with the thread's existing policy (`pcsx2/DEV9/net.cpp:107-119`). From memory, not verified: under the default `SCHED_OTHER` policy the maximum priority is 0, so the POSIX branch changes nothing.
- macOS: no QoS classes. A case insensitive search of `common/Darwin` for `qos`, `pthread_set_`, `thread_policy`, `priority` returns nothing. The task brief expected QoS use in `DarwinThreads.cpp`; it is not there at this commit.
- Windows only timer resolution: `VMManager::SetTimerResolutionIncreased` calls `timeBeginPeriod(1)` while the VM runs and is an empty function elsewhere (`VMManager.cpp:3557-3577`, called from `:275`).
- ARMSX2 adds a nice helper: `Threading::ThreadHandle::SetNicePriority` (ARMSX2 `common/Linux/LnxThreads.cpp:194-208`):
  ```cpp
  // PRIO_PROCESS + a tid sets the nice value of that specific thread on Linux.
  if (setpriority(PRIO_PROCESS, static_cast<id_t>(m_native_id), nice) == 0)
  ```
  The function treats `EPERM` as tolerable, because the process limit may forbid negative nice values (comment at `:200`).
- ARMSX2 uses it to lower background work: nice 5 for shader and pipeline compile workers (ARMSX2 `pcsx2/GS/GSCompileQueue.cpp:166`), nice 5 for a cache file thread (`pcsx2/GS/GSCacheFile.cpp:1019`) and texture replacement loading (`pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp:1740`), and nice 10 for the background VU recompiler, "Below the emulator's threads: a tier-up may wait, the VU must not" (`pcsx2/arm64/vuaot/VuAotBackground.cpp:262-264`, `Worker::Loop`).
- ARMSX2 also contains a raise to nice -1 for the EE, MTVU and GS threads (ARMSX2 `pcsx2/VMManager.cpp:5630-5638`). As read, that block sits inside the `#else` branch (`:5493` to `:5659`) of the function's top level `#if defined(__ANDROID__)` (`:5339`), and the Android branch returns before it (`:5492`). So it does not appear to be compiled on Android. Treat "ARMSX2 raises emulation threads to nice -1" as unconfirmed.
- A comment in ARMSX2 `pcsx2/SPU2/spu2_neon.cpp:121` mentions `SCHED_FIFO` and a fixed prime core. Its context was not checked.

##### Core placement

- Upstream has opt-in pinning, off by default (`"EnableThreadPinning", false`, `pcsx2-qt/Settings/EmulationSettingsWidget.cpp:45`), implemented only for Linux and Windows (`#if defined(__linux__) || defined(_WIN32)`, `VMManager.cpp:3585`).
- Core types are detected with the `cpuinfo` library. `InitializeProcessorList` (`VMManager.cpp:3598-3637`) drops SMT siblings and sorts by frequency: "Prioritize faster cores in heterogeneous CPUs", `return (lhs->core->frequency > rhs->core->frequency);` (`:3621-3625`).
- `VMManager::SetEmuThreadAffinities` (`VMManager.cpp:3725-3797`) gives one core each, fastest first: EE gets `s_processor_list[0]`, VU the next, GS the third (or the second without MTVU) (`:3754-3777`). Software renderer workers are pinned only to further cores in the same cluster as the GS thread, otherwise "let the OS schedule them instead" (`:3779-3796`; applied in `GSRasterizerList::OnWorkerStartup`, `pcsx2/GS/Renderers/SW/GSRasterizer.cpp:1524-1536`).
- Linux implementation: `sched_setaffinity((pid_t)m_native_id, sizeof(set), &set)` on the thread id, and a zero mask means all processors (`Threading::ThreadHandle::SetAffinity`, `common/Linux/LnxThreads.cpp:155-184`). macOS returns false, "Doesn't appear to be possible to set affinity" (`common/Darwin/DarwinThreads.cpp:189-193`). On macOS the performance levels (`hw.nperflevels`, `common/Darwin/DarwinMisc.cpp:240-268`) are used only to choose defaults (`VMManager.cpp:3673-3704`).
- ARMSX2 rejects per core pinning on Android and explains why (ARMSX2 `pcsx2/VMManager.cpp:5340-5348`, `VMManager::SetEmuThreadAffinities` at `:5325`): "never hard-pin the emu threads to fixed cores. Under Android's EAS scheduler the busiest thread ... is placed on the highest-capacity core", "measured: VU pinned to an A7xx runs ~1.4x slower than floating to the X3", and "cpuinfo also can't read per-core frequency on many Android devices (all clusters report 0 MHz)".
- ARMSX2 default is mode 7, "Performance Cores" (`int g_android_affinity_mode = 7;`, `:5185`; set from JNI and clamped to 0 to 7 in `platforms/android/app/src/main/cpp/native-lib.cpp:3672-3676`). It gives the EE, GS, MTVU and IOP threads one shared mask covering the whole fast tier and lets the scheduler place them inside it (`:5392-5433`). Modes 1 to 6 are per core rank orders and are described as experimental (`:5350-5353`, `:5450-5487`). Mode 0 clears all masks (`:5359-5370`).
- ARMSX2 guard against making things worse: "Confining N busy threads to fewer than N processors is strictly worse than leaving them alone", so if the tier is narrower than the thread count it pins nothing (`:5395-5403`, `:5436-5447`).
- ARMSX2 core detection order: `cpu_capacity`, then `cpufreq/cpuinfo_max_freq`, then cpuinfo's frequency, and an all zero result means "do not confine anything" (`GetProcessorWeights`, `:4758-4791`). The rule that turns capacities into a tier (`IopThread::PerformanceMask`, declared at `pcsx2/IopThread/IopPlacement.h:45`) was not read.
- ARMSX2 also pins the Oboe audio callback thread to the same performance mask with `sched_setaffinity` (`pcsx2/Host/OboeAudioStream.cpp:96-112`), and has an optional "prime core" policy that gives the single fastest core to the busiest thread (`:5421-5430`, details not read).

##### Shader and pipeline compile threads

- Upstream compiles Vulkan pipelines synchronously on the GS thread at first use: `GSDeviceVK::GetTFXPipeline` looks up a map and on a miss calls `CreateTFXPipeline(p)` (`pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp:5224-5231`, creation at `:5089-5214`). A driver pipeline cache is passed to each creation (`g_vulkan_shader_cache->GetPipelineCache(true)`, `:5214`).
- No compile worker threads upstream: searching `pcsx2/GS/Renderers/Vulkan`, `pcsx2/GS/Renderers/Common` and `pcsx2/GS/GS.cpp` for `std::thread`, `Threading::Thread`, `std::async`, `WorkerThread`, `ThreadPool`, `async` finds only screenshot saving threads (`pcsx2/GS/Renderers/Common/GSRenderer.cpp:41`).
- Software renderer workers upstream: default 2 extra threads, or 3 with more than three cores (`VMManager.cpp:3663-3665`), created by `GSRasterizerList::Create` (`GSRasterizer.cpp:1602-1613`), named `"GS-SW-%d"`, no priority change, affinity as described above.
- ARMSX2 adds a background compile queue. Worker count: `std::clamp(hw / 2 - 1, 1u, 4u)` where `hw` is `hardware_concurrency()` (`GSCompileQueue::GetDefaultWorkerCount`, ARMSX2 `pcsx2/GS/GSCompileQueue.cpp:35-39`), which is 3 on an 8 core phone.
- Each ARMSX2 worker undoes the affinity it inherited and lowers its priority (`GSCompileQueue::WorkerThread`, `GSCompileQueue.cpp:159-180`):
  ```cpp
  // A thread inherits its creator's affinity, and the GS thread may be pinned to one core.
  self.SetAffinity(0);
  self.SetNicePriority(5);
  ```
- The ARMSX2 background VU compiler does the same with nice 10 and resets its mask to the process mask, because otherwise "the worker would compete with the emulation threads" (`VuAotBackground.cpp:264-273`).

##### Frame pacing

- Upstream limiter: `VMManager::Internal::Throttle` (`VMManager.cpp:2313-2354`), called twice per guest frame from `pcsx2/Counters.cpp:497` (`Throttle(true)`) and `:572` (`Throttle(false)`); function names in `Counters.cpp` were not read. It sleeps whole milliseconds and then busy waits: `Threading::Sleep(msec - 1);` followed by `while (GetCPUTicks() < uExpectedEnd) { }` (`:2338-2347`).
- If a frame overran by a whole period or more, the schedule is moved forward by whole frames, "Prevents fast forward zoomies" (`:2324-2330`).
- Sync to host refresh rate: `VMManager::UpdateTargetSpeed` (`VMManager.cpp:2246-2296`) adjusts the target speed when the display rate is within 5 percent of the guest rate (`host_to_guest_ratio >= 0.95f && host_to_guest_ratio <= 1.05f`, `:2263`), and can then use vsync itself as the pacer instead of sleeping (`s_use_vsync_for_timing`, `:2266-2267`; `Throttle` returns early at `:2315`).
- Present mode: `VMManager::GetEffectiveVSyncMode` (`VMManager.cpp:2827-2847`) returns `Disabled` without vsync, `FIFO` when synced to the host rate, otherwise `Mailbox` ("to avoid dropping frames").
- ARMSX2 on Android asks for a display rate with `holder.surface.setFrameRate(requestedRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT, Surface.CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS)` (ARMSX2 `platforms/android/app/src/main/java/com/armsx2/runtime/EmulationSurface.kt:166-184`). No Choreographer and no Swappy (search terms under the next heading).

##### Android performance APIs

- Upstream PCSX2: none (no Android code, see the top of this section).
- ARMSX2, ADPF: an optional performance hint session covering three threads. Declared at ARMSX2 `pcsx2/PerformanceMetrics.h:79-87`: "register the calling thread as perf-critical so the OS ramps its CPU/GPU clocks toward the frame deadline ... Called on the EE (CPU), GS and MTVU threads." Registration calls are at `pcsx2/VMManager.cpp:499`, `pcsx2/MTGS.cpp:152` and `pcsx2/MTVU.cpp:136`.
- It is off by default: `static bool s_adpf_enabled = false; // experimental, opt-in` (ARMSX2 `pcsx2/PerformanceMetrics.cpp:161`), toggled from the app through JNI (`native-lib.cpp:388-396`).
- The target is the full frame period at the current speed, `1.0e9 / fps` (`AdpfTargetNs`, `PerformanceMetrics.cpp:168-175`), updated with `APerformanceHint_updateTargetWorkDuration` when it changes (`:628-633`). The work reported is the time between limiter waits, summed over the two waits per frame (`AdpfEndWorkSegment`, `:596-627`). Outliers above four times the target are dropped (`:634-637`). Symbols are resolved with `dlsym` (`:147-148`).
- ARMSX2 sustained performance mode: `window.setSustainedPerformanceMode(true)` (ARMSX2 `platforms/android/app/src/main/java/com/armsx2/runtime/MainActivityRuntime.kt:2890`; conditions around the call not read).
- ARMSX2 manifest: `android:isGame="true"` and `android:appCategory="game"` (ARMSX2 `platforms/android/app/src/main/AndroidManifest.xml:78-79`).
- ARMSX2, not present: searched `platforms`, `pcsx2`, `common` (excluding `3rdparty`) for `Choreographer`, `Swappy`, `swappy`, `AThermal`, `getThermalHeadroom`, `game_mode_config`. No hits.
- AetherSX2 and NetherSX2 are closed source. No public material about them was opened in this session, so nothing about their thread handling is verified here. From memory, not verified and not source: AetherSX2 was a closed Android port of PCSX2 that is no longer developed, and NetherSX2 is a community patch of its last build.


---

#### What an Android Xenia fork can take from this

Context: six guest CPU host threads, one GPU command thread and four Vulkan pipeline compile threads on a 2 prime + 6 performance core phone, all at nice 0, with a guest thread measured at 6 s running against 15 s waiting in the run queue.

- **Lower the compile threads, do not raise the emulation threads.** The one project here that tackled the same problem on Android, ARMSX2, puts background compile work at nice 5 (pipeline and shader workers) and nice 10 (background recompiler) with `setpriority(PRIO_PROCESS, tid, nice)`, and clears the affinity those workers inherit from their creator (`GSCompileQueue::WorkerThread`, `VuAotBackground.cpp` `Worker::Loop`). Positive nice needs no privilege. Its nice -1 raise for emulation threads does not appear to be compiled on Android, and Dolphin, PPSSPP and upstream PCSX2 set no priorities at all, so there is no precedent here for negative nice values.
- **Cap the number of compile threads by what is left over.** Dolphin defaults to one run time compile thread, its automatic setting is `clamp(cores - 3, 1, 4)` with only two busy emulation threads, and it uses a larger pool only while it blocks before the game starts. ARMSX2 uses `clamp(cores / 2 - 1, 1, 4)`. With seven busy emulation threads on eight cores, four compile threads at equal priority is more than either rule would allow. A large pool is reasonable for a blocking precompile phase, a small low priority pool for run time.
- **Order the compile queue so that what is needed now goes first.** Dolphin's queue priorities (on demand 100, ubershader 200, cache 300) and PPSSPP's `TaskPriority::HIGH` for pipeline tasks are queue ordering, not scheduler priority. PPSSPP also batches pipelines that share a shader pair onto one worker. Combined with low nice values this keeps the pipeline the GPU thread is waiting for from sitting behind speculative work.
- **Prefer a shared tier mask to per core pinning, and only when it cannot hurt.** ARMSX2 documents a measured 1.4x slowdown from pinning a busy thread to a middle core and defaults to confining emulation threads to the fast tier as a group, detected from `cpu_capacity`, then `cpuinfo_max_freq`, and never when the tier has fewer cores than threads. Upstream PCSX2 pins one thread per core by frequency but leaves it off by default, and Dolphin and PPSSPP do nothing. On a phone with no small cores a tier mask covers every core, so affinity has little to offer for the emulation threads there. Priority and ADPF are the tools that apply.
- **Use an ADPF hint session for the threads that must meet the frame deadline.** PPSSPP's version is small and on by default: one session for the emulation thread, target half of a 60 Hz frame, and the reported duration is thread CPU time so limiter sleep does not count. ARMSX2's version registers several threads (EE, GS, MTVU) in one session, targets the full frame period and drops outliers above four times the target, but ships it off by default as experimental. For Xenia the session would hold the six guest thread ids and the GPU command thread, not the compile threads. Both resolve the API with `dlsym` so the build can target older API levels.
- **Pace with a sleep based limiter with bounded catch up, and tell Android the frame rate.** None of the projects uses Swappy or Choreographer. Each paces on the emulation thread against guest time and gives up on lost time beyond a bound (Dolphin 100 ms, PPSSPP 5.5 frames, PCSX2 whole frames). PPSSPP defers the limiter sleep until after the frame has been handed to the render thread so the two overlap. PPSSPP and ARMSX2 call `Surface.setFrameRate`. Sleep then spin precision exists in Dolphin and PCSX2, but Dolphin disables it on Android for battery reasons and PPSSPP uses a plain `usleep` there. With nearly every core busy, blocking waits (condition variables, semaphores) are the norm in all three, and spinning is kept short and bounded (PCSX2: 50 microseconds, worker pools only).

## Appendix C: Eden (Switch), FEX, Winlator

As written by the research pass that read the source, unedited but for heading levels. Paths are relative to each repository at the commit named. "(session scratch)" was the session's working directory, which is not kept: the citations name the repository, commit and path, which is enough to find each one.

### Host thread priority, core placement and frame pacing: Eden (Switch), FEX-Emu, Winlator

Survey date: 2026-11-13. Every claim below comes from source opened in this session, in shallow clones under `(session scratch)/src/`. Citations are `path:line` relative to each repository root, with the function name. Anything marked "inference" is my reading of what the cited code implies, not something the code states. Anything marked "not checked" was not opened.

#### 1. Eden (Nintendo Switch emulator, yuzu descendant, Android build)

Repository: https://github.com/eden-emulator/mirror (the GitHub mirror that the project itself lists at `docs/user/ThirdParty.md:14`; the origin https://git.eden-emu.dev/eden-emu/eden was refused by this session's proxy with HTTP 403).
Commit read: `e9661b2a017c4b349de04764df296de12bc38942`, committed 2026-11-12T03:56:33+01:00.
Why Eden: it was first in the requested order, the mirror cloned, and the head commit is one day old.

##### Guest CPUs/threads to host threads

- Four emulated cores: `constexpr u32 NUM_CPU_CORES = 4;` (`src/core/hardware_properties.h:18`).
- One host thread per emulated core in multicore mode, one host thread in total in single-core mode. `CpuManager::Initialize` (`src/core/cpu_manager.cpp:26-33`):
  ```cpp
  num_cores = is_multicore ? Core::Hardware::NUM_CPU_CORES : 1;
  gpu_barrier.emplace(num_cores + 1);
  for (std::size_t core = 0; core < num_cores; core++)
      core_data[core].host_thread = std::jthread([this, core](std::stop_token token) {
  ```
  The mode comes from the `use_multi_core` setting (`src/core/core.cpp:120`, `System::Impl` setup).
- Each core thread names itself, raises its priority, asks for performance cores and then becomes a fiber host. `CpuManager::RunThread` (`src/core/cpu_manager.cpp:171-201`):
  ```cpp
  Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
  Common::SetCurrentThreadToPerformanceCores();
  auto& data = core_data[core];
  data.host_context = Common::Fiber::ThreadToFiber();
  ```
- Guest threads are fibers, not host threads. Each `KThread` owns a `Common::Fiber` (`src/core/hle/kernel/k_thread.cpp:274`, `m_host_context = std::make_shared<Common::Fiber>(...)`). Fibers are boost.context fcontexts: `Fiber::YieldTo` (`src/common/fiber.cpp:89-93`) calls `boost::context::detail::jump_fcontext(to.impl->context, &to)`. So a guest thread switch on an emulated core is a user-space stack switch on the same host thread, with no kernel involvement.
- Guest execution loop per core: `CpuManager::MultiCoreRunGuestThread` (`src/core/cpu_manager.cpp:73-86`) runs `physical_core->RunThread(kernel, thread)` until the core is interrupted, then `HandleInterrupt`.
- An idle emulated core blocks rather than spins: `CpuManager::MultiCoreRunIdleThread` (`src/core/cpu_manager.cpp:88-100`) calls `PhysicalCore::Idle`, which is `m_on_interrupt.wait(lk, [this] { return m_is_interrupted; });` (`src/core/hle/kernel/physical_core.cpp:224-227`).
- Single-core mode time-slices the four emulated cores on one host thread: `CpuManager::PreemptSingleCore` (`src/core/cpu_manager.cpp:146`) does `current_core.store((current_core + 1) % Core::Hardware::NUM_CPU_CORES);`.
- The HLE kernel scheduler decides which guest thread each emulated core runs. `KScheduler::UpdateHighestPriorityThreadsImpl` (`src/core/hle/kernel/k_scheduler.cpp:209-300`) takes `priority_queue.GetScheduledFront(core_id)` for each of the four cores (line 222), and for idle cores migrates a "suggested" thread from another core (lines 249-275, comment "Idle cores are bad"). Migration is blocked for very high guest priorities: `HighestCoreMigrationAllowedPriority = 2` (`src/core/hle/kernel/global_scheduler_context.h:28`).
- Guest priority and core masks are honoured inside that queue only. `Svc::SetThreadPriority` (`src/core/hle/kernel/svc/svc_thread.cpp:182`) and `Svc::SetThreadCoreMask` (`svc_thread.cpp:252-279`, ending in `thread->SetCoreMask(system.Kernel(), core_id, affinity_mask)`, implemented at `src/core/hle/kernel/k_thread.cpp:620`) feed `GetPriorityQueue(kernel).ChangePriority(...)` (`k_scheduler.cpp:560`) and `ChangeAffinityMask` (`k_scheduler.cpp:573`). Nothing maps a guest thread's priority to a host priority: every call site of `SetCurrentThreadPriority` is listed in the next section and none is in the kernel scheduler. A guest core mask selects an emulated core, hence a host thread, but all four host core threads have identical host priority and placement.
- Guest time slicing: a 10 ms looping CoreTiming event (`KernelCore::Impl::InitializePreemption`, `src/core/hle/kernel/kernel.cpp:282-293`) calls `GlobalSchedulerContext::PreemptThreads` (`src/core/hle/kernel/global_scheduler_context.cpp:41-53`), which rotates the queue at guest priority 59 on cores 0 to 2 and 63 on core 3.
- HLE services run on extra plain host threads: `kernel.RunOnHostCoreProcess(...)` (`src/core/hle/service/services.cpp:98-99`), created by `RunHostThreadFunc` (`src/core/hle/kernel/kernel.cpp:1069-1086`), which sets a name and no priority.

##### Priorities and nice values (which threads, which values, which API)

- Five levels: `enum class ThreadPriority : u32 { Low = 0, Normal = 1, High = 2, VeryHigh = 3, Critical = 4 }` (`src/common/thread.h:94-100`).
- On Android the implementation is per-thread nice through `setpriority`, not a real-time class. `Common::SetCurrentThreadPriority` (`src/common/thread.cpp:478-486`):
  ```cpp
  const int nice_value = NiceValueForPriority(new_priority);
  const pid_t tid = gettid();
  if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice_value) != 0) {
  ```
- The nice table (`src/common/thread.cpp:49-53`, used by `NiceValueForPriority` at lines 69-81):
  ```cpp
  constexpr int NICE_AUDIO = -16;          // Critical
  constexpr int NICE_URGENT_DISPLAY = -8;  // VeryHigh
  constexpr int NICE_DISPLAY = -4;         // High
  constexpr int NICE_DEFAULT = 0;          // Normal
  constexpr int NICE_BACKGROUND = 10;      // Low
  ```
  (the trailing comments are mine, from the `switch` at lines 71-77). The names match Android's `Process.THREAD_PRIORITY_*` constants (that correspondence is from memory, not verified in this source).
- Negative values are clamped to what the process may request: `LowestAllowedNice` (`thread.cpp:55-67`) reads `getrlimit(RLIMIT_NICE, ...)` and `NiceValueForPriority` returns `(std::max)(wanted, (std::min)(NICE_DEFAULT, LowestAllowedNice()))` (line 80). Whether a stock Android app actually gets -16 is not determined by this source.
- Desktop Linux uses the same nice table with `setpriority(PRIO_PROCESS, 0, nice_value)` (`thread.cpp:487-492`). Other POSIX systems use `pthread_setschedparam(pthread_self(), SCHED_OTHER, &params)` scaled between min and max (`thread.cpp:494-502`). Windows uses `SetThreadPriority` with `THREAD_PRIORITY_BELOW_NORMAL` up to `THREAD_PRIORITY_TIME_CRITICAL` (lines 454-464).
- Every call site found (search: `SetCurrentThreadPriority`, `ThreadPriority::` over `src`):

  | Level (Android nice) | Thread | Site |
  |---|---|---|
  | Critical (-16) | `CPUCore_N` / `CPUThread` guest core threads | `CpuManager::RunThread`, `src/core/cpu_manager.cpp:176` |
  | Critical (-16) | `GPU` command thread | `ThreadManager::StartThread`, `src/video_core/gpu_thread.cpp:32` |
  | Critical (-16) | `VulkanWorker` (runs the Vulkan scheduler work queue) | `Scheduler::WorkerThread`, `src/video_core/renderer_vulkan/vk_scheduler.cpp:272` |
  | VeryHigh (-8) | `VSyncThread` | `Conductor::VsyncThread`, `src/core/hle/service/vi/conductor.cpp:88` |
  | VeryHigh (-8) | `HostTiming` (CoreTiming event thread) | `CoreTiming::Initialize`, `src/core/core_timing.cpp:76` |
  | High (-4) | `VulkanPresent` | `PresentManager::PresentThread`, `src/video_core/renderer_vulkan/vk_present_manager.cpp:332` |
  | High (-4) | `GPUFencingThread` | `FenceManager::ReleaseThreadFunc`, `src/video_core/fence_manager.h:197` |
  | High (-4) | host1x `CDmaPusher` thread | `CDmaPusher::CDmaPusher`, `src/video_core/cdma_pusher.cpp:27` |
  | High (-4) | `AudioRenderSystemManager`, `DSP_AudioRenderer_Main` | `src/audio_core/renderer/system_manager.cpp:31`, `src/audio_core/adsp/apps/audio_renderer/audio_renderer.cpp:134` |
  | Low (+10) | `TimeWorker` | `TimeWorker::ThreadFunc`, `src/core/hle/service/glue/time/worker.cpp:130` |
  | Low (+10) | any `ThreadWorker` with non-default placement: `VkPipelineBuilder`, `VkPipelineSerialization`, `TextureDecoder` | `StatefulThreadWorker` constructor, `src/common/thread_worker.h:45-47` |

  Everything else (HLE service threads, `ImageTranscode` workers, the Android UI thread) stays at nice 0.
- Eden remembers each thread's nice and re-applies it. `RememberCurrentThreadNice` (`thread.cpp:429-436`) stores it per tid; `Common::RefreshThreadPolicies` (`thread.cpp:576-597`) loops over the registry calling `setpriority` and `sched_setaffinity` again. It is called from Kotlin on every resume: `NativeLibrary.refreshThreadPolicies()` in `EmulationFragment.onResume` (`src/android/app/src/main/java/org/yuzu/yuzu_emu/fragments/EmulationFragment.kt:1544`) through `Java_org_yuzu_yuzu_1emu_NativeLibrary_refreshThreadPolicies` (`src/android/app/src/main/jni/native.cpp:1331-1333`). Inference: this exists because Android can reset thread scheduling state when the app moves between cgroups.
- No Kotlin-side priority calls: `Process.setThreadPriority`, `THREAD_PRIORITY`, `setSustainedPerformanceMode` have no hits under `src/android`.

##### Core placement (pinning to big cores, avoiding small ones, how core types are detected)

Eden does pin, on Android only, and it is new code (file header "Copyright 2026 Eden Emulator Project").

- Detection, `ComputeTopologyLocked` (`src/common/thread.cpp:240-308`): start from the process mask (`sched_getaffinity(getpid(), ...)`, line 247); read `/sys/devices/system/cpu/cpuN/cpu_capacity` for every allowed CPU (line 254, via `ReadCpuScalar` at 188-195); fall back to `cpufreq/cpuinfo_max_freq` if capacity is unreadable, or if capacity is uniform while the MIDR values differ (lines 255-260; MIDR from `regs/identification/midr_el1`, `ReadCpuMidr` at 197-205).
- Classification (`thread.cpp:283-293`, constants at lines 97-100):
  ```cpp
  const bool fast_enough =
      core.weight * 100 >= fastest * ANDROID_PERFORMANCE_CAPACITY_PERCENT;   // 50
  if (!fast_enough && taken >= ANDROID_MINIMUM_PERFORMANCE_CORES) {          // 4
      break;
  }
  CPU_SET(core.cpu, &state.performance);
  ```
  Cores are sorted fastest first; a core is "performance" if it has at least 50% of the fastest core's weight, and at least four cores are always taken. Everything else allowed is "efficiency". If no efficiency core remains, `state.separated` is false (line 304) and all placement requests become no-ops (`ApplyCoreGroupLocked`, line 333). Symmetric CPUs also disable placement (lines 266-274). Inference for our target: on a 2 prime + 6 performance Snapdragon, if the six performance cores report at least half the prime capacity, all eight land in the performance set and Eden's affinity logic does nothing. It only bites on SoCs with true little cores.
- Application: `ApplyCoreGroupLocked` (`thread.cpp:331-351`) calls `sched_setaffinity(tid, sizeof(*mask), mask)` with the whole group mask. Threads are confined to a cluster set, never to a single core.
- ADPF takes precedence over affinity. `Common::SetCurrentThreadToPerformanceCores` (`thread.cpp:542-550`):
  ```cpp
  if (ADPF::AddCurrentThread(ADPF::Session::Render)) {
      SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
      return;
  }
  SetCurrentThreadCoreGroup(CoreGroup::Performance);
  ```
  If the thread can join the ADPF render session, Eden leaves its affinity unrestricted and lets the platform place it; the hard pin is the fallback for devices without a working hint session. `SetCurrentThreadToEfficiencyCores` (lines 552-560) does the same with the background session, falling back to the efficiency mask. `SetCurrentThreadToBackgroundWork` (lines 562-567) joins the background session and never restricts affinity.
- Who asks for performance cores: guest core threads (`cpu_manager.cpp:177`), `VulkanWorker` (`vk_scheduler.cpp:273`), `VulkanPresent` (`vk_present_manager.cpp:333`), `GPUFencingThread` (`fence_manager.h:198`), `VSyncThread` (`conductor.cpp:89`), `HostTiming` (`core_timing.cpp:77`). The `GPU` command thread sets Critical priority but makes no placement call (`gpu_thread.cpp:30-33`). Who asks for efficiency cores: `TextureDecoder` (`src/video_core/texture_cache/texture_cache_base.h:517-518`).
- The main thread is never placed: `SetCurrentThreadCoreGroup` refuses with "Refusing to place the main thread: the CPU topology is read from it" (`thread.cpp:411-415`).
- Affinity is defended against the kernel or vendor daemons resetting it. `Common::PollThreadPolicies` (`thread.cpp:599-630`) runs only when `KernelPreservesRequestedAffinity()` is false, that is kernel older than 6.2 (lines 353-364); at most every 500 ms (`ANDROID_POLICY_POLL_INTERVAL`, line 102) it compares one "canary" thread's mask with the expected one and calls `RefreshThreadPolicies` if they differ. It is driven from the vsync path (`Conductor::ProcessVsync`, `conductor.cpp:77`).

##### Shader and pipeline compile threads (count, priority, affinity)

- Count, `GetTotalPipelineWorkers` (`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:306-320`):
  ```cpp
  const int configured = AndroidSettings::values.pipeline_worker_count.GetValue();
  const int clamped = std::clamp(configured, 2, 8);
  ...
  return std::min(max_core_threads, desired);
  #else
  return max_core_threads;
  ```
  with `max_core_threads = max(hardware_concurrency, 2) - 1`. Desktop uses all cores minus one. Android uses a user setting clamped to 2 to 8 whose default is 2 (`src/android/app/src/main/jni/android_settings.h:150`, `pipeline_worker_count{linkage, 2, ...}`). The setting's own description warns that higher values raise temperature (`src/android/app/src/main/res/values/strings.xml:75`).
- Construction (`PipelineCache::PipelineCache`, `vk_pipeline_cache.cpp:356-359`):
  ```cpp
  workers(device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers(),
          "VkPipelineBuilder", {}, Common::ThreadPlacement::Background),
  serialization_thread(1, "VkPipelineSerialization", {},
                       Common::ThreadPlacement::Background) {
  ```
  `has_broken_parallel_compiling` is declared (`src/video_core/vulkan_common/vulkan_device.h:1232`) but a search of `src/video_core` found no assignment, so the single-worker branch appears unreachable at this commit.
- Priority and placement of those workers, `StatefulThreadWorker` constructor (`src/common/thread_worker.h:45-58`):
  ```cpp
  if (placement != ThreadPlacement::Default) {
      Common::SetCurrentThreadPriority(ThreadPriority::Low);
  }
  ```
  then `SetCurrentThreadToBackgroundWork()` for `Background`. Net effect on Android: nice +10, member of the ADPF background session (power-efficiency preferred, see below), affinity unrestricted. So compile threads may still use big cores when those are free, but lose to the nice -16 guest core threads whenever they compete.
- The same worker pool builds pipelines at run time and when loading the disk cache: `worker_thread->QueueWork(std::move(func))` in the `GraphicsPipeline` constructor (`src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp:348-349`), selected by `build_in_parallel ? &workers : nullptr` (`vk_pipeline_cache.cpp:927`); disk cache load queues at `vk_pipeline_cache.cpp:650` and `:696` (`PipelineCache::LoadDiskResources`).
- Asynchronous shaders: with `use_asynchronous_shaders` (`vk_pipeline_cache.cpp:354`), `PipelineCache::BuiltPipeline` (`vk_pipeline_cache.cpp:787-823`) returns `nullptr` for a not yet built pipeline, which skips the draw instead of blocking the GPU thread. It still blocks for small render targets (under 640x360), draws with side effects (transform feedback, rasteriser off), and draws with 6 or fewer indices or vertices.
- Other pools: `ImageTranscode` uses `max(hardware_concurrency, 2) / 2` workers with default placement, so nice 0 and all cores (`src/video_core/textures/workers.cpp:11-12`); `TextureDecoder` is one worker at nice +10 on efficiency cores (`texture_cache_base.h:517-518`).

##### Frame pacing (limiter, vsync, Swappy, Choreographer)

- Guest vsync is generated by emulator timing, not by the Android display. `Conductor::Conductor` (`src/core/hle/service/vi/conductor.cpp:31-41`) schedules a looping CoreTiming event `"ScreenComposition"` at `FrameNs` (1/60 s, line 19) which sets a signal; `Conductor::VsyncThread` (lines 86-100) wakes, and `Conductor::ProcessVsync` (lines 76-84) composes and signals the guest vsync event.
- The speed limit in multicore mode is applied by stretching that period. `Conductor::GetNextTicks` (`conductor.cpp:102-126`): `speed_scale = 100.f / Settings::SpeedLimit();`, or `0.01f` when the limiter is off ("Run at unlocked framerate"), then `speed_scale * (1000000000.f / effective_fps)` with `effective_fps = 60 / swap_interval`.
- The CoreTiming thread sleeps until the next event with `event.WaitFor(std::chrono::nanoseconds(wait_time))` (`CoreTiming::Initialize`, `src/core/core_timing.cpp:85-90`).
- `SpeedLimiter::DoSpeedLimiting` (`src/core/perf_stats.cpp:137-169`) is single-core only: it returns immediately when `use_multi_core` is set (lines 138-141). Otherwise it sleeps the accumulated error, clamped to 25 ms per speed scale. Call site: `nvdisp_disp0::Composite` (`src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp:92`).
- Presentation runs on its own thread: `PresentManager::PresentThread` (`vk_present_manager.cpp:330-356`) pops a frame and calls `CopyToSwapchain(frame)`, so a slow `vkQueuePresentKHR` does not block the GPU thread.
- Present mode, `ChooseSwapPresentMode` (`src/video_core/renderer_vulkan/vk_swapchain.cpp:44-77`): default setting is FIFO (`src/common/settings.h:412-413`); when the speed limiter is off or in turbo, FIFO is replaced by mailbox, else immediate (lines 58-66). Triple buffering is requested where possible (lines 268-279).
- Optional fixed-rate pacing: `Swapchain::AcquireNextImage` (`vk_swapchain.cpp:182-210`) can wait to a 30, 60, 90 or 120 FPS grid via `Scheduler::Wait(tick, target_fps)` (`src/video_core/renderer_vulkan/vk_scheduler.h:133-158`: sleep to within 1 ms, then `std::this_thread::yield()` spin). On Android API 30 and later this is bypassed:
  ```cpp
  if (android_get_device_api_level() >= 30) {
      scheduler.Wait(resource_ticks[image_index]);
  } else {
      wait_with_frame_pacing();
  ```
- On Android the display is told the content rate instead. `EmuWindow_Android::UpdateFrameRateHint` (`src/android/app/src/main/jni/emu_window/emu_window.cpp:177-254`) resolves `ANativeWindow_setFrameRateWithChangeStrategy` (fallback `ANativeWindow_setFrameRate`) with `dlsym` and calls it with a smoothed observed present rate (EMA factor 0.15, `UpdateObservedFrameRate`, lines 78-98), quantised to 0.5 FPS (`QuantizeFrameRateHint`, lines 100-109), changed only after 3 consistent votes and 0.15 to 0.40 s of stability (lines 209-216), with `ChangeFrameRateOnlyIfSeamless`. It runs once per presented frame (`OnFrameDisplayed`, lines 67-70, called from `src/video_core/renderer_vulkan/renderer_vulkan.cpp:217`).
- The launcher activity requests the highest refresh-rate display mode: `layoutParams.preferredDisplayModeId = maxRefreshRate.modeId` (`src/android/app/src/main/java/org/yuzu/yuzu_emu/ui/main/MainActivity.kt:95`).
- Swappy and Choreographer: not used. Searched `Swappy`, `Choreographer` in `src/android`, `src/common`, `src/core`, `src/video_core`: no hits.

##### Android performance APIs (ADPF `APerformanceHint`, game mode, thermal headroom)

- ADPF is used, loaded at run time so no API-level dependency: `dlopen("libandroid.so", RTLD_NOW)` and `dlsym` of `APerformanceHint_getManager`, `_createSession`, `_closeSession`, `_updateTargetWorkDuration`, `_reportActualWorkDuration`, `_setThreads`, `_setPreferPowerEfficiency` (`Resolve`, `src/common/adpf.cpp:52-71`).
- Two sessions: `enum class Session { Render, Background }` (`src/common/adpf.h:10-13`). `CreateSessionFor` (`adpf.cpp:124-145`) creates the render session with the current frame target and the background session with target 0 (falling back to 16.67 ms), then for background: `api.set_power_efficiency(handle, true);` (line 142). Thread membership is maintained with `APerformanceHint_setThreads` or by recreating the session (`SyncLocked`, lines 147-173; `AddCurrentThread`, lines 189-222).
- Target: `Conductor::ProcessVsync` calls `Common::ADPF::SetTargetWorkDuration(std::chrono::nanoseconds{this->GetFramePeriodNs()})` every vsync (`conductor.cpp:78`); `GetFramePeriodNs` (lines 128-147) is the speed-scaled frame period, divided by 4 when unlocked, clamped to 1 to 100 ms.
- Actual: `ADPF::ReportFrameInterval` (`adpf.cpp:253-279`) reports the wall-clock interval between successive composites on the calling thread, capped at 4 targets (`MAX_REPORTED_TARGETS`, line 104 and 271), from `nvdisp_disp0::Composite` (`nvdisp_disp0.cpp:91`). Note that it reports frame interval, not measured CPU work time.
- Game mode: `android:isGame="true"` and `android:appCategory="game"` (`src/android/app/src/main/AndroidManifest.xml:44-45`), plus `android.game_mode_config` (line 56) pointing at `res/xml/game_mode_config.xml`, which sets `supportsPerformanceGameMode="true"`, `supportsBatteryGameMode="false"`, `allowGameDownscaling="false"`, `allowGameFpsOverride="false"`. OEM booster opt-ins for Samsung, Xiaomi, Asus and OnePlus are at manifest lines 52-55.
- GPU clock: with the `force_max_clock` setting (default false, `src/common/settings.h:641`) the renderer calls `adrenotools_set_turbo(true)` (`src/video_core/renderer_vulkan/vk_turbo_mode.cpp:159`, gated at `renderer_vulkan.cpp:195`).
- Thermal: nothing beyond a battery temperature read for the overlay (`batteryIntent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0)`, `EmulationFragment.kt:1749`). No thermal headroom or thermal status API. Searched `AThermal`, `thermal`, `PowerManager`, `sustainedPerformance`, `GameManager`, `WakeLock`, `/sys/class/thermal`, `thermal_zone` in `src/android`, `src/common`, `src/core`, `src/video_core`: the only `thermal` hits are UI strings.
- Eden's CPU "tuning" settings change guest-visible time, not host scheduling. `CoreTiming::GetClockTicks` (`src/core/core_timing.cpp:226-238`) scales the guest counter by `target / CPU_CLOCK_BASE_MHZ` for the `cpu_clock` ("fast_cpu_time") setting and, with `sync_core_speed`, returns `u64(ticks / speed_limit)`. `GetNextTickCount` (`core_timing.cpp:205-210`) substitutes a fixed `cpu_ticks` value when `use_custom_cpu_ticks` is set.

##### What an Android Xenia fork can take from this

- Use per-thread nice through `setpriority(PRIO_PROCESS, gettid(), n)` with a small fixed ladder. Eden's ladder is guest CPU, GPU command and Vulkan submit threads at -16, timing and vsync at -8, present and audio at -4, compile and decode workers at +10 (`src/common/thread.cpp:49-53`). This directly addresses guest threads queuing behind pipeline compiles at nice 0.
- Keep the Vulkan pipeline compile pool small on Android: Eden defaults to 2 workers, user range 2 to 8, against cores minus one on desktop (`vk_pipeline_cache.cpp:306-320`), and puts them at nice +10 in a power-efficient ADPF session rather than pinning them.
- Prefer ADPF sessions to hard affinity, with affinity as the fallback: a render session for guest CPU, submit and present threads with a per-frame target, and a separate background session with `setPreferPowerEfficiency(true)` (`src/common/thread.cpp:542-567`, `src/common/adpf.cpp:124-145`). Resolve the symbols with `dlsym` so old API levels still load.
- If affinity is used, classify by `cpu_capacity` with a `cpuinfo_max_freq` fallback, keep at least four cores in the fast set, pin to the set and never to single cores, and never pin the main thread (`thread.cpp:240-308`, `411-415`). Expect this to be a no-op on a 2 + 6 SoC without little cores (inference above).
- Re-apply nice and affinity on `onResume`, and on kernels before 6.2 poll a canary thread about twice a second (`thread.cpp:576-630`, `EmulationFragment.kt:1544`).
- Declare game mode support and pass the measured content frame rate to `ANativeWindow_setFrameRateWithChangeStrategy` (seamless only, debounced) instead of adding Swappy (`emu_window.cpp:177-254`, `AndroidManifest.xml:44-56`).

#### 2. FEX-Emu (x86 and x86-64 user-mode emulator for ARM64 Linux)

Repository: https://github.com/FEX-Emu/FEX
Commit read: `ab135df091d005cc98dfddc56c3dc9c9e4bd501d`, committed 2026-11-11T20:20:16-05:00.

##### Guest CPUs/threads to host threads

- Strictly 1:1. A guest `clone` with `CLONE_THREAD` (`HandleNewClone`, `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/Thread.cpp:179-186`) goes to `CreateNewThread` (`Thread.cpp:90-122`):
  ```cpp
  NewThread->ExecutionThread = FEXCore::Threads::Thread::Create(ThreadHandler, &Arg);
  ```
  which ends in `pthread_create(&Thread, &Attr, InitializeThread, this);` with a minimum pthread stack that is then pivoted to a FEX-allocated stack (`PThread` constructor path, `Source/Tools/LinuxEmulation/LinuxSyscalls/Utils/Threads.cpp:205-224`).
- The new host thread records its own kernel TID as the guest's TID and enters the JIT: `Thread->ThreadInfo.TID = FHU::Syscalls::gettid();` then `CTX->ExecuteThread(Thread->Thread);` (`ThreadHandler`, `Thread.cpp:60-88`). Guest TID equals host TID, which is what makes the pass-through below meaningful. Without `CLONE_THREAD` the call is treated as a fork (`Thread.cpp:202`).
- There is no FEX scheduler, no fibers and no emulated core count: the host kernel schedules guest threads directly.

##### Priorities and nice values (which threads, which values, which API)

- Guest scheduling syscalls are passed to the host kernel unchanged. `RegisterCommon` (`Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/Passthrough.cpp:272-279`, `:296-297`, `:342-345`):
  ```cpp
  REGISTER_SYSCALL_IMPL(setpriority, SyscallPassthrough3<SYSCALL_DEF(setpriority)>);
  REGISTER_SYSCALL_IMPL(sched_setscheduler, SyscallPassthrough3<SYSCALL_DEF(sched_setscheduler)>);
  REGISTER_SYSCALL_IMPL(sched_setaffinity, SyscallPassthrough3<SYSCALL_DEF(sched_setaffinity)>);
  ```
  Also passed through in the same table: `getpriority`, `sched_setparam`, `sched_getparam`, `sched_getscheduler`, `sched_get_priority_max/min`, `sched_getaffinity`, `sched_yield` (line 216), `ioprio_set` (line 313), `getcpu` (line 342), `sched_setattr` and `sched_getattr` (lines 344-345). On ARM64 the pass-through is a bare `svc #0` with the remapped syscall number (`SyscallPassthrough3`, `Passthrough.cpp:63-75`). The 32-bit `nice` syscall calls host `::nice(inc)` (`Source/Tools/LinuxEmulation/LinuxSyscalls/x32/Thread.cpp:146-149`).
- So a guest program's own priority and affinity choices take effect on the host as written, and FEX adds none for guest threads.
- FEX sets a host priority in exactly two places, both to the lowest nice, both for its own background work:
  - threads created with the `LowPriority` flag: `setpriority(PRIO_PROCESS, FHU::Syscalls::gettid(), 19);` (`InitializeThread`, `Utils/Threads.cpp:327-329`). The only user of the flag is the disk cache writer, `FEXCore::Threads::Flags WriterThreadFlags = {.LowPriority = true, .Internal = true};` then `WorkQueueThread(WriterThreadFlags, "FEX:DiskCache")` (`DiskCache` setup, `FEXCore/Source/Interface/Core/DiskCache.cpp:481-484`). The Windows (WOW64/ARM64EC) build maps the same flag to `THREAD_BASE_PRIORITY_IDLE` through `NtSetInformationThread` (`Thread::RunThread`, `Source/Windows/Common/Threads.cpp:85-88`).
  - AOT compile threads: "Set the priority of the thread so it doesn't overwhelm the system when running in the background", `setpriority(PRIO_PROCESS, FHU::Syscalls::gettid(), 19);` (`AOTGenSection`, `Source/Tools/FEXInterpreter/AOT/AOTGenerator.cpp:110-111`). The file is built into the `FEX` binary (`Source/Tools/FEXInterpreter/CMakeLists.txt:9-11`) but a search for `AOTGenSection` found no caller outside its own two files, so it looks dormant.

##### Core placement (pinning to big cores, avoiding small ones, how core types are detected)

- FEX never sets host affinity itself. Searched `sched_setaffinity`, `pthread_setaffinity`, `affinity`, `pthread_setschedparam`, `SCHED_FIFO|RR|OTHER|BATCH|IDLE`, `sched_getcpu`, `SetThreadPriority`, `ThreadBasePriority` in `FEXCore` and `Source`: the only functional hits are the pass-through registrations above and the Windows idle priority.
- Reported CPU count is the number of `/sys/devices/system/cpu/cpuN` directories, deliberately ignoring the affinity mask: `CalculateNumberOfCPUs` (`Source/Common/CPUInfo.cpp:17-32`; header comment "regardless of affinity mask", `Source/Common/CPUInfo.h:8`). That count feeds the emulated `/proc/cpuinfo` (`EmulatedFDManager` constructor, `Source/Tools/LinuxEmulation/LinuxSyscalls/EmulatedFiles/EmulatedFiles.cpp:523-528`; `siblings` and `cpu cores` at lines 489-491) and the emulated `/sys/devices/system/cpu/online` and `/present`, written as `0-(N-1)` (`EmulatedFiles.cpp:580-600`).
- There is no option to report fewer cores or to hide small cores from scheduling. The full option list in `FEXCore/Source/Interface/Config/Config.json.in` has no entry for core or thread count (searched `"Cores"`, `"Threads"`, `NumCores`, `CPUCount`).
- What exists is a CPUID-level switch. `HideHybrid`, default `true` ("Hides hybrid CPU core arrangement", `Config.json.in:155-162`). `CPUIDEmu::SetupHostHybridFlag` (`FEXCore/Source/Interface/Core/CPUID.cpp:169-190`) sets `Hybrid = true` when the per-CPU MIDR values differ, then clears it if `HideHybrid()`. Only when the option is turned off does the guest see the hybrid bit (`((Hybrid ? 1U : 0U) << 15)`, `CPUID.cpp:793`) and a per-core type in leaf 1Ah: `Res.eax |= (Data.IsBig ? 0x40 : 0x20) << 24;` (`CPUIDEmu::Function_1Ah`, `CPUID.cpp:967-977`). Big or little is decided from a MIDR table (`struct CPUMIDR { ... bool DefaultBig; }`, `CPUID.cpp:192-195`, logic to line 421), not from capacity or frequency. This is information for the guest; it places nothing.

##### Shader and pipeline compile threads (count, priority, affinity)

FEX has no shaders; the equivalent is JIT code generation.

- Code is compiled on the thread that needs it, synchronously. `ContextImpl::CompileBlock` (`FEXCore/Source/Interface/Core/Core.cpp:857-928`) runs on `Frame->Thread`: lookup cache (`FindBlock`, line 880), then the disk cache (`DiskCache.Lookup`, line 891, with `LoadCachedCode` at 893), then `CompileCode(Thread, GuestRIP, MaxInst)` (line 928). There is no in-process background compile pool.
- Disk cache: option `DiskCache`, default `false` (`Config.json.in:180-186`). Its only extra thread is the nice 19 writer above.
- Offline code cache: option `EnableCodeCachingWIP`, default `false` (`Config.json.in:21-28`). `FEXServer` handles `TYPE_POPULATE_CODE_CACHE` (`HandleSocketData`, `Source/Tools/FEXServer/ProcessPipe.cpp:605-652`) by running `FEXOfflineCompiler generate <codemap>` as a child process: `RunOfflineCompiler` (`ProcessPipe.cpp:490-493`) through `EmbedSubprocess`, a plain `fork` and `execvp` followed by `waitpid` (`ProcessPipe.cpp:470-485`). No nice or affinity is applied to that child (searched `setpriority`, `nice` in `ProcessPipe.cpp`: no hits). The offline compiler compiles on one thread with `CTX->CompileRIP(Thread, ...)` (`Source/Tools/FEXOfflineCompiler/Main.cpp:475-535`).
- The dormant AOT generator would use one thread per CPU at nice 19 (`AOTGenerator.cpp:107-111`).
- TSO, briefly: `TSOEnabled` defaults to `true` ("Highly likely to break any multithreaded application if disabled", `Config.json.in:605-612`). Spin behaviour: not checked.

##### Frame pacing (limiter, vsync, Swappy, Choreographer)

None. FEX has no presentation path. Searched `vsync`, `swapinterval`, `Choreographer`, `Swappy` in `FEXCore` and `Source`: no relevant hits.

##### Android performance APIs (ADPF `APerformanceHint`, game mode, thermal headroom)

None found. Searched `APerformanceHint`, `PerformanceHint`, `AThermal`, `thermal`, `GameMode` in `FEXCore` and `Source` (`*.cpp`, `*.h`, `*.in`). The only files matching any term were `CPUID.cpp`, `CPUID.h`, `EmulatedFiles.cpp` and a v4l2 ioctl header; those matches were not opened (inference: CPUID thermal feature-bit names).

##### What an Android Xenia fork can take from this

- FEX's model is "one guest thread is one host thread, and the guest's own scheduling syscalls reach the host kernel" (`Passthrough.cpp:272-297`). The Xenia analogue is to translate the guest's `KeSetBasePriorityThread`, `KeSetAffinityThread` style calls into host nice or affinity rather than dropping them; FEX shows that passing guest intent through is enough for a well-regarded emulator, with no policy of its own.
- The one policy FEX does apply is the right one for our measured problem: background code generation and cache writing run at nice 19 (`Utils/Threads.cpp:327-329`, `AOTGenerator.cpp:110-111`). Compile work should never sit at the same nice as guest CPU threads.
- Report the real core count to the guest even when the process is restricted (`CPUInfo.cpp:17-32`), and hide core heterogeneity from the guest by default (`HideHybrid`, `Config.json.in:155-162`). Our six guest hardware threads should look identical to the title regardless of which host cluster runs them.
- Move compile cost off the play session where possible: a disk cache consulted before compiling (`Core.cpp:891-893`) and an out-of-process offline compiler fed by a recorded code map (`ProcessPipe.cpp:490-493`) are the JIT equivalents of a persistent Vulkan pipeline cache.

#### 3. Winlator (Wine with Box64 or FEXCore on Android)

Repository read for source: https://github.com/StevenMXZ/Winlator-Ludashi (README title "Winlator Bionic", Java package `com.winlator.cmod`). Commit read: `94a4009f599ad56b930f7380529e54362e372096`, committed 2026-11-11T21:08:20+00:00.

Why not the main repository: https://github.com/brunodev85/winlator at `30aafbdb9eea47b92669201621e03cd2cf04540d` (2026-11-02T14:20:47-03:00) holds no application source. It contains six C files (`glibc_patches`, `android_alsa`) plus component folders, and its README sends users to the release APK (`README.md:23`). The two forks named in the brief cloned but are stale: `coffincolors/winlator` head `49bfb90f` is dated 2025-08-10 and `longjunyu2/winlator` head `0556c80d` is dated 2025-08-14. Winlator-Ludashi was found through `gh` repository search, is two days old at head and has 342 Java files. Caveat: everything below describes this fork, which adds its own native X11 presenter (`cpp/ludashix11`); the closed-source upstream build may differ. All paths below are relative to `app/src/main/` unless they start with `README`.

##### Guest CPUs/threads to host threads

- The app does not create or schedule guest threads. It launches a Wine process tree under a translator: "`x86_64` containers using Box86/Box64, as well as `Arm64EC` containers which utilize FEXCore" (`README.md:11`). Windows threads become host threads inside Wine; that mapping is in Wine and the translators, whose sources are not in this repository (from memory, not verified here).
- The translators are configured only through environment variables: `BOX64_DYNAREC=1` and friends (`GuestProgramLauncherComponent`, `java/com/winlator/cmod/xenvironment/components/GuestProgramLauncherComponent.java:587-597`), Box64 presets (`java/com/winlator/cmod/box64/Box64PresetManager.java:48-98`, for example `BOX64_AVX`, `BOX64_MMAP32`), and FEX presets `FEX_TSOENABLED`, `FEX_VECTORTSOENABLED`, `FEX_MEMCPYSETTSOENABLED`, `FEX_HALFBARRIERTSOENABLED`, `FEX_X87REDUCEDPRECISION`, `FEX_MULTIBLOCK` (`java/com/winlator/cmod/fexcore/FEXCorePresetManager.java:34-55`). None of these sets a thread count, priority or affinity. Searched `BOX64_MAXCPU`: no hits.
- Wine threading defaults set by the app: `WINEESYNC=1` and `WINE_FAST_YIELD=1` unless the user overrides them (`XServerDisplayActivity.java:1319-1324`, in the environment setup; path `java/com/winlator/cmod/XServerDisplayActivity.java`).

##### Priorities and nice values (which threads, which values, which API)

- Nothing is done for the guest. Searched `renice`, `setpriority`, `nice(`, `pthread_setschedparam`, `SCHED_FIFO`, `taskset` in `java`, `cpp/winlator` and `cpp/ludashix11/src`: no hits.
- The only priority call in the app lowers its own installer thread: `android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);` (`ImageFsInstaller`, `java/com/winlator/cmod/xenvironment/ImageFsInstaller.java:167-169`).
- So Wine, Box64 or FEX, DXVK compile threads and the X server all run at the default nice 0.

##### Core placement (pinning to big cores, avoiding small ones, how core types are detected)

Winlator's mechanism is a user-chosen CPU list per container, applied as a Windows process affinity mask.

- Settings: `cpuList` and `cpuListWoW64` per container (`java/com/winlator/cmod/container/Container.java:67-68`, saved at 567-568), with a per-shortcut `cpuList` override (`XServerDisplayActivity.java:476-481`).
- Defaults are by CPU index, not by detection. `Container.getFallbackCPUList` (`Container.java:767-772`) is every CPU; `Container.getFallbackCPUListWoW64` (`Container.java:774-779`) is the upper half of the indices:
  ```java
  for (int i = numProcessors / 2; i < numProcessors; i++) cpuList += (!cpuList.isEmpty() ? "," : "")+i;
  ```
  Inference: this assumes the higher-numbered CPUs are the faster ones, which is common on ARM SoCs but is not checked.
- The lists become bit masks (`ProcessHelper.getAffinityMask`, `java/com/winlator/cmod/core/ProcessHelper.java:240-249`), stored as `taskAffinityMask` and `taskAffinityMaskWoW64` (`XServerDisplayActivity.java:473-481`).
- Application path 1, on every X window map: `onMapWindow` calls `assignTaskAffinity(window)` (`XServerDisplayActivity.java:637-639`), which picks the 32-bit or 64-bit mask and calls `winHandler.setProcessAffinity(processId, processAffinity)` or the by-name variant (`XServerDisplayActivity.java:3691-3703`). `WinHandler.setProcessAffinity` (`java/com/winlator/cmod/winhandler/WinHandler.java:201-225`) sends a datagram with `RequestCodes.SET_PROCESS_AFFINITY` to the helper running inside Wine, whose `handleSetAffinity` (`cpp/winlator/winhandler.c:212-242`, dispatched at 382-383) does:
  ```c
  HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
  if (hProcess) {
    SetProcessAffinityMask(hProcess, mask);
  ```
  The mask is therefore applied through the Win32 API and Wine translates it to the host. How Wine does that is not in this repository (not verified).
- Application path 2, a polling watcher: `winlauncher.exe /affinity <hex>` starts `affinityWatcherThread` (`cpp/winlator/winlauncher.c:11-33`, started at 161-164), which every 1000 ms walks the process list and calls `SetProcessAffinityMask` on every process with a pid above its own. The Java code that passes `/affinity` was not located.
- A direct host path exists but looks unused: `Java_com_winlator_cmod_core_ProcessHelper_nativeSetProcessAffinity` (`cpp/winlator/process_helper.c:9-23`) calls `sched_setaffinity((pid_t) pid, sizeof(cpuset), &cpuset)`, but a search for `nativeSetProcessAffinity` under `java` found no declaration or caller.
- The guest can be told the matching topology: with `syncCpuTopology`, the app sets `WINE_CPU_TOPOLOGY` to `coreCount + ":" + affinityCpuList` (`XServerDisplayActivity.java:484-491`, exported at 1315-1317).
- Core types are shown to the user, not used for placement. `CPUStatus.getMaxClockSpeed` (`java/com/winlator/cmod/core/CPUStatus.java:25-28`) reads `cpufreq/cpuinfo_max_freq`, and its callers are the task manager views (`java/com/winlator/cmod/winhandler/TaskManagerSidebar.java:247`, `TaskManagerDialog.java:193`) and `CPUStatus.java:69` (not opened). The task manager also lets the user re-pin a running process (`TaskManagerDialog.java:107-111`).

##### Shader and pipeline compile threads (count, priority, affinity)

- Compilation happens inside DXVK or VKD3D in the guest. The app only sets environment variables, in `DXVKConfigDialog.setEnvVars` (`java/com/winlator/cmod/contentdialog/DXVKConfigDialog.java:322-351`):
  ```java
  if (!async.isEmpty() && !async.equals("0"))
      envVars.put("DXVK_ASYNC", "1");
  ...
      envVars.put("DXVK_GPLASYNCCACHE", "1");
  ```
  plus `DXVK_STATE_CACHE_PATH` (line 351) and `VKD3D_FEATURE_LEVEL` (line 350).
- No compile thread count, priority or affinity is set. Searched `numCompilerThreads`, `VKD3D_WORKER`: no hits. Compile threads inherit the process affinity mask above and nice 0.

##### Frame pacing (limiter, vsync, Swappy, Choreographer)

- FPS limit is delegated to DXVK: `DXVK_FRAME_RATE` plus `dxgi.maxFrameRate` and `d3d9.maxFrameRate` in `DXVK_CONFIG` (`DXVKConfigDialog.setEnvVars`, `DXVKConfigDialog.java:325-331`, `347-348`).
- Present mode is passed to the Mesa WSI layer: `envVars.put("MESA_VK_WSI_PRESENT_MODE", presentMode);`, with `WRAPPER_MAX_IMAGE_COUNT=1` for immediate, and `MESA_VK_WSI_DEBUG=forcesync` when "syncFrame" is on (`XServerDisplayActivity.java:3152-3163`).
- Display rate: with the `high_refresh_rate_mode` preference the activity sets `params.preferredRefreshRate = pickHighestRefreshRate();` (`XServerDisplayActivity.java:327-331`), and the native X11 view votes with `surface.setFrameRate(requested, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);` (`java/com/winlator/cmod/widget/NativeX11View.java:258-262`).
- This fork paces presentation with Choreographer frame timelines. `DisplayTimeline33` implements `Choreographer.VsyncCallback` and forwards `frame.getPreferredFrameTimeline().getVsyncId()` to native code every vsync (`NativeX11View.java:184-196`); the native presenter resolves `ASurfaceTransaction_setFrameTimeline`, `ASurfaceTransaction_setDesiredPresentTime` and `ASurfaceTransaction_setEnableBackPressure` (`cpp/ludashix11/src/native_displayx_renderer.cpp:57-59`, `74-77`).
- Swappy: searched `Swappy`, no hits.

##### Android performance APIs (ADPF `APerformanceHint`, game mode, thermal headroom)

- ADPF is used only by this fork's presenter thread. Symbols are resolved with `dlsym` (`native_displayx_renderer.cpp:60-73`) and, when "performance mode" is on (`XServerDisplayActivity.java:581-582`, flag bit 2 at `cpp/ludashix11/src/native_jni.cpp:42`), `DisplayXNativeRenderer::Surface` creates a one-thread session (`native_displayx_renderer.cpp:344-349`):
  ```cpp
  const int32_t tid = syscall(SYS_gettid);
  if (auto *manager = api->hintManager())
      hint = api->createHint(manager, &tid, 1, int64_t(1'000'000'000. / refresh));
  ```
  `Draw` reports the measured work time (`api->hintWork(hint, ...)`, lines 474-477) and `Options` updates the target on a refresh change (lines 511-512). Inference: the game's own threads live in the Wine process tree and are not in this session.
- Game mode: `android:appCategory="game"` and `android:isGame="true"` (`AndroidManifest.xml:38-39`). No game mode config (searched `game_mode_config`, `GameManager`: no hits).
- Thermal: `CPUStatus.discoverCpuTempPaths` reads `/sys/class/thermal/thermal_zone*` for the HUD (`java/com/winlator/cmod/core/CPUStatus.java:99-108`). No thermal API (searched `AThermal`, `getThermalHeadroom`, `sustainedPerformance`: no hits).
- A partial wake lock keeps the session alive: `powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Winlator::KeepAlive")` (`java/com/winlator/cmod/services/NotificationService.java:74-76`).

##### What an Android Xenia fork can take from this

- Winlator shows the minimum that users accept: no priorities at all, and affinity left to the user with a per-game CPU list. A per-title "CPU list" override is cheap to add as a diagnostic control, but Eden's automatic ladder is the better default.
- Affinity set once does not stick, so Winlator re-applies it on every window map and from a one-second watcher (`XServerDisplayActivity.java:3691-3703`, `cpp/winlator/winlauncher.c:11-33`). Any pinning we add needs the same re-application.
- If the usable core set is restricted, tell the guest and size worker pools from the restricted set, as `WINE_CPU_TOPOLOGY` does (`XServerDisplayActivity.java:484-491`); do not size pipeline compile pools from `hardware_concurrency` alone.
- For presentation, the Ludashi presenter combines a Choreographer vsync id passed to `ASurfaceTransaction_setFrameTimeline` with a one-thread ADPF session that reports real work time (`NativeX11View.java:184-196`, `native_displayx_renderer.cpp:344-349`, `474-477`). This is a lighter alternative to Swappy for a Vulkan presenter thread.
- Stutter from pipeline creation is handled by letting draws proceed without the pipeline (`DXVK_ASYNC`) and by a persistent state cache (`DXVK_STATE_CACHE_PATH`), the same two ideas as Eden's asynchronous shaders and pipeline cache.

## Appendix D: Android's own guidance

As written by the research pass, unedited but for heading levels. The file prefixes (`g_`, `r3/`, `k_` and so on) name which mirror and tag a file was read from; the table at the top maps them. "(session scratch)" was the session's working directory, which is not kept.

### What Android offers an unprivileged game or emulator for scheduling, hints, pacing and thermals

Research notes, compiled 2026-11-12 and 2026-11-13. Every statement cites a file that was opened in this session. Anything not opened is labelled "from memory, not verified".

#### 0. Sources and how to read the citations

`android.googlesource.com`, `cs.android.com` and `source.android.com` were not reachable from this session. AOSP code was read through GitHub mirrors. Saved copies live in `(session scratch)/survey/src/` and `(session scratch)/survey/headers/`.

| Prefix or folder | Origin | What it represents |
|---|---|---|
| `headers/performance_hint.h`, `headers/thermal.h` | `GrapheneOS/platform_frameworks_native`, default branch, fetched 2026-11-12 | AOSP `frameworks/native/include/android/`. Git blobs `fe54314d` and `93ae9614`, byte-identical to the LineageOS copies (`headers/*.lineage.h`) and to more than twenty other forks found by code search, so unmodified AOSP content. |
| `g_` | GrapheneOS mirrors, default branch | Android 16 tree with `BUILD_ID=CP2A.260605.016` (`g_build_id.mk`). Called "CP2A head" below. Reading CP2A as Android 16 QPR2 is my inference, not verified. |
| `l_` | LineageOS mirrors, default branch | Same `BUILD_ID`. Used to confirm that a `g_` file is unmodified AOSP (same blob hash). |
| `r1/` | `aosp-mirror`, tag `android-16.0.0_r1` | First Android 16 release. |
| `r3/` | `aosp-mirror`, tag `android-16.0.0_r3` | Later Android 16 tag. `r3/ThermalManagerService.java` is a 404 placeholder: use `g_ThermalManagerService.java` or `r1/`. |
| `qpr1/` | `aosp-mirror`, branch `android16-qpr1-release` | Every file fetched has the same blob hash as `r3/`. |
| `a_` | `aosp-mirror`, default branch | Stale snapshot of unknown date (it has `BAKLAVA` in `Build.java`). Not used for claims. |
| `k_` | `torvalds/linux` master at 7.4-rc2 | Mainline kernel docs and code. |
| `ack_` | `aosp-mirror/kernel_common`, branch in the file name | Android common kernel (`android16-6.12`, `android15-6.6`, `android14-6.1`). |

`aosp-mirror` has no usable `platform_frameworks_native`, `platform_hardware_interfaces` or `platform_system_sepolicy` at these tags, so those three projects are cited from the `g_` and `l_` copies. Nothing newer than the CP2A head was visible: an API 37 addition, if any exists, is not covered.

#### 1. ADPF performance hint sessions (NDK)

##### 1.1 Prototypes

Copied from `headers/performance_hint.h` (AOSP `frameworks/native/include/android/performance_hint.h`, CP2A head). `__ANDROID_API_T__` is 33, `__ANDROID_API_U__` is 34 and `__ANDROID_API_V__` is 35 (`r3/api-level.h`, AOSP `bionic/libc/include/android/api-level.h`, tag `android-16.0.0_r3`). The export list in `g_libandroid.map.txt` (AOSP `frameworks/base/native/android/libandroid.map.txt`) gives the same levels for every symbol. `_Nonnull` and `_Nullable` are Clang annotations and can be dropped from function pointer types.

```c
/* API 33 */
APerformanceHintManager* _Nullable APerformanceHint_getManager();
APerformanceHintSession* _Nullable APerformanceHint_createSession(
        APerformanceHintManager* _Nonnull manager,
        const int32_t* _Nonnull threadIds, size_t size,
        int64_t initialTargetWorkDurationNanos);
int64_t APerformanceHint_getPreferredUpdateRateNanos(
        APerformanceHintManager* _Nonnull manager);          /* deprecated in 36 */
int APerformanceHint_updateTargetWorkDuration(
        APerformanceHintSession* _Nonnull session, int64_t targetDurationNanos);
int APerformanceHint_reportActualWorkDuration(
        APerformanceHintSession* _Nonnull session, int64_t actualDurationNanos);
void APerformanceHint_closeSession(APerformanceHintSession* _Nonnull session);

/* API 34 */
int APerformanceHint_setThreads(
        APerformanceHintSession* _Nonnull session,
        const pid_t* _Nonnull threadIds, size_t size);

/* API 35 */
int APerformanceHint_setPreferPowerEfficiency(
        APerformanceHintSession* _Nonnull session, bool enabled);
int APerformanceHint_reportActualWorkDuration2(
        APerformanceHintSession* _Nonnull session, AWorkDuration* _Nonnull workDuration);
AWorkDuration* _Nonnull AWorkDuration_create();
void AWorkDuration_release(AWorkDuration* _Nonnull aWorkDuration);
void AWorkDuration_setWorkPeriodStartTimestampNanos(AWorkDuration* _Nonnull aWorkDuration,
        int64_t workPeriodStartTimestampNanos);
void AWorkDuration_setActualTotalDurationNanos(AWorkDuration* _Nonnull aWorkDuration,
        int64_t actualTotalDurationNanos);
void AWorkDuration_setActualCpuDurationNanos(AWorkDuration* _Nonnull aWorkDuration,
        int64_t actualCpuDurationNanos);
void AWorkDuration_setActualGpuDurationNanos(AWorkDuration* _Nonnull aWorkDuration,
        int64_t actualGpuDurationNanos);

/* API 36 */
int APerformanceHint_createSessionUsingConfig(
        APerformanceHintManager* _Nonnull manager,
        ASessionCreationConfig* _Nonnull config,
        APerformanceHintSession * _Nullable * _Nonnull sessionOut);
int APerformanceHint_getMaxGraphicsPipelineThreadsCount(
        APerformanceHintManager* _Nonnull manager);
int APerformanceHint_notifyWorkloadIncrease(
        APerformanceHintSession* _Nonnull session,
        bool cpu, bool gpu, const char* _Nonnull identifier);
int APerformanceHint_notifyWorkloadReset(
        APerformanceHintSession* _Nonnull session,
        bool cpu, bool gpu, const char* _Nonnull identifier);
int APerformanceHint_notifyWorkloadSpike(
        APerformanceHintSession* _Nonnull session,
        bool cpu, bool gpu, const char* _Nonnull identifier);
int APerformanceHint_setNativeSurfaces(APerformanceHintSession* _Nonnull session,
        ANativeWindow* _Nonnull* _Nullable nativeWindows, size_t nativeWindowsSize,
        ASurfaceControl* _Nonnull* _Nullable surfaceControls, size_t surfaceControlsSize);
bool APerformanceHint_isFeatureSupported(APerformanceHintFeature feature);
APerformanceHintSession* _Nonnull APerformanceHint_borrowSessionFromJava(
        JNIEnv* _Nonnull env, jobject _Nonnull sessionObj);
ASessionCreationConfig* _Nonnull ASessionCreationConfig_create();
void ASessionCreationConfig_release(ASessionCreationConfig* _Nonnull config);
void ASessionCreationConfig_setTids(ASessionCreationConfig* _Nonnull config,
        const pid_t* _Nonnull tids, size_t size);
void ASessionCreationConfig_setTargetWorkDurationNanos(
        ASessionCreationConfig* _Nonnull config, int64_t targetWorkDurationNanos);
void ASessionCreationConfig_setPreferPowerEfficiency(
        ASessionCreationConfig* _Nonnull config, bool enabled);
void ASessionCreationConfig_setGraphicsPipeline(
        ASessionCreationConfig* _Nonnull config, bool enabled);
void ASessionCreationConfig_setNativeSurfaces(
        ASessionCreationConfig* _Nonnull config,
        ANativeWindow* _Nonnull* _Nullable nativeWindows, size_t nativeWindowsSize,
        ASurfaceControl* _Nonnull* _Nullable surfaceControls, size_t surfaceControlsSize);
void ASessionCreationConfig_setUseAutoTiming(
        ASessionCreationConfig* _Nonnull config, bool cpu, bool gpu);

/* API 36 enum, backed by int32_t, values 0 to 5 in this order */
typedef enum APerformanceHintFeature : int32_t {
    APERF_HINT_SESSIONS, APERF_HINT_POWER_EFFICIENCY, APERF_HINT_SURFACE_BINDING,
    APERF_HINT_GRAPHICS_PIPELINE, APERF_HINT_AUTO_CPU, APERF_HINT_AUTO_GPU,
} APerformanceHintFeature;
```

Documented return codes (same header): `updateTargetWorkDuration` gives `EINVAL` for a negative target and `EPIPE` on service failure. `reportActualWorkDuration` gives `EINVAL` when the duration or the session target is not positive. `createSessionUsingConfig` gives `EINVAL`, `EPIPE`, `ENOTSUP` (no hint sessions, or auto timing unsupported) or `EBUSY` (too many graphics pipeline threads, the session is still created). The three `notifyWorkload*` calls give `EBUSY` when rate limited. All values are positive errno numbers, not negated.

##### 1.2 Which threads, and what happens with a dead or foreign tid

- The header says the tids "must be part of this process' thread group" and that session threads "should be long-lived and not created or destroyed dynamically" (`headers/performance_hint.h`).
- The check is in `HintManagerService.checkTidValid` (`r3/HintManagerService.java`, AOSP `frameworks/base/services/core/java/com/android/server/power/hint/HintManagerService.java`, tag `android-16.0.0_r3`). It reads `Uid:` and `Tgid:` from `/proc/<tid>/status` and accepts a tid whose tgid is the caller's, or whose uid is the caller's uid, or which belongs to one of the caller's isolated processes. The first tid that fails makes the service throw `SecurityException`. A dead tid has no status file. `readProcLines` only fills its output when the file opens (`android_os_Process_readProcLines`, `r3/android_util_Process.cpp`), so uid and tgid stay 0, neither test matches and the tid is rejected. This is my reading of the code, not something I ran.
- `APerformanceHint_createSession` with a dead or foreign tid: the service throws, no session is created, and the documented result is `nullptr`. Caution: in Android 16 the helper declares `APerformanceHintSession* sessionOut;` without initialising it and returns it after a failed create (`r1/performance_hint.cpp`, `r3/performance_hint.cpp` and `g_performance_hint.cpp`, AOSP `frameworks/base/native/android/performance_hint.cpp`, function `APerformanceHintManager::createSession`). Whether the build zero-initialises that local is from memory, not verified. On API 36 and later prefer `APerformanceHint_createSessionUsingConfig`, which sets `*sessionOut = nullptr` first and returns an errno. In that function a `SecurityException` falls into the default branch and comes back as `EPIPE`.
- `APerformanceHint_setThreads` with a dead or foreign tid returns `EPERM` (`EX_SECURITY` mapped in `APerformanceHintSession::setThreads`, same files). An empty list returns `EINVAL`. The header also documents `EINVAL` for tids outside the thread group, but the service code read here throws `SecurityException` for that case, so expect `EPERM`. If the app is not in the foreground at the time, the service stores the list without checking it and applies it on resume (`AppHintSession.setThreadsInternal`, `r3/HintManagerService.java`).
- Threads that die later: when the uid leaves the foreground the service schedules a clean-up after 1000 ms (`CLEAN_UP_UID_DELAY_MILLIS`) that drops exited tids. If none remain the session is force-paused until `setThreads` is called again (`g_HintManagerService.java`, clean-up code above `checkTidValid`).

##### 1.3 What a session does underneath

- Framework side: the service forwards to the power HAL (`halCreateHintSessionWithConfig`, then `halCreateHintSession`). The HAL contract says of `reportActualWorkDuration`: "The system will attempt to adjust the core placement of the threads within the thread group and/or the frequency of the core on which they are run to bring the actual duration close to the target duration" (`g_IPowerHintSession.aidl`, AOSP `hardware/interfaces/power/aidl/android/hardware/power/IPowerHintSession.aidl`, CP2A head). The mechanism is left to the vendor.
- Pixel reference HAL: sessions vote a per-thread uclamp minimum (`mUclampMinInit`, `mUclampMinHigh`, `mUclampMinLow`) through a PID-style controller, and apply it with `sched_setattr` and `SCHED_FLAG_UTIL_CLAMP_MIN | SCHED_FLAG_UTIL_CLAMP_MAX` (`l_pixel_PowerHintSession.cpp` and `set_uclamp` in `l_pixel_TaskRampupMultNode.cpp`, which is a saved copy of `power-libperfmgr/aidl/PowerSessionManager.cpp` from `LineageOS/android_hardware_google_pixel`, default branch). Other vendors differ.
- Side effect on the threads: for each session thread that uses `SCHED_OTHER`, the service sets `SCHED_OTHER | SCHED_RESET_ON_FORK` and restores the nice value. This is behind the flag `resetOnForkEnabled()` in `r1/` and `r3/` and unconditional at the CP2A head. The kernel then refuses an unprivileged scheduler change that clears the flag: "Normal users shall not reset the sched_reset_on_fork flag" leads to `EPERM` (`user_check_sched_setscheduler` in `k_syscalls.c`, mainline `kernel/sched/syscalls.c`). So after a thread joins a session, `sched_setscheduler` or `sched_setattr` from the app must keep the reset-on-fork flag. `setpriority` is a different path and is not affected.
- Foreground only: `isUidForeground` is true when the uid's process state is at least as important as `PROCESS_STATE_IMPORTANT_FOREGROUND` (numerically `<=`). When the uid drops below that, each session is paused in the HAL and reports are ignored (`isHintAllowed`). It resumes automatically (`MyUidObserver` and `AppHintSession.updateHintAllowedByProcState`, `r3/HintManagerService.java`).
- Client-side throttling of reports: a report at or over target is sent at once. Reports under target are batched and sent at most once per preferred update rate (`reportActualWorkDurationInternal`, `r3/performance_hint.cpp`). The header says client-side rate limiting is not needed.
- Load hints: 20 per 2 seconds per process (`kMaxLoadHintsPerInterval`, `kLoadHintInterval`), a hint for CPU and GPU counts as two, over budget returns `EBUSY`, unsupported hints are dropped silently.
- Graphics pipeline sessions: at most 5 threads per app in total (`MAX_GRAPHICS_PIPELINE_THREADS_COUNT`). Auto timing requires graphics pipeline mode and an attached surface.
- Games: the service retags an app session as `GAME` when the package has `ApplicationInfo.CATEGORY_GAME` (`updateSessionTag`). At the CP2A head `APerformanceHint_reportActualWorkDuration2` starts with `if (session->getTag() == hal::SessionTag::GAME) { ... return 0; }` and so reports nothing for such a session, while `APerformanceHint_reportActualWorkDuration` has no such test (`g_performance_hint.cpp`, identical blob in `l_performance_hint.cpp`). The test is absent in `r1/` and `r3/`. I do not know the reason. An app declared `android:appCategory="game"` should check on a device that the durations arrive, or use the single-duration call.

##### 1.4 Target, actual and the reporting pattern

- The API is "intended for periodic workloads, such as frame production". Set a target, then "report the actual work duration at the end of each cycle". The framework compares the two and tries "to help the client reach a steady state under the target" (header group comment).
- All times come from `CLOCK_MONOTONIC`. In `AWorkDuration` the start timestamp and the total must be greater than zero. CPU and GPU durations must be zero or more and at least one must be positive. Zero means "not measured" (header).
- A target of 0 disables duration reporting. The session can still carry modes and load hints (header, `updateTargetWorkDuration`).
- Session methods are not thread safe (header).

##### 1.5 Failure modes

- `APerformanceHint_getManager` returns `nullptr` when the `performance_hint` service is missing or `registerClient` fails (`APerformanceHintManager::create`, `r3/performance_hint.cpp`).
- With no HAL support the service throws `UnsupportedOperationException("PowerHintSessions are not supported!")` when the HAL preferred rate is -1, so creation fails (`ENOTSUP` from the config call).
- On API 36 and later ask `APerformanceHint_isFeatureSupported(APERF_HINT_SESSIONS)`. Before that the header's deprecation note describes using `getPreferredUpdateRateNanos` as the old support check. That -1 means unsupported is from the Java documentation, from memory, not verified here.

##### 1.6 Java equivalent

`android.os.PerformanceHintManager` has `getPreferredUpdateRateNanos()` and `createHintSession(int[] tids, long initialTargetWorkDurationNanos)`. `PerformanceHintManager.Session` has `updateTargetWorkDuration`, `reportActualWorkDuration(long)`, `reportActualWorkDuration(WorkDuration)`, `sendHint`, `setPreferPowerEfficiency`, `setThreads`, `getThreadIds` and `close` (`g_PerformanceHintManager.java`, AOSP `frameworks/base/core/java/android/os/PerformanceHintManager.java`, same blob in `r1/` and `r3/`). The Java class wraps the native session. `APerformanceHint_borrowSessionFromJava` (API 36) unwraps it.

#### 2. Thermal API (NDK)

##### 2.1 Prototypes

Copied from `headers/thermal.h` (AOSP `frameworks/native/include/android/thermal.h`, CP2A head). Levels match `g_libandroid.map.txt`.

```c
enum AThermalStatus {
    ATHERMAL_STATUS_ERROR = -1,  ATHERMAL_STATUS_NONE = 0,     ATHERMAL_STATUS_LIGHT = 1,
    ATHERMAL_STATUS_MODERATE = 2, ATHERMAL_STATUS_SEVERE = 3,  ATHERMAL_STATUS_CRITICAL = 4,
    ATHERMAL_STATUS_EMERGENCY = 5, ATHERMAL_STATUS_SHUTDOWN = 6,
};
typedef enum AThermalStatus AThermalStatus;
typedef void (*AThermal_StatusCallback)(void* _Nullable data, AThermalStatus status);

/* API 30 */
AThermalManager* _Nonnull AThermal_acquireManager();
void AThermal_releaseManager(AThermalManager* _Nonnull manager);
AThermalStatus AThermal_getCurrentThermalStatus(AThermalManager *_Nonnull manager);
int AThermal_registerThermalStatusListener(AThermalManager *_Nonnull manager,
        AThermal_StatusCallback _Nullable callback, void* _Nullable data);
int AThermal_unregisterThermalStatusListener(AThermalManager* _Nonnull manager,
        AThermal_StatusCallback _Nullable callback, void* _Nullable data);

/* API 31 */
float AThermal_getThermalHeadroom(AThermalManager* _Nonnull manager, int forecastSeconds);

/* API 35 */
struct AThermalHeadroomThreshold { float headroom; AThermalStatus thermalStatus; };
typedef struct AThermalHeadroomThreshold AThermalHeadroomThreshold;
int AThermal_getThermalHeadroomThresholds(AThermalManager* _Nonnull manager,
        const AThermalHeadroomThreshold* _Nonnull * _Nullable outThresholds,
        size_t* _Nonnull size);

/* API 36 */
typedef void (*AThermal_HeadroomCallback)(void *_Nullable data, float headroom,
        float forecastHeadroom, int forecastSeconds,
        const AThermalHeadroomThreshold* _Nullable thresholds, size_t thresholdsCount);
int AThermal_registerThermalHeadroomListener(AThermalManager* _Nonnull manager,
        AThermal_HeadroomCallback _Nullable callback, void* _Nullable data);
int AThermal_unregisterThermalHeadroomListener(AThermalManager* _Nonnull manager,
        AThermal_HeadroomCallback _Nullable callback, void* _Nullable data);
```

The header annotates `AThermal_acquireManager` as `_Nonnull` yet documents "nullptr on failure": check the pointer. Callbacks run "on system binder thread pool".

##### 2.2 Headroom semantics and the polling limit

- Return value: non-negative, 1.0 means the device is or will be throttled at `ATHERMAL_STATUS_SEVERE`. Values may exceed 1.0 with no fixed mapping. 0.0 is a fixed distance below 1.0 and maps to no status. Values in (0.0, 1.0] scale linearly with temperature. Negative values are clamped to 0.0 (header). In the service the distance is 30 degrees Celsius (`DEGREES_BETWEEN_ZERO_AND_ONE = 30.0f`, `g_ThermalManagerService.java`, AOSP `frameworks/base/services/core/java/com/android/server/power/thermal/ThermalManagerService.java`).
- `forecastSeconds`: how far ahead to extrapolate. The service accepts 0 to 60 and returns NaN outside that range (`MIN_FORECAST_SEC`, `MAX_FORECAST_SEC`, in `g_` and in `r1/ThermalManagerService.java`). Until the service has several samples (`MINIMUM_SAMPLE_COUNT = 3`) the current headroom is returned whatever the forecast (header, constant in the service).
- Rate limit, as documented in the header: it tracks slow sensors such as skin temperature, so "there is no benefit to calling this function more frequently than about once per second, and attempted to call significantly more frequently may result in the function returning `NaN`". The return clause repeats it: NaN "if the device does not support this functionality or if this function is called significantly faster than once per second".
- The games guide is stricter because of older devices: "Don't call the GetThermalHeadroom() API too frequently. If you do so, the API returns NaN. You shouldn't call it more than once every 10 seconds", and "Avoid calling from multiple threads". It also says that if the first value is NaN the API is not available on the device (https://developer.android.com/games/optimize/adpf/thermal, last updated 2026-02-26). Practical rule: one polling thread, one call every 10 seconds to be safe on all devices (1 Hz is the header's floor), and treat NaN as "no data", not as "cool".
- NaN is also what the NDK wrapper returns on any binder failure (`AThermal_getThermalHeadroom`, `r3/thermal.cpp`, AOSP `frameworks/base/native/android/thermal.cpp`) and what the service returns when the HAL is not ready or no SEVERE threshold exists (`g_ThermalManagerService.java`). I did not trace the exact code that enforces the call-frequency limit: the limit itself is cited from the header only.
- `AThermal_getThermalHeadroomThresholds`: `*outThresholds` must be null on entry or the call returns `EINVAL`. `ENOSYS` means the feature is disabled. The SEVERE threshold is always 1.0. The header says the caller should "array-delete" the result, and that before API 36 the list is cached and owned by the manager. That ownership rule is awkward from C: copy the values and take care before freeing.
- Headroom listener (API 36): fires on threshold crossings and threshold changes, not on forecast changes, so the header says polling "should still be used". The service spaces callbacks at least 5000 ms apart and requires a change of 0.03 (`HEADROOM_CALLBACK_MIN_INTERVAL_MILLIS`, `HEADROOM_CALLBACK_MIN_DIFFERENCE`).

#### 3. Thread priority, real-time classes and affinity

##### 3.1 Nice values

`Process.THREAD_PRIORITY_*` (`r3/Process.java`, AOSP `frameworks/base/core/java/android/os/Process.java`, tag `android-16.0.0_r3`): `LOWEST` 19, `BACKGROUND` 10, `LESS_FAVORABLE` +1, `DEFAULT` 0, `MORE_FAVORABLE` -1, `FOREGROUND` -2, `DISPLAY` -4, `URGENT_DISPLAY` -8, `VIDEO` -10, `AUDIO` -16, `URGENT_AUDIO` -19. Hidden: `THREAD_PRIORITY_TOP_APP_BOOST` -10, "Priority we boost main thread and RT of top app to". The Javadoc for FOREGROUND and every level above it says applications "can not normally change to this priority".

##### 3.2 Is raw `setpriority` to a negative nice permitted? Yes, and why

- `init.rc` runs "# set RLIMIT_NICE to allow priorities from 19 to -20" then `setrlimit nice 40 40` in early init, and every process inherits it (`r3/init.rc` line 43, AOSP `system/core/rootdir/init.rc`, also in `g_init.rc` and `l_init.rc`).
- The kernel lets a task lower a nice value without `CAP_SYS_NICE` when `nice_to_rlimit(nice) <= RLIMIT_NICE`. With a limit of 40 every value down to -20 passes (`is_nice_reduction` and `can_nice` in `k_syscalls.c`, `set_one_prio` in `k_sys.c`).
- SELinux grants every domain `setsched` and `getsched` on itself (`allow domain self:process { ... getsched setsched ... }`, `g_sepolicy_private_domain.te`, AOSP `system/sepolicy/private/domain.te`, CP2A head). `setpriority`, `sched_setaffinity`, `sched_setattr` and `sched_setscheduler` are in bionic's syscall list for all ABIs and not in the app seccomp blocklists (`r3/SYSCALLS.TXT`, `r3/SECCOMP_BLOCKLIST_APP.TXT`, `r3/SECCOMP_BLOCKLIST_COMMON.TXT`, AOSP `bionic/libc/`).
- So `setpriority(PRIO_PROCESS, tid, -10)` on the app's own thread succeeds on AOSP. The Javadoc wording is a convention, not an enforced limit in the code read. Two cautions: the common kernel has vendor hooks on this path (`trace_android_rvh_set_user_nice`, `ack_android16-6.12.c`), and ActivityManager changes the main and render thread priorities of the top app itself. OEM behaviour is not verified.
- `Process.setThreadPriority` adds nothing: since Android 13 `androidSetThreadPriority` is only `setpriority` (`r3/Threads.cpp`, AOSP `system/core/libutils/Threads.cpp`, same code at tags `android-13.0.0_r1` to `android-15.0.0_r1`). At `android-12.0.0_r1` it also moved a thread with nice 10 or more into the `SCHED_SP_BACKGROUND` profile and moved it back when the nice dropped below 10. Raw `setpriority` never did that. On Android 13 and later nothing in AOSP userspace moves a thread to a background cgroup because of its nice value.

##### 3.3 Real-time classes

Apps hold no capabilities: `neverallow { appdomain -bluetooth -network_stack -nfc } self:capability_class_set *;` (`g_sepolicy_private_app.te`, AOSP `system/sepolicy/private/app.te`). Without `CAP_SYS_NICE` the kernel refuses a change to an RT policy when `RLIMIT_RTPRIO` is zero ("Can't set/change the rt policy", `user_check_sched_setscheduler`, `k_syscalls.c`). `init.rc` raises only `nice`, `nofile` and `memlock`. That the default `RLIMIT_RTPRIO` is 0 is from memory, not verified. Conclusion: no `SCHED_FIFO` or `SCHED_RR` for an app.

##### 3.4 Affinity and cpusets

- `sched_setaffinity` on the app's own threads is allowed (same SELinux and seccomp evidence as above).
- The kernel intersects the requested mask with the task's cpuset: `cpuset_cpus_allowed(p, cpus_allowed); cpumask_and(new_mask, ctx->new_mask, cpus_allowed);` (`__sched_setaffinity`, `k_syscalls.c`). If no valid CPU remains the call fails with `EINVAL` (`dest_cpu >= nr_cpu_ids`, `ack_core_android15-6.6.c`, `kernel/sched/core.c`).
- When the cpuset changes, because the app moved between top-app, foreground and background or because the cpuset's CPU list was rewritten, the kernel calls `set_cpus_allowed_ptr(task, cpus_attach)` for every task (`cpuset_attach_task` and `cpuset_update_tasks_cpumask`, `k_cpuset.c`). On `android15-6.6` the new mask is first intersected with the mask the task last asked for (`p->user_cpus_ptr`) if that intersection is not empty, otherwise the whole new cpuset is used (`__set_cpus_allowed_ptr`, `ack_core_android15-6.6.c`). The same function in `ack_core_android14-6.1.c` showed no such masking in my extract, so on 6.1 and older expect the older documented rule: the task "will be allowed to run on any CPU allowed in its new cpuset, negating the effect of the prior sched_setaffinity() call" (`k_cpusets.rst`, `Documentation/admin-guide/cgroup-v1/cpusets.rst`). I did not trace 6.1 fully.
- Consequences: a pin can be widened silently when the app is backgrounded and may not come back on older kernels, so re-apply it on resume and re-read `sched_getaffinity`. A mask chosen from the top-app CPUs can fail with `EINVAL` in the background. Vendor hooks exist here too (`trace_android_vh_sched_setaffinity_early`, `trace_android_rvh_set_cpus_allowed_ptr`).

#### 4. cpusets and uclamp

- AOSP mounts cgroup v1 `cpu` at `/dev/cpuctl` and `cpuset` at `/dev/cpuset` (`r3/cgroups.json`, AOSP `system/core/libprocessgroup/profiles/cgroups.json`).
- `init.rc` creates the cpusets `foreground`, `background`, `system-background`, `restricted`, `top-app`, `foreground_window` and `camera-daemon`, each a copy of all CPUs, with the comment "the device's init.rc must actually set the correct cpus". `system-background` is "for system tasks that should only run on little cores" and `restricted` is "for system tasks that are being throttled due to screen off" (`r3/init.rc` lines 314 to 346). The real CPU lists are device specific.
- Task profiles (`r3/task_profiles.json`, AOSP `system/core/libprocessgroup/profiles/task_profiles.json`): `CPUSET_SP_TOP_APP` is `MaxPerformance` (cpu cgroup `top-app`), `ProcessCapacityMax` (cpuset `top-app`), `MaxIoPriority` and `TimerSlackNormal`. `CPUSET_SP_FOREGROUND` uses cpu and cpuset `foreground`. `CPUSET_SP_BACKGROUND` uses cpu `background`, cpuset `background`, low I/O priority and a 40 ms timer slack. `CPUSET_SP_RESTRICTED` uses cpuset `restricted`.
- uclamp in the generic profiles: only attributes (`UClampMin`, `UClampMax`, `UClampLatencySensitive` on `cpu.uclamp.*`) and two profiles, `CpuPolicySpread` and `CpuPolicyPack`, that write `cpu.uclamp.latency_sensitive`. The generic `init.rc` writes a uclamp value only for `nnapi-hal`. A `top-app` `cpu.uclamp.min` is therefore set by device files. That Pixel and other devices do so is from memory, not verified.
- The cgroup files are owned by `system` with mode 0664, so an app cannot move itself or write clamp values (`r3/init.rc` lines 130 to 174).
- Can an app set uclamp itself? Not on the Android common kernel. `android16-6.12` adds to `user_check_sched_setscheduler`: `if (!capable(CAP_SYS_NICE)) { /* Can't change util-clamps */ if (attr->sched_flags & SCHED_FLAG_UTIL_CLAMP) return -EPERM; }` (`ack_android16-6.12.c`, `kernel/sched/syscalls.c`). Mainline 7.4-rc2 has no such test (`k_syscalls.c`): there a request is only bounded by the cgroup `cpu.uclamp.max` and the `sched_util_clamp_min` and `sched_util_clamp_max` sysctls (`k_sched-util-clamp.rst`, sections 3.1 to 3.3). Other Android kernel branches were not checked. For an app, ADPF is the supported route to a uclamp boost.
- OEM additions (extra cgroups, vendor scheduler modules, game boosters) are device specific and not covered by any source read here.

#### 5. Finding core capacities and per-thread run time

- `/sys/devices/system/cpu/cpuX/cpu_capacity`: "capacity of cpuX" (`k_sysfs-devices-system-cpu`, `Documentation/ABI/testing/sysfs-devices-system-cpu`). Capacity is the CPU's "maximum attainable performance level" (`k_sched-capacity.rst`). The arm64 binding normalises so that the largest CPU is 1024 (`k_cpu-capacity.txt`, `Documentation/devicetree/bindings/cpu/cpu-capacity.txt`).
- `/sys/devices/system/cpu/online`, `possible`, `present`: CPUs "online and being scheduled", CPUs that "have been allocated resources", CPUs "identified as being present" (same ABI file).
- cpufreq policy files (`k_cpufreq.rst`, `Documentation/admin-guide/pm/cpufreq.rst`): `cpuinfo_max_freq` is the maximum frequency in kHz, `related_cpus` lists "all (online and offline) CPUs belonging to this policy", `scaling_cur_freq` is usually the last frequency requested and "may or may not reflect the frequency the CPU is actually running at". They sit under `cpufreq/policyN/` and under each `cpuN/cpufreq/` link.
- SELinux: everything under `/sys/devices/system/cpu` is labelled `sysfs_devices_system_cpu` (`genfscon sysfs /devices/system/cpu ...`, `g_sepolicy_private_genfs_contexts`) and every domain may read it (`r_dir_file(domain, sysfs_devices_system_cpu)`, `g_sepolicy_private_domain.te` line 221, same line in the `l_` copy). File modes and vendor relabelling of single files such as `scaling_cur_freq` vary by device and were not verified, so treat every read as optional.
- `/proc/<pid>/schedstat` has three fields: time on CPU in nanoseconds, time waiting on a run queue in nanoseconds, and the number of timeslices run (`k_sched-stats.rst`, `Documentation/scheduler/sched-stats.rst`). That the per-thread path `/proc/self/task/<tid>/schedstat` has the same format, that `/proc/self/task/<tid>/comm` holds the thread name, and that an app may read both for its own threads, is from memory, not verified.

#### 6. Frame pacing

Web pages in this and the next two sections were fetched on 2026-11-13. Text copies are in `(session scratch)/survey/web/`.

**Android Frame Pacing library (Swappy).** Part of the AGDK, for OpenGL and Vulkan. It "uses Android Choreographer for synchronization and deals with the variability in the tick delivery", uses presentation timestamps (`EGL_ANDROID_presentation_time`, `VK_GOOGLE_display_timing`) "so that frames are not presented early", and uses sync fences (`EGL_KHR_fence_sync`, `VkFence`) "to inject waits into the application that allow the display pipeline to catch up" in place of buffer stuffing. It picks the display refresh rate closest to the target frame rate. Pipeline mode "separates the CPU and GPU workloads across VSYNC boundaries". Non-pipeline mode suits very short frames. Auto mode "measures CPU and GPU times" to choose the swap interval and to switch pipelining off for very fast frames. The page recommends starting with both auto modes off and a fixed swap interval (https://developer.android.com/games/sdk/frame-pacing). The Vulkan entry points shown are `SwappyVk_determineDeviceExtensions`, `SwappyVk_setQueueFamilyIndex`, `SwappyVk_initAndGetRefreshCycleDuration`, `SwappyVk_setWindow`, `SwappyVk_setSwapIntervalNS`, `SwappyVk_queuePresent` (called in place of `vkQueuePresentKHR`), `SwappyVk_setAutoSwapInterval`, `SwappyVk_setMaxAutoSwapIntervalNS`, `SwappyVk_setAutoPipelineMode` and `SwappyVk_destroySwapchain` (https://developer.android.com/games/sdk/frame-pacing/vulkan/add-functions). Their exact signatures were not copied: take them from the library's `swappyVk.h`.

**AChoreographer.** From `g_choreographer.h` (AOSP `frameworks/native/include/android/choreographer.h`, CP2A head, same blob in the `l_` copy):

```c
AChoreographer* AChoreographer_getInstance();                                   /* API 24 */
void AChoreographer_postFrameCallback64(AChoreographer* choreographer,
        AChoreographer_frameCallback64 callback, void* data);                   /* API 29 */
void AChoreographer_postVsyncCallback(AChoreographer* choreographer,
        AChoreographer_vsyncCallback callback, void* data);                     /* API 33 */
```

The API 33 vsync callback receives an `AChoreographerFrameCallbackData` with `getFrameTimeNanos`, `getFrameTimelinesLength`, `getPreferredFrameTimelineIndex` and, per timeline, `getFrameTimelineVsyncId`, `getFrameTimelineExpectedPresentationTimeNanos` and `getFrameTimelineDeadlineNanos`, all API 33. The reference says that from API 33 "apps can follow proper frame pacing and even choose a future frame to render" (https://developer.android.com/ndk/reference/group/choreographer). The frame pacing page warns that Choreographer alone leaves device-specific tick offsets and that buffer stuffing "may still occur for long frames".

**Frame rate hints.** From `g_native_window.h` (AOSP `frameworks/native/libs/nativewindow/include/android/native_window.h`, CP2A head):

```c
int32_t ANativeWindow_setFrameRate(ANativeWindow* _Nonnull window,
        float frameRate, int8_t compatibility);                                 /* API 30 */
int32_t ANativeWindow_setFrameRateWithChangeStrategy(ANativeWindow* _Nonnull window,
        float frameRate, int8_t compatibility, int8_t changeFrameRateStrategy); /* API 31 */
```

The frame rate guide says an app that will not run at the display refresh rate "should specify presentation timestamps for each frame" with `ASurfaceTransaction_setDesiredPresentTime()`, `EGL_ANDROID_presentation_time` or `VK_GOOGLE_display_timing`, because that "stops the platform from presenting an app frame too early" (https://developer.android.com/media/optimize/performance/frame-rate). API 36 hint sessions only gain from surface binding when the app keeps the surface frame rate up to date with these calls (`headers/performance_hint.h`, `APerformanceHint_setNativeSurfaces`).

**Vulkan timing extensions.** `VK_GOOGLE_display_timing` is a device extension listed under Android 7.0, API level 24 (https://developer.android.com/ndk/guides/graphics/extensions). `VK_EXT_present_timing` appears in none of the Android pages read. Whether Android drivers expose it is from memory, not verified.

#### 7. Game Mode and related features

**Game Mode API.** Available on "Select Android 12 devices" and on "Devices running Android 13 or higher". The app declares `android:appCategory="game"`, calls `GameManager.getGameMode()` (API 31) and "must always query the getGameMode() method when resuming a paused process". The modes are UNSUPPORTED, STANDARD, PERFORMANCE and BATTERY, and the class reference adds `GAME_MODE_CUSTOM`. Support is declared with a `<meta-data android:name="android.game_mode_config" android:resource="@xml/game_mode_config" />` entry and a `game-mode-config` resource carrying `android:supportsBatteryGameMode` and `android:supportsPerformanceGameMode`. "If your game declares support for a Game Mode, the game must implement its own optimization. As a result, the platform resets any previously applied Game Mode interventions by OEMs" (https://developer.android.com/games/optimize/adpf/gamemode/gamemode-api, https://developer.android.com/reference/android/app/GameManager).

**Interventions.** These are OEM-set optimisations applied without code changes: WindowManager backbuffer resize (resolution downscaling), ANGLE in place of the native GLES driver, and FPS throttling (Android 13 or later, it can only limit the frame rate). "OEMs might choose to implement Game Mode interventions without developer feedback." The opt-out attributes in the same resource are `android:allowGameDownscaling="false"` and `android:allowGameFpsOverride="false"` (https://developer.android.com/games/optimize/adpf/gamemode/gamemode-interventions, https://developer.android.com/games/optimize/adpf/gamemode/fps-throttling). For an emulator that renders at its own resolution and paces its own frames, declaring these opt-outs avoids a silent backbuffer resize or frame cap.

**Game State API.** `GameManager.setGameState(GameState)` is API 33 and tells the system what the game is doing, for example loading or `GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE`, so that "the system is be able to optimize resources and power accordingly" (https://developer.android.com/games/optimize/adpf/gamemode/gamestate-api). What a device does with the loading flag is vendor specific and not verified.

**Sustained performance mode.** `Window.setSustainedPerformanceMode(boolean)` is API 24 and "Sets the Sustained Performance requirement for the calling window" (https://developer.android.com/reference/android/view/Window). `PowerManager.isSustainedPerformanceModeSupported()` reports whether the device implements it (`g_PowerManager.java`, AOSP `frameworks/base/core/java/android/os/PowerManager.java`). Which current phones implement it is not verified.

**Fixed performance mode.** For benchmarking only, Android 11: it "sets CPU and GPU clocks with an upper and lower bound" and "does not yield the maximum device performance" (https://developer.android.com/games/optimize/adpf/fixed-performance-mode).

**Game Dashboard and OEM game boosters.** The Game Mode page names the Game Dashboard as the user's way to switch modes. Vendor game boosters, per-game profiles and thermal overrides are device specific. Nothing about them was verified.

#### 8. Multi-threaded workloads, non-frame workloads and core pinning

**What the documentation says about grouping threads.** "A Session represents a group of threads with an inter-related workload such that hints for their performance should be considered as a unit. The threads in a given session should be long-lived and not created or destroyed dynamically" (`headers/performance_hint.h`, repeated at https://developer.android.com/reference/android/os/PerformanceHintManager.Session). Threads whose timings are not related therefore belong in separate sessions, each with its own target. For an emulator one reasonable split is a session for the guest CPU threads timed per emulated frame, and a second one for the render or GPU-command thread timed per presented frame. That split is my inference from the definition. I found no Google page that prescribes a number of sessions.

**Non-frame work.** The API is "intended for periodic workloads". A session created with target 0 reports nothing but can still carry `setPreferPowerEfficiency` (API 35), which marks threads that "can be safely scheduled to prefer power efficiency over performance", a fit for shader compilation or streaming workers. From API 36 the `notifyWorkloadIncrease`, `notifyWorkloadReset` and `notifyWorkloadSpike` hints cover sudden changes such as a level load: they "should be sent shortly before the start of the cycle where the workload is going to change" and "should not be sent repeatedly for an ongoing expensive workload" (header).

**API 36 graphics pipeline mode.** It is meant for a session that times "the critical path of graphics pipeline from end to end", limited to 5 threads per app, and it can take timing from SurfaceFlinger through `setUseAutoTiming` (header, `g_SessionMode.aidl`). Check `APerformanceHint_isFeatureSupported` first.

**State of the games guide.** The ADPF overview (last updated 2026-02-26) lists the Thermal API, Game Mode and Game State APIs, Fixed Performance Mode and Power Efficiency Mode, and does not list a Performance Hint API page. The former address https://developer.android.com/games/optimize/adpf/performance-hint-api returned 404 on 2026-11-13 (https://developer.android.com/games/optimize/adpf). Together with the `GAME` tag behaviour in section 1.3 this suggests Google is changing how games feed hint sessions. That reading is mine and is not confirmed by any page. The once-per-frame pattern with total, CPU and GPU durations is cited here from the header only.

**Pinning threads to big cores.** Google's page on thread scheduling describes affinity and then advises against it. Pinning "instructs the system to schedule it on the given core when your game is in the foreground", but "The platform software can't dynamically adjust task placement for runtime factors such as load and thermal throttling", device behaviour varies widely by price and age, and "By forcing affinities to big cores, you may unnecessarily increase battery drain and thermal load." The key sentence: "For these reasons, it's generally best to avoid manually setting CPU affinities." The same page still allows it as a remedy: "If you find that certain threads are being scheduled on CPUs that don't meet their needs for performance or power, consider manually setting the CPU affinity for those threads." It also notes that big cores usually have the higher CPU indexes "by common convention, but it's not a guarantee", which is the reason to read `cpu_capacity` (https://developer.android.com/agi/sys-trace/threads-scheduling, last updated 2026-05-19).

#### 9. Claims not verified from a primary source

- That CP2A.260605.016 is Android 16 QPR2, and anything added after it (API 37).
- Whether platform builds zero-initialise the uninitialised `sessionOut` local in `APerformanceHintManager::createSession`.
- The dead-tid path through `checkTidValid`: read from code, not run on a device.
- The code that enforces the call-frequency limit of `AThermal_getThermalHeadroom`: cited from the header and the games guide only.
- That `getPreferredUpdateRateNanos` returning -1 means hint sessions are unsupported.
- The default `RLIMIT_RTPRIO` of 0 for apps.
- Device files that set a `top-app` `cpu.uclamp.min`, and every OEM or vendor behaviour (vendor hooks, game boosters, Game State handling, sustained performance support).
- The uclamp `CAP_SYS_NICE` rule on Android kernel branches other than `android16-6.12`, and the cpuset affinity behaviour on `android14-6.1` and older.
- DAC modes and vendor SELinux labels of single cpufreq files such as `scaling_cur_freq`.
- The per-thread `/proc/self/task/<tid>/schedstat` and `comm` paths and their readability by the app.
- Android support for `VK_EXT_present_timing`, and the exact `SwappyVk_*` signatures.
- The session split suggested in section 8 and the reading of the games guide's current state.

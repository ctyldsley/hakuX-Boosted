# Handover: hakuX-Boosted

The one file to read first in any new session, cloud or desktop. It says
where the work stands, what is waiting on whom, and how to work in this
repository. **Update it before a session ends** (the "Right now" section
and a line in the log at the bottom), and commit it with the work.

Last updated: 2026-11-12 (run 37 tested on a cold shader cache; the build on the phone is run 37, `42f03a8`).

## The backlog: promised to the owner, not yet in a build

The owner asked (2026-11-12) that nothing discussed gets lost while the
core problems are chased. Keep this list honest: strike an item only when
a build with it has been on the phone. Order within a group is the order
to do it in.

Stability (first):
- The guest stall: Fable II joins its worker while holding a lock the
  worker needs (see Right now). Run 39 still stalls. Measure wake
  latency and lock hand-off cost next; nothing fixed yet.
  Measurement written (`NoteWakeLatency`, `NoteCriticalSectionWait`):
  in the build after run 39. Read "late wake" and "waited ... for critical
  section" lines, and the totals line in a thread dump.
- The glitches that survive waiting for pipelines: cause not seen, needs
  a capture (`device-watch.py`), ideally in an area already visited.
- Scrambled frames at scene cuts: may be the same thing, unconfirmed.

First-run shader hitches and glitches (every title):
- Wait only for full-screen passes, let object draws pop in. Written
  (`vulkan_async_wait_fullscreen_draws`), committed, not built or run.
  To check on a cold cache with `shader_profiling = true`: the log's
  "submission waited ... for N full-screen pass pipeline(s)" lines say
  how many waits and how long; the owner says whether the whole-screen
  flashes are gone. If object pop-in is what still offends, the heuristic
  is not the answer and the stand-in is.
- The compile threads above the guest's spinning threads. Not written.
- Try `vulkan_placeholder_pipelines = true` on the phone (config only) to
  see the vertex interpreter stand-in; then the pixel stand-in (cheap
  approximation first, interpreter after). Largest item; a cloud session
  once the phone has shown what it must fix.
- Slimmer SPIR-V from the translator (registers in memory, two samples
  and run-time switches per texture fetch, debug names).
- Keep a title's stored shader list across reinstalls.

Performance (after stability; nothing here is started):
- Step 0, `HostThreadState` in bionic's `TLS_SLOT_APP` (the TLS resolver
  is about 9% of the busiest thread).
- The yield and wait path (`NtYieldExecution` about 13% of that thread).
- Host thunks (about 20%): inline fast paths for the hottest exports,
  critical sections first.
- The JIT's hot functions; then weigh a JIT redesign.
- The game's audio mixing thread (about 28% of a core): HLE candidate.
- Re-test the 60 FPS patch as the yardstick after each of these.

UI (independent of the core work). All three are written and built on the
branch `claude/ui-settings-tabs` (run 41), not merged and not on the phone:
see "The UI refinement" below.
- Settings as tabs: Global / Xbox / Xbox 360.
- The frame rate counter inside the display's safe inset.
- Xbox-inspired look with the polish of the Cocoon launcher, no branded
  assets.

## Cloud sessions started 2026-11-12 23:30, to merge after review

Each is on its own branch cut from this branch at `7648e9c` (or `04cbb62`
if that build failed), adds settings that default to today's behaviour,
and leaves its notes in `boosted/notes/<name>.md` instead of editing this
file or `ARCHITECTURE.md`. None can run anything on a device. Merge one
at a time, each followed by a device run with its setting on, and copy
its list of changed upstream files into `ARCHITECTURE.md` at the merge.

- `claude/pixel-stand-in`: a pixel-shader stand-in for the placeholder
  pipeline model (`gpu/vulkan/vulkan_pipeline_cache.*`,
  `vulkan_command_processor.*`, `vulkan_shader.*`, `gpu/shaders/**`).
- `claude/spirv-slimming`: no debug names, an optional SPIRV-Tools pass,
  and a written analysis of the translator's bulk
  (`gpu/spirv_shader_translator*`, `gpu/spirv_builder.*`).
- `claude/thread-placement`: a sourced survey of how other emulators
  and Android handle priority, core placement and pacing; ADPF hint
  session, core placement policy and a per-second core and run-queue
  log, in new files under `xenia/base`.
- `claude/ui-settings-tabs` (started earlier): settings tabs, the frame
  rate counter's inset, the look.

Kept on this branch, because they are in files this session is still
changing: the guest scheduler (yield storm, timed sleeps), the missed
pokes, `TLS_SLOT_APP`, and the test of `guest_threads_above_helpers`.
Until those branches are merged, this session keeps out of the files
they own.

## Right now

- **`host_perf_hint = true` did not lift the phone's frequency cap (run 73, `device-logs/run73-hint`, 582 s, cold cache).** The cap follows heat, not the hint: per 100 s the performance cores' limit went 2745, 2402, 1862, 1670, 1546 MHz (baseline: 2745, 2739, 2099, 1764), thermal headroom 0.41 rising to 0.57, battery 42.5 C at the end. Even cold the limit is 2745 of 3628 MHz. Frame rate no better: median 30, tenth percentile 26 (baseline 28), 21 seconds with a frame over 100 ms (16 in 423 s). Late wakes 17, worst 68.5 ms. The owner felt it slightly better; the numbers do not show it. Defect in the hint session: it covers the six dispatch threads but `GPU Commands` is reported missing (the log's `GPU missing`), to fix in `base/host_perf.cc`. GameThread's dispatch thread (G0) and the spinning 3D Engine thread (G3) each fill a core, so wasted heat is the lever: next run is `guest_scheduler_park_poll_loops = true` alone, hint off.

- **Run 73 baseline, measured 2026-11-13 (cold cache, every new switch off; `device-logs/run73-baseline`).** No freeze in 12,063 frames. Dispatch threads and `GPU Commands` confirmed at nice -8, compile threads at nice 8. Late wakes 19 (worst 37.6 ms) against 43 in 6,490 frames (worst 179.1 ms) in run 43; critical-section waits 13 against 28. Missed wakes 0. Frame rate from `host perf`: median 30 a second, tenth percentile 28, worst frame 826 ms. The owner saw no difference: glitches and stutter in the first minutes, as before (199 pipelines compiled, same as run 43). **New fact: the phone caps its cores well under their maximum while the game runs.** `scaling_max_freq` moved between 1.67 and 2.75 GHz on the performance cores (maximum 3.63) and 1.50 and 2.88 GHz on the prime cores (maximum 4.61), thermal status `none`, battery 38 C, charging. Two guest threads (CPU 0 and CPU 3) each fill a core. Next run: `host_perf_hint = true` to see whether a performance hint session lifts the cap.

- **Run 73 (combined build, `claude/integration-1113` at `5fceb59`) is on the phone; the settings tabs failed the owner's first look, fixed in the next push, not yet built.** Two things, 2026-11-13: (1) Global carried each console's rows (controls, GPU drivers, file access), so tabs read as duplicates. The owner's rule: Global is only what applies to both (games folders, theme, updates, logs), Xbox only the Xbox core's, Xbox 360 only the 360's, no row twice. Done in `SettingsScreen.kt` and `boosted/X360.kt`. (2) A black, empty window after back: reproduced over adb, two taps on the back arrow 120 ms apart pop the library as well (`nav.popBackStack()` unguarded, upstream's). Fixed with `backOnce()` in `HakuXApp.kt`. The tabs themselves switched correctly when driven over adb, slowly and quickly; the owner's "would not accept input" was not reproduced apart from that. The baseline game run of run 73 (every new switch off, `host_perf_log = true`) is still to do.

- **Why Fable II's worker is late: its host thread queues for a core.
  Measured, fix pushed, not built or run.** A second start of run 43 on a
  cold cache, profiled (`device-logs/run43-profile`: log, `perf-guest.data`
  with the six dispatch threads at 1 kHz for 200 s, `stat-all.txt`). It
  did not stall this time; the owner saw glitches, stutter and warped
  sound early on.
  - `/proc/<pid>/task/<tid>/schedstat` (on a core, waiting for a core,
    timeslices): Guest CPU 0 219 s / 11 s; CPU 3 190 s / 4 s; CPU 5 119 s
    / 9 s; CPU 4 19 s / 13 s; **CPU 1 6.3 s / 15.1 s** in 249,000 slices.
    The worker lives on guest CPU 1. Its dispatch thread sleeps most of
    the time and, each time it wakes, waits for a core longer than it
    then runs.
  - The new timing in the same run: `XThread001B` (the worker) ran 16 ms
    or more after its signal 11 times, longest 137 ms, frames 2849 to
    3165, the first gameplay scene while pipelines compiled; it waited
    up to 45 ms for section `83499138` nine times and GameThread up to
    48 ms twice. No "missed wake made good" line: the poke arrived, the
    host thread was not run. Also late: the audio thread `XThread0009`
    18 times up to 51 ms (the warped sound), the VFS threads up to
    179 ms.
  - Not first-time translation: the profile of the worker's dispatch
    thread has no translator in it (44% title code, 25% in the timed
    sleep's syscall, which is the price of the 1 ms and 2 ms looks).
  - Why: every thread in the process, the six dispatch threads, the GPU
    command thread and the four pipeline compile threads alike, was at
    nice 0 (the audio threads alone at -12). Eleven or more runnable
    threads on eight cores, and a thread that has slept gets no
    preference over ones that spin.
  - The fix: `guest_threads_above_helpers` (Kernel, default true). The
    dispatch threads and the GPU command thread at nice -8, the pipeline
    compile threads at nice 8. **To verify:** `cat /proc/<pid>/task/*/stat`
    for the nice values; the worker's "late wake" lines and CPU 1's
    schedstat wait should all but go; then long play for the stall. If
    queueing remains, the next step is one core per dispatch thread
    (affinity), as the console has.
- **Run 43 (the 2 ms gate recheck plus the timing) stalled within
  seconds of the first gameplay scene (2026-11-12 22:45), and the timing
  says the worker was NOT woken late.** Evidence in
  `device-logs/run43-stuck`. Whole run: 40,544 satisfied waits, 38,968
  within 0.25 ms, 3 over 16 ms (all an audio thread, `XThread0009`),
  longest 32 ms; no "missed wake made good" line and no backstop rescue
  at all; 150 contended critical sections, 4.75 ms each on average, 13 of
  16 ms or more, all GameThread or 3D Engine before frame 1341 (the menus
  and loading: `8349F4FC` and `4C11F140`, mostly one frame long, worth a
  look of their own). So the 18 rescues a quarter of an hour on run 39
  were real but are not this stall.
  - What the dump adds: the section's event was last set by the WORKER
    (its leave, `lr 8263DD58`) 61.012 s before the dump, and "done" by
    the worker 75 ms after that. So in the frame before the fatal one
    GameThread first waited for the worker to leave the section, took
    it, then waited 75 ms more on "done": the job was already taking
    more than two frames, and only did not need the section again by
    luck. Next frame it did.
  - When: the worker thread (`8262DDD0`) is created at frame 1523; the
    first gameplay scene starts at frame 1656 with a storm of shader
    translation and pipeline compiles on a cold cache (1, 11, 16, 13, 21
    frames in the next five seconds, 85 contended sections in one of
    them); the stall is at about frame 1700 to 1720. Every stall so far
    was at a moment when something new began (a cutscene pan, the
    trader's speech, a first sword swing, this scene).
  - Suspects now: (1) the worker running code for the first time, each
    function translated on the thread that calls it (the JIT code cache
    was at 128 MiB by frame 1401); (2) the worker's host thread losing
    its core to the four pipeline compile threads. A profile of the six
    guest CPU threads through the same start is being taken to split the
    worker's time between translating, running and not running. If (1):
    translate ahead of use on other threads, and keep the translated
    code between runs. If (2): the compile threads below the guest's.
- **Run 39 stalled the same way (2026-11-12 22:04, frame 5518), so the
  hand-off reading below was WRONG, and the game's own code now says what
  the stall is.** Evidence in `device-logs/run39-stuck`: the log, the
  title's code (`text_82000000.bin`, 16 MB from the stalled process) and
  its globals; `ppcdis.py`, `frames.py` and `xrefs.py` in the session's
  scratchpad did the disassembly (capstone, PPC big-endian).
  - What Fable II does, from the disassembly. Frame update `82275F00`
    takes a scoped lock on the global section `83499138` (`8227611C`) and,
    still holding it, calls `822BA4F0`, which (1) waits for ever on the
    global "done" event (handle at `83496AF0`) if there is one, (2) runs
    the world update `822CE280`, (3) sets the global "start" event
    (handle at `83496AEC`). The worker thread (`8262DDD0`) loops: wait
    "start", enter a second section `83499154`, run the task list
    (`83496AF8`, 13 tasks at the stall, virtual `+0x94` on each), set
    "done", leave. Its tasks take `83499138` in small getters
    (`8263B960`).
  - So the main thread JOINS the last frame's job while HOLDING a lock
    that job needs. It only works if the job, started at the end of one
    frame's locked update, has always finished before the next frame's
    locked update begins. On the console it has a whole frame for a job
    of a few milliseconds. Here it sometimes has not finished: the worker
    is found parked on `83499138`, GameThread on "done", every time
    (runs 26, 32, 37, 39). Not a lock bug: a race the console's timing
    always wins.
  - Why the job is late here is NOT known yet. Suspects, in order: (1) a
    late wake of the worker on "start": the scheduler's backstop re-poll
    is 64 ms (`kRepollBackstopMs`), two frames, and it only warns at 50
    rescues in 5 s, so one missed wake is silent and is enough; (2) slow
    hand-offs of `83499138` between the two threads (spin count 0, every
    contention is a park and a wake); (3) the worker's host thread
    descheduled under load (it stalled with the recorder, the KGSL hook,
    pipelines compiling); (4) frames shorter than the console's when the
    title catches up after a hitch.
  - **Measured on run 39 with `guest_scheduler_stats = true` (2026-11-12
    22:10, four minutes of play):** "backstop rescues" 6, one each in six
    different seconds (frames 289, 978, 2150, 2983, 3758, 5566). So wakes
    ARE missed, about one every 40 seconds, and each waited for the 64 ms
    backstop. Also in those lines: 370,000 fiber switches a second at the
    median, 1.4 million at the peak (a cost of its own: threads that
    yield in a loop), and a steady 30 frames a second.
  - **Pushed, not built or run: `kGateRecheckMs = 2`.** A parked waiter
    gated on an object or a set has its gate's epoch looked at again
    within 2 ms without a poke (a compare under the lock; nothing is
    re-polled unless the epoch moved). Each catch is logged: "missed
    wake made good by the timed look: tid ... handle ... signalled N ms
    earlier". If Fable II's worker (the thread with entry `8262DDD0`)
    shows there on its "start" handle and the stall stops, that was it.
    Why the poke is missed in the first place is still not found:
    `WakeForSignal`, the park in `BlockCurrentThread` and the CPU's
    park-and-recheck were read and look right.
  - Next, in this order. No build: `guest_scheduler_stats = true` is set
    on the phone, the log then has "backstop rescues N" each second.
    Next build: a wake-latency histogram (signal or wait start, whichever
    is later, to the waiter running) in the scheduler's counters and the
    thread dump, with each wake over 16 ms logged by thread and handle,
    and contended critical section counts and wait time per thread. Then
    fix what the numbers name. The contender-queued-first change stays:
    it closes a real gap, it was not this.
  - Run 39's full-screen waits (`vulkan_async_wait_fullscreen_draws`, cold
    cache, about 5 minutes): 37 waits, 3.0 s in all, three long ones (539,
    853, 890 ms), the rest 141 ms or less, none gave up; 184 pipelines,
    20.1 s. The owner: glitches "a bit less but still there", worst at
    the first corner towards the town square. So object pop-in, or the
    second cause, is what still offends: the stand-in is the answer, the
    heuristic only removed part.
- **The guest deadlock came back on run 37 (2026-11-12 20:06, 34 minutes
  in, a different place from before) and its cause is now read: a
  critical section's hand-off. Fix pushed, not built or run.** Evidence in
  `device-logs/run37b-stuck` (the log with two thread dumps, the section's
  page and its event object read from the stalled process). So the KGSL
  hook being off did not cure it, and the owner identity was never wrong.
  - The state, identical to run 32: section `83499138` owned by
    GameThread (`owning_thread 30029020`, `recursion_count 1`,
    `lock_count 1`, which is host-endian: one waiter). The worker
    `XThread001D` is the one queued waiter on the section's event
    `F8000240`. That event was last set by GameThread itself (`lr
    822A8778`, its leave) and is unsignalled, yet the worker never got it.
    GameThread waits on `F80001E4`, the worker's "done" event, last set
    by the worker in the same millisecond. Frames still present.
  - The reading: the worker incremented `lock_count` (so GameThread's
    leave set the event) but was not in the event's waiter queue yet,
    because `RtlEnterCriticalSection` only queued it inside
    `XObject::Wait`, after the kernel APC pass and the object lookup.
    GameThread left and re-entered in that gap, found the queue empty and
    its own wait consumed the set meant for the worker. The console's
    kernel hands a set to the parked waiter; here the gap is host code a
    busy phone can deschedule (it showed with the recorder, the KGSL hook
    and now with four threads compiling pipelines). Not proven by a trace:
    it is the only order found that leaves this state.
  - The fix (`xboxkrnl_rtl.cc`): a free section is taken by
    compare-and-swap; a contender is put in the event's waiter queue
    BEFORE it increments `lock_count`, so `CooperativeMayAcquire` refuses
    a returning owner. The identity comparison from run 33 is removed
    (never fired; it cost a thread-local read on the hottest exports).
    **To verify:** long play with the phone loaded (a cold shader cache
    is a good load); the same stall would say the reading is wrong.
- **Run 37 on a cold shader cache (`42f03a8`, 2026-11-12 19:25): the
  attract video is fixed, the first-run glitches are not, and the reason
  is the driver's compile time.** Evidence in `device-logs/run37/xe.log`.
  - The video: no green bar or flicker on the first play after boot (the
    owner's eyes). The physical-commit invalidation in `memory.cc` did it.
  - The glitches are still there, in the same places. `skipping the draw`,
    `Draw skipped` and `do not match` are all 0, so
    `async_shader_translate_on_draw` closed that path. What is left logs
    nothing: **the app ships `vulkan_async_skip_draws = true`**
    (`SettingsSchema.kt`, `default_config.toml`; the cvar's own default is
    false, and the bullet below wrongly assumed false). With it,
    `VulkanPipelineCache::EndSubmission` does not wait, and a deferred bind
    that is still null when the command buffer replays drops its draw.
    Not proven on device yet: the next run sets it false on a cold cache,
    the glitches should turn into waits.
  - **How long a pipeline takes on Turnip v39 (Adreno 840)**, from
    `shader_profiling = true`: 198 pipelines in the first ten minutes,
    24.8 s in all, median 56 ms, p90 408 ms, max 754 ms, 75 over 100 ms.
    Alone in their frame (no other compile running): median 194 ms. It is
    CPU work, not starvation: the four `Vulkan Pipelines` threads had
    22.3 s of CPU against 8.6 s waiting to run. So a new material is
    missing for 2 to 20 frames (skip on), or the frame stalls that long
    (skip off). Neither is acceptable.
  - **It follows the size of the SPIR-V the translator emits.** Quartiles
    of VS+PS bytes against the median compile: 8-30 KB 5 ms, 30-76 KB
    39 ms, 78-152 KB 100 ms, 152-353 KB 371 ms. One shader is 282 KB of
    SPIR-V, the median 39 KB, for guest shaders of a few hundred
    instructions at most. The translation itself is cheap (174 shaders,
    562 ms in all, median 1.6 ms). No optimiser runs on the output
    (SPIRV-Tools is in `third_party`, not hooked up).
  - **So the lever for every title is the shader translator's output**,
    in this order: (1) profile the compile threads on a cold run to see
    which driver passes the time goes to (`simpleperf --app -t <tids>
    --call-graph fp` works, tested); (2) make the SPIR-V small: emit only
    what a shader uses (texture fetch variants, swizzle and write-mask
    code, the pixel shader's tail), or run SPIRV-Tools' dead-code and
    CFG passes on the translation thread, whichever measures better;
    (3) for what still costs more than a frame, a stand-in that draws
    at once (an interpreter shader, as Dolphin and RPCS3 have) and
    compiling a title's stored list at boot, including a list brought
    from another install (`shareable/*.xsh` and `.xpso` are GPU
    independent). Totals so far for Fable II's first 90 minutes: 190
    shaders, 222 pipelines; all 189 stored shaders translate in 122 ms
    at a warm boot.
  - **Second cold run, `vulkan_async_skip_draws = false`**
    (`device-logs/run37b`: log, `perf.data`, `shader-dump/`). 194
    pipelines, 29.5 s, median 74 ms, max 976 ms; 56 frames waited, 35 of
    them over 100 ms, 26 over 250 ms. What the owner saw is not recorded
    yet: ask (glitches gone and hitches instead would prove the cause).
    The compile threads' profile is 84% inside `libvulkan_freedreno.so`
    and 16% in the allocator; the bundled driver has no symbols, so the
    passes cannot be named from the phone.
  - **What the SPIR-V is made of** (169 dumped modules, 9.6 MB; set
    `dump_shaders` to a folder under `compose/` to repeat). By words:
    AccessChain 9.7%, ExtInst 9.7%, Load 7.7%, Decorate 7.0% (a
    NoContraction on each arithmetic result), VectorShuffle 6.7%, Select
    6.5%, names 4.8%. Guest registers live in memory, so every operand is
    an AccessChain and a Load and the driver has to rebuild SSA. The
    largest pixel shader (44 `tfetch`, 636 lines of ucode) has 88 sample
    instructions, 60 switches and 813 selects: each texture fetch samples
    twice and switches on state at run time
    (`spirv_shader_translator_fetch.cc` spends 1,860 lines on one fetch).
    The largest vertex shader has no fetch at all and is still 281 KB
    for 822 lines: about 340 words an instruction.
  - **A stand-in already exists for vertex shaders and is unused here.**
    `vulkan_placeholder_pipelines` (off on the phone and by default)
    selects upstream's hot-swap model: a placeholder pipeline is made at
    once from the ucode interpreter vertex shader
    (`shaders/ucode_interpreter.vs.glsl`, `async_shader_vs_interpreter`)
    and a no-op pixel shader, and swapped for the real one when it is
    ready. The fork's author preferred dropping draws to that
    placeholder's artefacts. A pixel-shader stand-in is the missing half.
  - **Plan agreed in outline with the owner's standing instruction
    (every title, no hitch and no glitch on a first run):** (a) soon: do
    not treat every draw alike while its pipeline compiles: wait for
    full-screen passes (their absence is the whole-screen flash), let
    object draws pop in, and give the compile threads priority over the
    guest's spinning threads (they waited 28% of the time); keep a
    title's stored shader list across reinstalls; (b) then: a stand-in
    pixel shader so the placeholder model draws something sensible at
    once, first a cheap one (first bound texture by the first
    interpolator), then an interpreter like the vertex one; (c) in
    parallel: slim the translator's output, measured by the same
    `shader_profiling` numbers.
  - The warm cache from before this run is kept in
    `device-logs/warm-shader-cache` on the desktop.
- **The dropped draws at `UpdateBindings`: fixed in run 37 (0 logged on a
  cold cache), see above for what remains.** With placeholder pipelines off (the default, the "deferred-bind"
  model) the draw thread does not translate shaders: the background thread
  does. A draw whose pixel shader is not translated yet has no pixel
  bindings, so `GetGuestGraphicsPipelineLayout` cannot complete the layout
  (`counts_complete` false), `RefreshPipelineLayoutIfStale` leaves the
  slot's minimal layout, and `UpdateBindings` refuses the draw because the
  vertex stage wants textures the layout has no room for. That is the
  logged state exactly: `layout stale flag true, pipeline handle null,
  translated true`, `PS 0t/0s`. So `vulkan_async_skip_draws = false` ("no
  draw is ever lost") only held once both shaders were translated. New
  setting `async_shader_translate_on_draw` (GPU, default true): such a
  draw's shaders are translated on the draw thread, the draw is recorded
  and the submission waits for the pipeline as designed. Cost: the SPIR-V
  translation once per new shader on the draw thread. **Check on a cold
  cache** (delete `compose/cache/shaders` and the pipeline cache over adb):
  `skipping the draw` should stay at 0, and say how the hitching feels.
- **For the owner, standing instruction (2026-11-12):** changes should
  raise every game, not polish one. Fable II is the test title. Look for
  the architectural step the way hakuX's own was (QEMU replaced by a FEX
  based translator and a native kernel): weigh the JIT, the guest
  scheduler and the memory model as candidates for a rewrite whenever the
  profiles point at them, and say so when one is found.

- **The merged build on the phone (run 35, `daee679`, 2026-11-12 16:21).**
  The owner's first look at the UI work: the library "looks right" (Fable
  II in hakuX's list), the in-game menu's five options, Settings with the
  frame rate counter appearing at once, and Edit controls all work. The
  in-game settings write to the title's config without losing the bundled
  defaults (`mount_cache`, `vulkan_depth_unorm24` still there).
  - **UI to refine (the owner's notes, for one pass after stability):**
    hakuX's settings should be tabbed Global / Xbox / Xbox 360 rather than
    an Xbox 360 section of links; the frame rate counter's left edge is
    cut off at the top left of this phone (rounded corner or cutout: it
    needs the display's safe inset); the look overall is to move towards
    the Xbox-inspired, Cocoon-launcher polish noted below.
  - **The attract video: cause found in a capture, fix pushed, not built
    or run** (`device-logs/attract/sheet_video.jpg`). Every other frame of
    the video is right; the one between has a green cast, a green line at
    about 72% of the height and a solid green block below it, and the
    owner's stills show the Lionhead logo ghosted over it. That is a YUV
    frame whose chroma is partly stale and partly empty. `xe.log` has, at
    frame 476 while the logo video played, `Vulkan shared memory: skipped
    upload of 68 decommitted page(s)` twice, at `1F32E000` and `1F372000`
    (two adjacent 272 KB buffers). The shared memory marks such pages
    valid without copying, and nothing invalidated them again when the
    guest allocated the same physical pages for the next video's buffers:
    no watch, so its writes were never seen. `PhysicalHeap::Alloc`,
    `AllocFixed` and `AllocRange` now invalidate what they commit
    (`memory.cc`). If the video is still wrong on the next build, check
    whether its buffers are at those addresses at all.
    The owner adds: the video is clean the second time it plays in the
    same boot. That fits: the pages are left wrongly valid only when a
    draw touches them in the moment they are freed (the log has the skip
    exactly twice a boot, at the first hand-over between videos); at the
    end of the first play Release invalidates them and nothing re-marks
    them.
  - **Next, agreed with the owner (stability before performance):**
    1. Run 36 (the video fix) on the phone: the first play of the attract
       video after a boot, and the scene-cut flash, which may be the same
       stale-page fault.
    2. Draws dropped while their pipeline compiles: stop dropping them,
       then compile the stored pipeline list ahead at start. Test on a
       cold cache by deleting `compose/cache/shaders` over adb, not by
       reinstalling.
    3. Performance: the game thread's thunk, TLS resolver and scheduler
       costs; then the JIT's hot functions.
    4. The UI pass (tabs Global / Xbox / Xbox 360, the counter's inset,
       the look), in a cloud session on a branch cut from this one.
  - **The 60 FPS patch off, same build: 27 minutes, frame 31,330, no
    stall** (2026-11-12 16:35 to 17:02). No identity mismatch, skipped
    draw, host-side crash or fiber warning, and a steady 30.1 frames a
    second where runs 26 and 32 gave 21 to 27. So the patch was not what
    kept the deadlock away. What remains different from the runs that
    stalled: the caller's identity from the control region (which logged
    nothing) and the KGSL hook off. The hook is now the suspect for the
    stall as well as the ANRs: at priority 7 the emulator's GPU work and
    the compositor's shared a ringbuffer, and every stalled run had it on.
    One run with `kgsl_context_priority = 7` and nothing else changed
    would settle it; not asked of the owner yet.

- **`claude/xbox-ui` is merged in (2026-11-12).** The next APK from this
  branch has both: the core as tested on run 33 (the hook off by default)
  and the UI work, which has built but never run. What to look at on the
  phone is in "The UI work" below. If the merged build's UI gets in the way
  of testing the core, run 34's APK (`130291b`, the last build without the
  UI) installs over it. The UI branch is finished with unless the cloud
  session is given more to do; further UI work goes on this branch or a new
  one cut from it.

- **Longest clean run so far: 48 minutes, frame 36,890 (run 33, the 60 FPS
  patch on, `kgsl_context_priority = 0`, a warm shader cache; 2026-11-12
  14:30 to 15:18).** No not-responding report, no guest stall, no skipped
  draw, no `thread identity mismatch` line, no host-side crash. 28 to 30
  frames a second in this later area (34 to 43 in the opening scenes). The
  owner: playable, graphical faults "very minimal" this time, performance
  uneven, touch input laggy at this frame rate.
  - The hook is now off by default in the code (see the log's newest line
    for the build).
  - **What stopped the guest deadlock is not known.** Three things changed
    at once against the runs that stalled: the caller's identity from the
    control region (run 33), the hook off, the 60 FPS patch. The identity
    change logged no mismatch at all, so on its own evidence it changed
    nothing. Take them apart one at a time when the stall has to be
    understood rather than avoided: first the same build with the patch
    off.
  - **No skipped draws with a warm cache** (414 to 1,601 on cold ones):
    the dropped draws and much of the flicker belong to shaders compiling
    during play. Fixing the draw that arrives before its pipeline, and
    compiling ahead, is the stability item for a first run.
  - **Order agreed with the owner:** stability first, then performance.

- **Run 33 with the 60 FPS patch: the app's own UI thread starves and
  Android reports it not responding (2026-11-12 13:57;
  `device-logs/run33`).** Not the guest deadlock, and no `thread identity
  mismatch` line in 3,870 frames. The app's main thread sat in
  `HardwareRenderer.nSyncAndDrawFrame` for 12.3 s (`Davey!`, `Skipped 1410
  frames`): it waits for the app's render thread, which draws the touch
  overlay on the same GPU the emulator is flooding. The touch controls are
  that overlay, so input stops, and the 5 s input timeout raises the ANR.
  The emulator's Turnip contexts are at KGSL priority 7 (this fork's hook,
  `kgsl_context_priority`, the compositor's ringbuffer) and with the title
  uncapped they never leave the GPU idle. The table at the top of this
  file already names the suspect ("GPU hangs with the compositor active").
  - **Being tried:** `kgsl_context_priority = 0` in the phone's config
    (the hook off). If the ANRs stop, 7 is wrong as a default once a title
    is uncapped: make it yield to the app's own UI (a lower priority, or a
    host frame limiter that leaves the GPU a gap each refresh).
  - The earlier "System isn't responding" on run 32 was the same flood
    with the recorder and a memory shortage on top.


- **The 60 FPS patch, measured (run 32, 2026-11-12 13:36).** With Canary's
  "60 FPS" patch on, the same early scenes run at 34, 42 and 43 frames a
  second (emulator frame counter over 10 s windows) against 21 to 27
  without it. So the title was waiting on its presentation interval, not
  out of CPU: at 30 it just misses 33 ms and falls to every third refresh.
  Uncapped, the profile changes: `GameThread`'s host thread is the busiest
  (24% of samples, about a core), the 3D Engine thread spins in one small
  guest function waiting for it (`sub_82B9BF90`, 10% of all CPU: a poll
  loop, `lr=82B9BF98` in earlier dumps), the GPU command thread 18%,
  Turnip 8%, and pipeline compilation threads 5% (stutter while new
  shaders compile). Guest code 60%, `libe.so` 17%, libc 11%. For 60: the
  game thread needs about a third more throughput, and its share of
  thunk, TLS resolver and scheduler calls is where to take it first.
  Whether the game plays at the right speed with the patch is the
  owner's to say.


- **The stall, read out of the stalled process (run 32, 2026-11-12 12:34;
  `device-logs/watch/20261112-123444`).** The new dump: the worker
  `XThread001B` is parked in `RtlEnterCriticalSection` (guest `lr=822006CC`,
  called from `8263B988 82EA9C78 82EAB0A8 8262D96C 8262DED0 8262DE18`) on
  the section's event `F800023C`, auto-reset, not signalled, the worker
  first in its queue, last set by GameThread's `RtlLeaveCriticalSection`
  (`lr=82195B10`) 112 s earlier. GameThread waits for the worker's "job
  done" event (`F8000234`, last set by the worker 111.9 s earlier; called
  from `822BA584 8229E400 8227613C 821D478C 82176298 821C9088 82277134
  822F48CC`). Then, with `run-as` and `/proc/<pid>/mem` on the frozen
  process (guest memory is the `xenia_memory` mapping at `0x800000000`):
  the section is at guest `83499138`, `lock_count` 1 (held, one waiter),
  `recursion_count` 1, `owning_thread` `30029020`, and that KTHREAD's
  thread id is 7: **GameThread**. So by the section's own record
  GameThread holds it while it waits for the worker, and the worker waits
  for the section.
  - **Reading of it (not proven): the owner field is wrong.** If the
    worker took the section while the emulator took it for GameThread,
    its next (nested) enter does not see itself as the owner and waits for
    itself. The owner came from `XThread::GetCurrentThread()`, a host
    thread-local; the fiber's own identity is in its control region
    (`r13`, `prcb_data.current_thread`).
  - **Change pushed to test it** (see the log's newest line for the
    build): the three critical section exports take the caller from the
    control region, and log `thread identity mismatch on critical section`
    (64 times at most) when the thread-local disagrees. If the stall is
    gone and the line appears, find who leaves the thread-local stale (a
    fiber resumed outside `GuestScheduler::SwitchTo`?) since every other
    `GetCurrentThread()` caller is exposed too. If the line never appears
    and it still stalls, the game really does hold the section across the
    wait and the question becomes why the worker needs it here and not on
    a console (the worker's wake latency under the cooperative scheduler).
- **The flash at scene cuts** (`device-logs/watch/full-1113/flick_a.jpg`):
  about five frames where the game's picture is sheared into horizontal
  bands with a green strip at the bottom, the touch overlay intact over
  it, then one half-wrong frame. At the cut from the intro flight to the
  first ground scene, every run. Looks like the front buffer shown with
  the previous one's layout (size, pitch or tiling) for a few frames after
  the title changes it: look at the swap path's front buffer texture.
- **Run 32 measured:** 21 frames a second ten minutes in. The busiest host
  thread (`Guest CPU 3`) is not doing a frame's work: 12.7% of it is
  `NtYieldExecution` (export 257) and 20% the guest/host thunk
  (`a64_host_code`), 9% `tlsdesc_resolver_dynamic`: a fiber spinning on
  the title's lock routine while it waits. 21 is also what a title locked
  to vertical sync gives when a frame just misses 33 ms (60/3). The
  bundled "60 FPS" patch (`be8 0x82B9C8EB = 1`, the presentation interval)
  is the cheap experiment that tells waiting from working: try it once the
  stall is out of the way.


- **The stall is a guest deadlock, caught in the act (fifth run, run 26,
  2026-11-12; `device-logs/watch/20261112-104003` on the desktop).** Four
  thread dumps on request, frames 2,384 to 3,169, all the same:
  `GameThread` blocked in `WaitForMultipleObjects` (`lr=822D75AC`,
  wait-all on one handle, `F80001DC`, epoch 14, no deadline), and
  `XThread001B` (created by GameThread at frame 1,283, entry `8262DDD0`)
  blocked on an event (`F8000240`, epoch 40) at `lr=822006CC`, which is
  inside the title's own lock routine `sub_82200688` (the one run 24's
  spinning thread was in: `RtlEnterCriticalSection`/`Leave` and a yield).
  Neither epoch moves, so neither object is ever signalled again; the
  64 ms backstop re-polls both and finds nothing. Everything else runs:
  27 frames a second of a still camera. Run 13's last dump shows the same
  pair in the same shape. So it is one intermittent fault, anywhere in the
  game: the worker waits for a lock that is never handed over, or a wake
  that is never sent. Read and found sound: the gate and its backstop
  (`RereadyBlocked`), `lwarx`/`stwcx.` (one CASAL against the captured
  value), `GetNativeObject` (under the global lock). Not found.
  - **Next on this:** in the dump, for each blocked thread, the waited
    object's type and whether it is signalled now, and the guest call
    chain (walk the PPC back chain from `r1`): that names the lock and its
    holder. `RtlEnterCriticalSection` is the first place to look
    (`xboxkrnl_rtl.cc`: it spins on the host without yielding, then waits
    on the section's own event).
  - **`guest_scheduler = false` cannot be tried from the config as it
    stands.** `kernel_flags.cc` has `UPDATE_from_bool(guest_scheduler,
    2026, 8, 1, 13, false)`: a config whose `defaults_date` is older
    (the phone's is `2026072714`) has `false` turned back into `true` at
    load. The "scheduler off" run of 2026-11-12 was therefore a normal
    run, and its getting past the intro was chance. To test it for real,
    set `defaults_date = 2026080114` in `[Config]` as well.
- **Run 26 measured:** 26.6 and 22.4 frames a second in the intro (the
  emulator's frame counter between two dumps; SurfaceFlinger's `--latency`
  returns no rows on this phone, so no percentiles yet). Profile:
  `getpagesize` and `clock_getres` are gone from it. The largest single
  cost outside guest code is now `tlsdesc_resolver_dynamic`, 3.6% of all
  CPU time (it rose with the never-inlined readers): the per-thread state
  wants one cheap home, the PPC context the shims already receive being
  the obvious one. Guest code 49%, `libe.so` 20%, libc 15%, Turnip 3.4%.


- **Fourth device run (23:00, run 24: the optimised core with the
  per-thread reader fix, Turnip).** Loads and plays: the `XObject::Wait`
  crash is gone. Then the scene at the trader (Murgo) never moved on: the
  picture stays live (20 to 36 frames a second, 4,500 draws a frame), no
  watchdog report, no host crash; holding LT (the prompt on screen) through
  adb changed nothing. Run 18 passed this scene, run 13 froze a little later
  with every game thread waiting. Not explained: the log has nothing,
  because the report of thread states only comes when no frame is presented.
  **Now it can be asked for:** `adb shell touch
  /sdcard/Android/data/<package>/files/compose/dump_threads` and within a
  second `xe.log` has every guest thread, what it runs or waits on. Do this
  first at the next stall, twice a few seconds apart.
- **Where the time goes (simpleperf, 12 s at that scene; kept on the
  desktop, `device-logs/run4/prof_*.txt`). The game is CPU-bound, not
  GPU-bound:**
  - JIT'd guest code 52%, `libe.so` 24%, libc 15%, Turnip 4.7%, the dynamic
    linker 2.7%.
  - By thread: one game thread 38% (a full core), the game's audio thread
    28% (all of it guest code: the title mixes its audio itself, hottest
    function `sub_82C62548` at 8% of everything), the GPU command thread
    22% (half a core), the rest small.
  - The busy game thread is a polling loop: `NtYieldExecution` (export 257)
    between `RtlEnterCriticalSection`/`Leave`. A fifth of its time is the
    yield itself: `YieldCurrentThread`, three `std::mutex` round trips, the
    fiber switch, `clock_gettime`, and until now `getpagesize` (3.8% of the
    thread) and `clock_getres` on every pass (both cached now).
  - `tlsdesc_resolver_dynamic` is 6.8% of that thread: every `thread_local`
    read in `libe.so` goes through the linker's slow resolver (a dlopen'ed
    library). Worth fixing for every title: one TLS block or a register
    for the scheduler's per-thread state. Not done.
  - `PpcVrsqrtefpLane` (a VMX instruction done in a C helper) shows on a
    third guest thread.
  - **So for 60 fps the lever is guest CPU cost (the JIT, the scheduler's
    yield path, the audio thread), not tiled rendering.** Frame time is
    not measured exactly yet: the frame counter gave 600 frames in 16.5 s
    and in 28.8 s at this scene.
- **Skipped draws, now named** (run 24's new log line): vertex shaders
  `C30A97D946FA2BE4` and `FB68A7F2301210E1`, host vertex shader type 6,
  six textures and three samplers against a slot layout of none; `layout
  stale flag true, pipeline handle null, translated true`, async shader
  compilation on. The draw arrives while the slot's pipeline is still being
  made (handle null) and the layout was never refreshed: wait for, or
  refresh, the layout before the bind instead of dropping the draw.

- **STOP: run 22's APK cannot load Fable II past character selection.**
  Second device run (2026-11-11 22:18, run 22, commit `60a088e`, the
  bundled Turnip in use: `driverID: MesaTurnip`, Mesa 27.0.0-devel). At
  frame 556 `xe.log` says `Host-side crash on fiber thread (handle
  0xF80000F4, guest tid 0x1A) ... guest lr 0x822D74D8. Halting fiber`. The
  thread is the one that opens `\dataudio`; with it halted the load
  screen runs for ever (tooltips cycle, 248,000 draws every 600 frames,
  nothing else). The host PC is in `libe.so` at offset `0x153BA84`:
  `xe::kernel::XObject::Wait(uint, uint, uint, ulong*)` + `0x75C` (from the
  APK's own symbol table and the process's maps). Run 13 (the core built
  Debug, -O2) passed this point with the same ISO.
  - **Cause found by disassembling run 22's `libe.so`, fix pushed (see the
    log's newest line for whether it is built).** `XObject::Wait` reads
    the thread's TLS base once on entry (`mrs x28, tpidr_el0` at
    `+0x1C`), calls `GuestScheduler::BlockCurrentThread`, then reads the
    current `XThread` through that same base (`ldr x29, [x28, x22]` at
    `+0x758`) and dereferences it. Guest threads are fibers the scheduler
    moves between host threads: after the switch the fiber was on another
    host thread, the base was the previous one's, the pointer there was
    null. ThinLTO inlined `XThread::GetCurrentThread` into the wait loop;
    the Debug build called it, so it worked. The optimised build stays:
    the readers of per-thread variables that fiber code uses are now
    `XE_NOINLINE` (`XThread::GetCurrentThread`, `GetCurrentFiberThread`,
    `IsInThread`, `cpu::Thread`'s, `ThreadState::Get`, `Fiber::
    GetCurrentFiber`, `current_thread_id`, the host `Thread::
    GetCurrentThread`, and the scheduler's CPU index through
    `CurrentCpu()`). **The same fault can be anywhere a `thread_local` is
    read directly on both sides of a fiber switch**: a new `Host-side crash
    on fiber thread` line is the sign, and the offset in it plus the APK's
    symbol table names the function (`device-logs` method above).
  - Run 18's APK (Turnip, the core as before) is on the phone meanwhile.
  - Good news in the same log: on Turnip none of the 2,253 skipped draws
    (`UpdateBindings`) appeared in 4,200 frames, and the KGSL priority hook
    is active.
  - The 4x target the MSAA patch leaves is one 640x1024 colour and depth
    pair: 18,600 of about 248,000 draws per 600 frames go to it.
  - The phone's pages are 4 KiB (`getconf PAGE_SIZE`): the 16 KiB fix-up
    does not apply to it.

- **Branch:** `claude/xbox360-emulator-architecture-eac8l7`. All work is on
  it. `master` is still upstream hakuX 1.2.0; nothing has been merged to it
  and there is no pull request.
- **First device run done** (2026-11-11, run 13's APK, Fable II GOTY, a
  Snapdragon 8 Elite Gen 5 phone: SM8850, Adreno 840, 16 GB, Android 17).
  The app, the 360 library and the game start; the game reached play, drew
  with flicker and glitches, then froze. What the logs say is under
  "First device run" below.
- **The bundled Turnip driver is the change waiting to be tried.** That run
  was on Qualcomm's stock driver: no Turnip package had been installed, so
  none of the Turnip-side work was in effect. The app now ships StevenMXZ's
  Turnip (v39) and uses it for the 360 core by default. Built on Actions:
  run 18, commit `8a8618a`
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43050816108),
  the APK is its artifact `hakuX-Boosted-debug-apk`, now signed with one
  fixed debug key so later builds install over it. Not run on a device yet.
- **The APK to install: run 22, commit `60a088e`**
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43099465293),
  artifact `hakuX-Boosted-debug-apk`. It holds everything below and
  installs over run 18's (same debug key). Its check that the 360 core is
  compiled -O3 with ThinLTO passed. Not run on a device.
- **Changed since run 18 (review pass; none run on a device).** Runs 19 to
  21 gave no APK: GitHub Actions trouble that evening (an upload that
  failed, runs queued without runners), not the build.
  The changes:
  - The 360 core is compiled in xenia's Release configuration (-O3,
    ThinLTO) in the debug APK too. Until now Gradle's debug variant built
    it as Debug: -O2, no link-time optimization. Every APK tested so far had
    the slower core. The Actions step "How the 360 core was compiled"
    prints the flags.
  - On hosts with pages over 4 KB the JIT's address fix-up for 0xE0000000+
    no longer branches (`a64_seq_util.h`). Check the phone:
    `adb shell getconf PAGE_SIZE`. If it says 16384, every guest load and
    store with a computed address carries that fix-up (four instructions),
    and the E0 view's 4 KB offset is worth a proper look (ROADMAP phase 4).
  - The game's surface asks for a 60 Hz (or 50) display mode
    (`EmulatorHostActivity.hintFrameRate`), for even pacing on 120, 144 and
    165 Hz panels.
  - Diagnostics only: `Pipeline layout behind its shaders` in `xe.log`
    names, once per vertex shader, the pipeline slot behind the skipped
    draws (stale flag, handle, modification, host vertex shader type); the
    MSAA line now says color or depth and the target's size.
- **The skipped draws were read for, not found.** Ruled out by reading:
  the layout key, the bindings publish (guarded, first translation only),
  the claim wait, the bind's reread of the slot's layout. Still open:
  the slot keeping a minimal layout with its stale flag clear. The new log
  line's flags decide between the storage-load path
  (`InitializeShaderStorage`, which never sets the flag) and the live one.
- **The "Disable MSAA" patch applied on run 13, and the title still made 4x
  targets.** Answered from that run's `xe.log` (kept on the desktop):
  module hash `4145F96D2DEE2AB5` (the GOTY file's), `Patcher: Applying
  patch for: Fable II(4D5307F1) - Disable MSAA` and the streaming fix, both
  sessions; all five patch files were seeded with the patch enabled. Then,
  at frame 1, `This title creates multisampled render targets (4x MSAA)`.
  So Canary's patch does not cover every target the game makes (or one made
  before the patched code runs). The patch is one byte (0x8238DF3F = 1, in
  the function that also holds the 1120 of the resolution patch). Two log
  lines on the next run settle it: the creation line's new detail (color or
  depth, size), and `Draws in frames A-B: N, of them M into multisampled
  targets` every 600 frames (first 40 windows). M at 0 once in play means
  the 4x targets are boot or loading screens only and tiled mode can be
  tried; M above 0 means the scene is still multisampled and tiled mode
  is not safe for the title.
- **Third device run (22:25, run 18's APK: Turnip, the core built Debug).**
  Fable II loads into play: no host-side crash, which confirms the Release
  configuration as what exposed the `XObject::Wait` fault. The skipped
  draws are back on Turnip (390 by frame 2,475, the same `VS 6t/3s` against
  an empty layout, `PM4_DRAW_INDX(1, 18, 2)`), so they are the emulator's,
  not the driver's. Slow: about 2,500 frames in 10 minutes including the
  loads. **The frame 9210 stall did not happen on Turnip:** the owner
  played on to frame 11,388 (18 minutes) and was still progressing; the XMA
  thread's warning pair appeared once (frame 3,162) and cleared; 392 draws
  skipped in all. The owner's eyes: playable, some stutter, an occasional
  texture glitch.
- **The UI work is on its own branch, `claude/xbox-ui`,** in a cloud
  session started from the desktop. It merges this branch in before each
  push; this branch takes it back once a build has been checked on the
  phone. Core work stays here.
- **The look the owner wants for the UI** (told to the desktop session; pass
  it to the UI branch): Xbox-inspired without any branded assets, and as
  polished as the open-source Cocoon launcher feels.
- **Wanted by the owner after the game runs smoothly: Fable II at 60 fps.**
  Canary's "60 FPS" patch is in the bundled catalog (off). It needs the
  frame time first: measure on the optimised core before turning it on.
- **Next for the owner's phone:** the APK of the first green run at or
  after `2c2c6ec` (the never-inlined per-thread readers on the optimised
  core). It is signed with the fork's key and installs over run 18.

## Watching a test run (`boosted/tools/device-watch.py`)

Written after the fourth run; **not yet run against the phone** (it was
unplugged). `start` records the phone's screen in rolling 60 s pieces;
`grab [secs]` takes the last seconds as still frames (ffmpeg), a
screenshot, `xe.log` with a summary, a thread dump on request, and frame
pacing from SurfaceFlinger (average, median, 95th and 99th percentile frame
time: the stutter the owner feels, as numbers); `fps` and `dump` do the last
two alone. The owner says "now" when a glitch shows, the session runs
`grab` and reads the frames. Check the first use: the layer name
`pacing()` picks, and that a piece closed by `pkill -2` plays.

## The performance work: what the owner wants and what the profile says

The owner's aim: the emulator's own overhead cut down so far that a
Snapdragon 8 Gen 2 reaches 60 fps in games and lesser chips hold a game's
native 30, for every title, with Fable II only the test. Nothing here is a
promise: each step is measured on the phone (`device-watch.py fps`,
simpleperf as in run 4) and kept only if the number moves.

Order, by what the run 4 profile showed (CPU-bound: guest code 52%, the
emulator 24%, libc 15%, the driver under 5%):

0. **How to take the thread-local cost out (worked out 2026-11-12, not
   written).** `libe.so` is dlopen'ed, so each `thread_local` read is a
   call to the linker's `tlsdesc_resolver_dynamic` (3 to 9% of the busy
   threads), and bionic refuses the initial-exec model in a dlopen'ed
   library. Bionic keeps one thread-pointer slot free for applications from
   API 29: `TLS_SLOT_APP`, slot 2, at `tpidr_el0 + 16` on arm64 (bionic's
   `tls_defines.h`; the app's minimum is 29). Plan: one small
   `HostThreadState` per host thread (current XThread, cpu::Thread,
   ThreadState, fiber, scheduler CPU, logging thread id), its pointer in
   that slot, read through an always-inline accessor whose `mrs` is a
   volatile inline asm so that it is re-read after a fiber switch (the
   hoisting that made run 22 crash cannot happen again). Then the
   XE_NOINLINE readers of 9e2d69d and f7ee729 become two instructions.
   Keep the plain `thread_local` path for other platforms.
1. **The scheduler's yield and wait path** (a fifth of the busiest game
   thread). Done so far: `page_size()` and the clock resolution cached.
   Next: the `thread_local` reads through `tlsdesc_resolver_dynamic` (6.8%
   of that thread: `libe.so` is dlopen'ed, so every read is a call into the
   linker; keep the scheduler's per-thread state behind one pointer, or in
   the PPC context the JIT already holds in a register); `std::mutex`
   three times per yield (`lock_` is one lock for all six CPUs); a yield
   that finds nothing else ready should return without the two fiber
   switches.
2. **Draws dropped while their pipeline compiles** (`Pipeline layout
   behind its shaders`: stale layout flag, null handle, async compilation):
   refresh the layout or wait instead of failing the draw. Missing
   geometry and flicker in any title that compiles shaders during play.
3. **The JIT.** Take the hottest guest functions from a profile
   (`perf-<pid>.map` names them: `sub_82C62548` is 8% of everything, the
   title's own audio mixer) and read what the A64 backend emitted for
   them: loads and stores through `ComputeMemoryAddress`, VMX operations
   that leave for C helpers (`PpcVrsqrtefpLane` showed), flag and
   condition register traffic. This is where a title-independent gain of
   the size the owner wants would have to come from; measure before
   believing any of it.
4. **eDRAM and resolves.** Not the limit in this scene (the GPU thread is
   half a core, Turnip 5%), so after the above; it becomes the limit when
   the CPU side stops being one. Tiled rendering needs the remaining 4x
   target (640x1024, colour and depth) gone first.
5. **The 60 fps patch** for Fable II (bundled, off) only once the frame
   time at 30 has headroom.

The stall at the trader (frames presented, script waiting, music only, no
speech starting) is separate: next occurrence, `device-watch.py dump`.

## The UI work (branch `claude/xbox-ui`): built, not run on a device

The owner's words: the Xbox 360 being a separate section with separate
ROMs is wrong; one "Xbox" emulator for both consoles, polished, with
on-screen controls and in-emulation settings (graphics, emulation,
controls). All three steps below are written and built on Actions.
**None of it has been seen running**: a cloud session cannot run the app.
Every "should" below is what the code is meant to do, to be checked on the
phone.

- **The APK to install: run 29, commit `351ccc9`**
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43121829736), artifact `hakuX-Boosted-debug-apk`.
  Same debug key as before, so it installs over the last build
  (`adb install -r`).
- **Branch:** `claude/xbox-ui`, cut from
  `claude/xbox360-emulator-architecture-eac8l7` and merged with it before
  each push (last merged: `47d02b7`). Nothing was pushed to the core
  branch. To take the UI work in once the phone agrees: on the core branch,
  `git merge origin/claude/xbox-ui`.
- **Builds, one per step:** step 1 run 25 (`8fb071a`), step 2 run 28
  (`3869b98`), step 3 run 29 (`351ccc9`). All green, including the check
  that the 360 core is -O3 with ThinLTO and the check of what the APK
  holds. Run 29's APK has all three steps and the core up to `f7ee729`
  (the thread dump and the last never-inlined readers).

### Step 1: the in-game menu of Xbox 360 games

Back (or a swipe back) pauses the game and opens the menu. It now has:
Show or hide controls, **Edit controls**, **Settings**, Resume, Quit.

- Edit controls: the touch controls editor over the paused game (dimmed).
  Landscape layout only in this mode; "Save" or "Cancel" returns to the
  menu.
- Settings: three sections, Graphics, Emulation, Controls. One row per
  setting; a tap (or A on a controller) moves the row to its next value.
  Under each row: "Applies now" or "Applies on the next start".
- Applies now (the core rereads these every frame; checked in the core's
  source, list in `emulator_xendroid.cpp` and `ui/pause/InGameSettings.kt`):
  show frame rate, frame rate limit, the VSync cap, keep aspect ratio
  (letterbox or stretch), skip draws while shaders compile, on-screen
  controls shown, and the touch controls' opacity, auto-hide and vibration.
- Applies on the next start: resolution scale, antialiasing, scaling,
  Turnip mode, render target path, occlusion query mode, readback resolve,
  guest scheduler, delay collapsing, game patches, title update.
- Every change is saved to the running game's own config
  (`config/<TITLE ID>.config.toml`), the file the per-game settings screen
  edits. The touch controls' three are saved with the controls' layout
  (all games).
- With a controller: D-pad or left stick moves, A chooses, B goes back.

What to check on the phone:

1. In Fable II press back: the menu shows the five options.
2. Settings opens, lists the three sections, scrolls; the highlighted row
   follows the D-pad if a controller is attached.
3. "Show frame rate" On: the FPS overlay appears behind the menu at once.
   "Frame rate limit" to 30: after Resume the game is capped at 30.
   "Keep aspect ratio" Off: the picture stretches to the screen's edges.
4. Change "Resolution scale, width" and "height" to 2x, quit, start the
   game again: it renders at the higher resolution (and slower). Then the
   game's long-press menu in the library, "Game settings": the same values
   show as overrides.
5. Edit controls: drag a button, Save, Resume: the button is where it was
   put.
6. If the settings page says "Settings are available once the game has
   started" on a game that is running, that is a bug: the title ID did not
   come through (`current_title_id`).

### Step 2: one library

hakuX's library lists both consoles' games from hakuX's games folders. The
"Xbox 360" button is gone, and with it XenDroid's own library and its
games folder setting (its content installer now uses hakuX's first
folder).

- An Xbox 360 game's card has an "XBOX 360" mark and its title ID, its
  real name and its icon. Found: disc images (`.iso`), ZArchives (`.zar`),
  folders with `default.xex`, GOD and XBLA packages.
- Long press (or Y on a controller) on a 360 game: Game settings, Patches,
  Add-ons and updates. These are the 360 side's screens; back returns to
  the library.
- **"All files access" is needed for 360 games** (the 360 core reads files
  by path; hakuX's folders are picked through Android's folder picker).
  When a folder holds a 360 game and the access is missing, the library
  shows a line with an "Allow" button; after allowing, coming back to the
  app lists again. The phone already gave this access for the old 360
  library, so the line should not appear there.
- A games folder on a provider other than the phone's own storage or an SD
  card (a cloud provider, say) cannot be mapped to a path: its 360 images
  are still marked and start, but without names, icons or the menu.

What to check on the phone:

1. The library shows Fable II with its name, icon and the "XBOX 360" mark,
   once, in the same list as any Xbox games. No "Xbox 360" button at the
   top.
2. Tap it: the game starts (as before, in the 360 core).
3. Long press it: the three entries open the right screens for Fable II
   (the Disable MSAA patch is listed under Patches), back returns.
4. If Fable II is missing: is its folder one of hakuX's games folders
   (Settings, Games)? The old 360 library had its own folder setting,
   which is no longer read. Add that folder in hakuX's settings.
5. The first listing after the update takes longer (each 360 image is
   opened once for its name and icon); later ones are quick.
6. Original Xbox games list, start and show their menus exactly as before.

### Step 3: one settings screen, one look

(Superseded on the branch `claude/ui-settings-tabs`: the Xbox 360 section is
a tab there, and the palette is no longer a copy. See "The UI refinement".)

- hakuX's Settings has an **Xbox 360** section, after Games: the 360
  core's settings (graphics, emulation, audio), touch controls, controller
  buttons, profiles, install content, the "All files access" state, about.
  Each opens the 360 side's screen; back returns to the settings.
- The 360 side's screens, and the in-game menu and prompts of 360 games,
  use hakuX's colours and follow hakuX's theme setting (Settings, Display,
  Theme: the phone's, light or dark). The palette is a copy
  (`x360/app/.../ui/theme/HostTheme.kt`): if hakuX's changes, change it
  there.

What to check on the phone:

1. Settings shows the Xbox 360 section; each row opens a screen and back
   returns to the settings.
2. Those screens are white with hakuX's green in the light theme and
   near-black in the dark theme, like hakuX's own. Switch hakuX's theme
   and open one again.
3. A 360 game's in-game menu follows the same theme.

### Known gaps, deliberately left

- The two cores keep separate settings, per-game settings and controller
  mapping: they share no code. The Xbox 360 section links to the 360's.
- The 360 side's screens keep their own layout (XenDroid's files), only the
  colours are hakuX's.
- XenDroid's library screen (`ui/library/GameLibraryScreen.kt`) is still in
  the tree, unreachable. Its extras are not in hakuX's library yet: ISO to
  ZArchive compression, the launcher shortcut, "install from disc" for
  discs that carry content (GTA V). The last one is reachable by hand:
  Settings, Xbox 360, Install content.
- The in-game settings rows only step forward through their values.
- Strings added by this work are English only (hakuX's own are in its
  resources).

## The UI refinement (branch `claude/ui-settings-tabs`): built, not run on a device

A cloud session's work on 2026-11-12, on its own branch cut from
`claude/xbox360-emulator-architecture-eac8l7` at `6c4d710`. Nothing was
pushed to the core branch and no file under `x360/emulator-core` is
changed. To take it in when the owner is ready to test both together: on
the core branch, `git merge origin/claude/ui-settings-tabs`.

- **The APK: run 41, commit `07a48c9`**
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43308622305),
  artifact `hakuX-Boosted-debug-apk`. It has the core as of `6c4d710`.
  (Run 40 was replaced by the next push before it compiled anything.)

**What changed**

1. **Settings are three tabs.** Global: games folders and "All files
   access", the controls of each console, the app's theme, the GPU driver
   of each console, updates, logs. Xbox: hakuX's emulation settings. Xbox
   360: the 360 core's 139 settings as rows in hakuX's style, in 16
   sections that open in place (the first, Display, starts open), with
   Profiles and Install content above
   and About below (those three still open a screen, as hakuX's hard disk
   row does). Storage is unchanged: hakuX's rows write the same
   preferences, the 360's rows go through XenDroid's own view model and
   repository to the same config file.
2. **The frame rate counter** of both consoles sits inside the display
   cutout, the rounded corners and the system bars, whichever way round
   the phone is held in landscape. The 360's can still be dragged.
3. **One look**, defined once (`x360/app/.../ui/theme/HostTheme.kt`,
   `SharedLook`) and worn by both sides: near-black surfaces in steps, one
   green accent, rounder shapes, heavier titles, and a green ring around
   whatever a controller has focused (rows, cards, tabs). The app is now
   dark until a theme is chosen. No image, icon, font or logo was added;
   the only console names on screen are the plain words "Xbox" and
   "Xbox 360".

**What was verified**

- Built: run 41 is green on the first compile, including its checks that
  the 360 core is -O3 with ThinLTO and of what the APK holds.
- Unit tests: `SettingsSchemaTest` (12), `SettingsModifiedTest` (4) and
  `ConfigValueShapeTest` (5) pass, run on a plain JVM in the cloud session
  (Actions does not run them). Two of `SettingsSchemaTest`'s were already
  failing on the core branch: its counts still said 136 settings after
  three were added (`38a60ed`, `9916218`). The counts are corrected in
  `b43c503`; the schema is untouched.
- **Not verified: anything on a device.** A cloud session cannot run the
  app. How the tabs, the rows, the counter and the colours look and behave
  is what the code is meant to do, nothing more.

**What to look at on the phone**

1. Settings shows three tabs. Every row that was there before is in one of
   them and does what it did. L1 and R1 on a controller step through the
   tabs.
2. Xbox 360 tab: "Reading the Xbox 360 core's settings" for a moment, then
   16 sections, "Display" open. Turn "Show debug overlay" on, leave the
   settings, start Fable II: the counter shows. Then the game's long-press
   menu, Game settings: still XenDroid's own screen, and it shows the same
   global values.
3. Global tab, GPU drivers, Xbox 360 games: says "Turnip, bundled with the
   app". Leave it as it is unless testing drivers.
4. The counter in Fable II: whole, top left, clear of the corner. Turn
   the phone the other way up: still whole. If it had been dragged
   somewhere before, it stays there unless that place is now cut off.
5. The look: dark with a brighter green. With a controller, the focused
   row has a green ring. If the phone was in light mode and the app is now
   dark, that is the new default (Settings, Global, App theme).

**Choices made that are the owner's to reverse**

- Dark as the default theme: one line in hakuX's `data/AppPrefs.kt` and
  one in `HostTheme.kt` (`HostLook.isDark`).
- The 360's sections start closed but for the first, in an order meant
  for a player (Display first, Logging last), under plainer names ("Audio"
  for APU).
  Order and names are in `HostSettingsPane.kt`; the schema is as it was.
- Rows show the ring and a wash instead of a ripple when pressed.

**Known gaps**

- Opening the settings now loads the 360 core's library into the app's
  main process (Android 10 and later), for the 360's GPU driver row.
- "Large tiles" was not done: it means restructuring the library screen,
  which this pass was told not to do.
- The 360 side's other screens (per-game settings, patches, content,
  profiles, key map, about) keep XenDroid's layout; they get the colours
  and the focus ring only.
- New strings are English only, as before.

## First device run (2026-11-11): what the logs say

The logs are the 360 side's session ZIP
(`Android/data/<package>/files/compose/logs/`) and logcat.

- **Driver: Qualcomm's, not Turnip.** `xe.log`: `driverID:
  QualcommProprietary`, `vulkan_lib_path` empty. `TU_DEBUG=sysmem` and the
  KGSL priority hook do nothing on it. Judge rendering only on Turnip.
- **The freeze is the guest stalling, not a crash.** At frame 9210, about
  525 s in, `GuestScheduler: no guest frame presented in 2000 watchdog
  ticks`: '3D Engine' running at `lr=82B9BF98`, 'Texture Streamer' at
  `82CC207C`, 'XThread000F' (the XMA thread, `F8000070`) not switching
  fibers at `82C45A48` with `XmaContext: kick still unserviced` before it.
  The same pair of warnings appeared and cleared at frame 4154. Not
  explained yet; see whether it survives the driver change first.
- **2,253 draws were skipped**, all one kind: `UpdateBindings: pipeline
  layout counts (VS 0t/0s, PS 0t/0s) do not match the draw's binding counts
  (VS 6t/3s, PS 0t/0s)`. A vertex shader with six textures drawn with a
  pipeline whose layout has none ("the minimal layout from untranslated
  shaders", `vulkan_command_processor.cc`). Missing geometry for certain;
  a candidate for Fable II's ground (vertex texture fetch is how a
  heightfield is displaced). Independent of the driver.
- **The "reboot" was Android killing the app.** The phone did not restart
  (kernel uptime unbroken). When the frozen game was left, both processes
  were killed (`critical_mem_pressure`) and the system restarted the
  emulator activity, which booted the game again. The phone was short of
  memory for the whole session (65 background apps killed in 8 minutes);
  what the emulator process itself held was not captured.
- **No touch controls at first: fixed.** The phone's built-in
  `uinput-xiaomi` device reports gamepad keys, so the 360 side took it for a
  controller and started with the touch controls hidden. It now uses hakuX's
  test of a real controller (`settings/TouchOverlayDefault.kt`).
- **The title is 4x MSAA** (the log line this fork added), so tiled mode
  needs the MSAA patch first, as expected.

## What to do next, by what the device test says

| Result | Next |
|---|---|
| (now) Fable II on the bundled Turnip | Check `xe.log` says `driverID: MesaTurnip` (not `QualcommProprietary`); then whether the flicker, the skipped draws and the frame 9210 stall are still there |
| The skipped draws remain | Find why the pipeline layout is the minimal one for a vertex shader with textures (`UpdateBindings`, `vulkan_command_processor.cc`); suspect the tessellated-draw path |
| The stall remains | The XMA thread that stops yielding (`lr=82C45A48`) and the 3D Engine's wait (`lr=82B9BF98`): disassemble both guest addresses |
| The app or the 360 library does not open | Logcat first. Suspects: `Application.ensure_init` (the 360 side's start-up inside hakuX's app), R8 on the 360 Kotlin, the manifest merge (`boosted/android/x360-ui`) |
| Fable II boots, ground drawn | Go to performance: tiled mode (step 1 of `FABLE2.md`), then the frame rate patches |
| Ground missing | `tessellation_debug` 1, 2, 3 and the logged vertex shader decide it (table in `FABLE2.md`) |
| Tiled mode holds with MSAA off | Ship it as the title's default (`boosted/x360-titles/config`), then repeat the recipe on the next titles |
| Tiled mode hangs | Read `xe.log` for the multisample line (a pass still multisampled); stay on sysmem; look at pass count and the JIT instead (`docs/ROADMAP.md` phase 4) |
| GPU hangs with the compositor active | Check the `kgsl draw contexts at priority 7` log line; `kgsl_context_priority = 0` rules the hook out |

## What this fork is, in five lines

- hakuX (original Xbox, upstream `rfandango/hakuX`) with XenDroid (Xbox
  360, upstream `rfandango/XenDroid`, a Xenia Edge fork) under `x360/` as a
  git subtree. One Android app, two cores, each in its own process.
- The goal is not the merge itself: it is to make Xbox 360 on Snapdragon
  as good as hakuX made the original Xbox. Fable II is the first target.
- hakuX's side is left alone so upstream merges stay trivial (the 1.3.0
  merge had no conflicts).
- Everything of this fork's own is in `boosted/`, `.github/workflows/` and
  the `hakuX-Boosted:` comments in the two upstream trees.
- Read `docs/STATUS.md` for what is verified, `docs/ARCHITECTURE.md` for
  the layout and merge procedures, `docs/CROSSOVER.md` for what does and
  does not carry over from hakuX, `docs/ROADMAP.md` for the plan.

## Findings that should not be rediscovered

- **There is no hakuX-style leap to copy.** hakuX's jump was low-level to
  high-level emulation; Xenia has been high-level from the start, and both
  projects share an author, so most small tricks are already in XenDroid.
  Gains have to be measured on a device, one at a time.
- **Tiled rendering is the first lever.** XenDroid defaults Turnip to
  `sysmem` (untiled, slower) because tiled mode hangs the Adreno 830, and
  its own notes show the hang is on multisampled passes only. A title with
  MSAA patched off may run tiled.
- **Fable II's ground is tessellated** (continuous quad patches), not a
  depth problem per se, and may already be fixed: XenDroid's fix to the
  patch index of auto-indexed tessellated draws (2fb3c036, 5 July) is newer
  than its notes on the missing ground.
- **Zero-copy guest memory is not a quick win.** It needs hakuX's
  first-write barrier to be correct (the GPU reads at submit time), and its
  gain is unproven on GPU-bound titles. `docs/ROADMAP.md` phase 3.
- **Xenia Canary's patch set is already bundled** (501 files, the
  `x360/patches/xenia-canary` submodule). `python3
  boosted/tools/title-info.py <title>` pools what is known about a title.
- **An outside roadmap was checked against the code (2026-11-11): do not
  redo it.** A Gemini-written "XenoDroid" plan and two "patches" (a
  zero-times-infinity multiply guard for the SPIR-V translator, a byte-swap
  elimination pass and a VMX dot product for the ARM64 backend). Both
  patches are pseudocode against APIs this tree does not have, and what
  they aim at is already here: the Shader Model 3 "0 * anything = 0" rule
  (`gpu/spirv_shader_translator_alu.cc`), float controls where the driver
  has them, CTX1 decoding (`texture_load_ctx1`), the float24 depth and PWL
  gamma paths, byte swaps cancelled and folded into loads and stores
  (`SimplificationPass::CheckByteSwap`,
  `memory_sequence_combination_pass.cc`), VMX dot products inline in NEON
  with the console's double-precision and overflow rules
  (`a64_sequences.cc`), FFmpeg's XMA decoder built with NEON, and
  `dynamic_rendering_local_read` for in-pass resolves. Wrong for this tree:
  the assembler is xbyak_aarch64 (not oaknut), fragment shader interlock is
  absent on Turnip (not slow), `VK_QCOM_tile_memory_heap` is the stock
  driver's only, and the page problem is 16 KB host pages against the 4 KB
  offset of the 0xE0000000 view (not 64 KB against 4 KB). Its numbers have
  no source. Worth keeping from it: an automated benchmark run for the
  adb loop, per-title "GPU saver" and aspect ratio patches, and its
  structure (one frontend, two separate cores), which is what this fork
  already is. All three are in `docs/ROADMAP.md`.
- **XDK documentation is not used.** The copies in circulation are leaks;
  Xenia's source, Canary's data and Mesa's freedreno cover the same ground.
- **XenDroid's Kotlin frontend has no stated licence** (`docs/STATUS.md`).
  Settle it with upstream before distributing builds.

## How to work here

**Building.** Only GitHub Actions builds the app (`.github/workflows/
boosted.yml`): every push to a `claude/**` or `boosted/**` branch builds
hakuX's core, then the APK with both cores, and checks what the APK holds.
A warm run takes about 12 minutes. By hand on a desktop:
`boosted/README.md`.

**Installing a build:** debug APKs are signed with the fork's own debug key
(`boosted/android/signing`) from the build after run 13, so each installs
over the last (`adb install -r`). Run 13's APK was signed with a runner's
throwaway key and has to be uninstalled once.

**In a cloud session:**
- The container cannot reach Google's Android SDK host, so there is no
  local NDK or Gradle build. Push and read the Actions run.
- It cannot download Actions artifacts either. The job's "What the APK
  holds" step is the evidence of what was built.
- `gh` is for search only (`gh api -X GET search/commits`, `search/issues`).
  Searching `repo:rfandango/XenDroid` commits is how the Fable II patch was
  found; `git fetch <sha>` from a public repository works.
- A newer push cancels the run before it.

**On a desktop:** the same branch. With the Android SDK installed you can
build locally and use adb; the device etiquette in upstream's own
`HANDOFF.md` (repository root, hakuX's) applies to hakuX's devices.

**Rules of this fork:**
- Never claim a speed-up that was not measured on a device. `STATUS.md`
  separates built from run; keep it true.
- Changes to upstream files are small, marked `hakuX-Boosted:`, and listed
  in `docs/ARCHITECTURE.md`. hakuX's core is not modified.
- What a title is known to need goes in `boosted/x360-titles` so it applies
  without the user doing anything: `config/` (per-game settings), `patches/`
  (our own patches, shipped enabled), `enable/` (names of bundled Canary
  patches on by default). Anything that can hang or that changes how a game
  plays stays a choice.
- Commit messages follow upstream's style: `area: what changed and why`,
  with the reasoning in the body and "Not built or run yet" where true.
- UK English, no em dashes.

**Taking upstream in:**
- hakuX: `git fetch https://github.com/rfandango/hakuX.git master && git
  merge FETCH_HEAD`; if `android/README.md`'s dependency table changed,
  update `boosted/tools/fetch-native-deps.sh` (it checks commits).
- XenDroid: `git subtree pull --prefix=x360
  https://github.com/rfandango/XenDroid main --squash`, then
  `python3 boosted/tools/sync-x360-gitmodules.py`, then compare XenDroid's
  Gradle files and manifest with `boosted/android/` (`docs/ARCHITECTURE.md`).
  Currently at hakuX `a7cdb5a` (v1.3.0 + 14) and XenDroid `779680a`.

## Settings this fork added (360 side)

| Setting | Section | Default | What |
|---|---|---|---|
| `kgsl_context_priority` | Vulkan | 0 (was 7) | Turnip's KGSL contexts on the compositor's ringbuffer (hakuX's hook) when 1 to 15; off by default since it starved the app's own UI with a title uncapped |
| `guest_threads_above_helpers` | Kernel | true | Dispatch threads and the GPU command thread at nice -8, pipeline compile threads at nice 8 (written 2026-11-12, not built or run) |
| `async_shader_translate_on_draw` | GPU | true | A draw's untranslated shaders are translated on the draw thread, so the draw is recorded instead of refused |
| `vulkan_async_wait_fullscreen_draws` | Vulkan | true | With `vulkan_async_skip_draws`: full-screen passes still wait for their pipeline (written 2026-11-12, not built or run) |
| `vulkan_async_wait_fullscreen_draws_ms` | Vulkan | 1500 | The longest one submission waits for those |
| `tessellation_debug` | GPU | 0 | Logs tessellated draws and their vertex shader; 1 no culling, 2 no depth test, 3 both |
| (log line) | | | "This title creates multisampled render targets", a warning when tiled mode is on |

## The bundled Turnip driver

- `boosted/drivers/turnip.lock.json` pins a release of
  `StevenMXZ/Adreno-Tools-Drivers` and each package's SHA-256: the plain
  upstream-Mesa builds, one for Adreno 8xx and one for 7xx (not the
  "Patched" builds, not the One UI ones).
- `boosted/tools/fetch-turnip.py` downloads them for the build (checked
  against the lock); Gradle packs them as `assets/gpu-drivers`.
- `x360/app/.../core/BundledDriver.kt` unpacks the one for the phone's GPU
  and sets `vulkan_lib_path` to it, once. A driver the user installed, or
  the system driver once they chose it, is left alone. hakuX's core keeps
  its own choice of driver; the package is only added to its driver list.
- `.github/workflows/turnip-sync.yml` moves the lock to the newest release,
  commits it and starts a build. Its daily schedule only runs once the file
  is on the repository's default branch; until then run it by hand
  (Actions, "Turnip sync"). Every build also notes when a newer release is
  out.
- The packages are a third party's prebuilt binaries. Run a new one on a
  device before passing a build on.

## Session log

Newest first. One or two lines each: what changed, what was verified.

- **2026-11-12 (desktop, 22:30).** Runs 38 and 39 built; run 39 (hand-off
  change plus full-screen waits) on the phone stalled after 5518 frames.
  The title's code read from the stalled process: the main thread joins
  its worker's per-frame job while holding a lock the job needs, so the
  stall is the worker being late, not a lock fault. Cause of the lateness
  not measured yet. Full-screen waits: 37, 3.0 s, glitches reduced, not
  gone.

- **2026-11-12 (desktop, late).** Second cold run with the setting false:
  the owner saw fewer glitches and 1 to 2 second freezes on entering new
  areas, which is the dropped draws turned into waits; some glitches
  remain (cause not seen yet). The game then stalled in the guest
  deadlock; read from the process, it is a critical section's set taken
  back by the thread that made it. Fix pushed (contender queued before
  its increment), not built or run.

- **2026-11-12 (cloud, UI branch `claude/ui-settings-tabs`).** Settings as
  three tabs with the 360's settings as rows, the frame rate counter inside
  the screen's safe area on both consoles, one look for both sides. Built
  green (run 41, `07a48c9`); the settings unit tests run and pass on a
  plain JVM after their stale counts were corrected. Not merged, not run
  on a device.

- **2026-11-12 (desktop, night).** Run 37 on a cold shader cache: the
  attract video is fixed (verified by eye), the `UpdateBindings` drops are
  gone (0 logged), the first-run glitches remain. Traced to the app's
  `vulkan_async_skip_draws = true` dropping draws at replay while Turnip
  takes a median 56 ms and up to 754 ms per pipeline, in step with the
  size of the SPIR-V the translator emits (up to 282 KB a shader). Not
  verified: the A/B with the setting false, and where the driver spends
  the time (both set up for the next run).

- **2026-11-12 (desktop, evening).** Run 35 (core and UI merged) on the
  phone: the UI's first look, 27 minutes without a stall with the 60 FPS
  patch off at a steady 30, the attract video fault captured and traced to
  stale pages after a decommit (physical allocations now invalidate; not
  built). The KGSL hook is off by default.

- **2026-11-12 (desktop, afternoon).** Merged `claude/xbox-ui` into this
  branch at the owner's request, so one APK carries the core work and the
  UI work. Only this file conflicted. Not built or run when written: the
  UI has never been seen running.

- **2026-11-12 (desktop, midday).** Run 32 on the phone: the dump's new
  detail and the frozen process's memory give the stall's mechanism (top
  of "Right now"). The critical section exports now take the caller from
  the control region and report a mismatch. Not built or run yet.

- **2026-11-12 (desktop, morning).** Run 26 on the phone with
  `device-watch.py` (works: frames, dumps; not SurfaceFlinger's pacing).
  The stall caught as a deadlock of GameThread and a worker (top of "Right
  now"). The build's signing key was never used: now injected and checked
  (`28b9806`, not built when written). No core change.

- **2026-11-12 (cloud, UI).** The UI work on `claude/xbox-ui`: the
  in-game menu's controls editor and settings, one library for both
  consoles, an Xbox 360 section in hakuX's settings and hakuX's colours on
  the 360 screens (section above). Verified: runs 25, 28 and 29 green, the
  APK artifact of run 29 exists. Not verified: anything on a device; no
  screen of this work has been seen.

- **2026-11-11 (desktop, 23:40).** `device-watch.py` (rolling screen
  capture, grab, frame pacing, thread dump) and the performance order
  above. The tool compiles; not run against the phone. No core change.

- **2026-11-11 (desktop, 23:15).** Fourth device run profiled on the phone
  (top of "Right now"). Added the thread dump on request, cached
  `page_size()` and the clock resolution, made the last per-thread readers
  never inline (the global lock's cached thread id, the shim string buffer,
  `SetCurrentThread`, `ThreadState::Bind`). Not built or run yet.

- **2026-11-11 (desktop, end).** Third device run read (run 18 loads into
  play, skipped draws are the emulator's). The package step skips mirrors
  that do not answer. Wrote the UI work up for a cloud session. Not
  verified: the build of `2c2c6ec`, the fix on a device.

- **2026-11-11 (desktop, late).** Found run 22's crash (a TLS base kept
  over a fiber switch, top of "Right now") and made the per-thread readers
  never inline, keeping -O3 and ThinLTO. Not built or run yet.

- **2026-11-11 (desktop, night).** Second device run, run 22 on the
  bundled Turnip: the driver and the touch controls work as built; a
  host-side crash in `XObject::Wait` halts a loading thread (top of "Right
  now"). Logs kept on the desktop (`device-logs/run2`). No code changed.

- **2026-11-11 (cloud, late).** Added the multisampled-draws line (above)
  after the desktop's MSAA finding. Run 19 confirmed the review-pass
  commit compiles and packages; no APK from it (upload failed at GitHub),
  later runs queued without runners. The workflow's concurrency group was
  renamed on a wrong guess that run 19 was holding it; harmless. Verified:
  run 22 green at `60a088e`, including the -O3 and ThinLTO check and the
  APK upload.

- **2026-11-11 (desktop, later).** Answered the MSAA patch question from
  run 13's log (above): applied, 4x targets regardless. The phone's adb
  authorisation lapsed, so run 18 or later is not installed yet and the
  page size is unchecked. No code changed.

- **2026-11-11 (cloud, night).** Review pass for performance and misses:
  the 360 core to Release configuration, the branchless 16 KB page fix-up,
  the frame rate hint, the two diagnostics above. Verified: see whether the
  Actions run for this commit passed and what its compile-flags step shows.
  Not verified: anything on a device.

- **2026-11-11 (cloud, evening).** Evaluated the owner's Gemini roadmap
  and two patch sketches against the tree (finding above); nothing in them
  to apply. ROADMAP.md gained the benchmark run, per-title GPU savers and
  two JIT measurements. No code changed.

- **2026-11-11 (cloud, later).** Checked what the desktop session pushed:
  all five commits are on the main branch, none on a side branch, and run
  18 built them (after three runs lost to the runner's package mirror).
  Noted the MSAA patch question above. No code changed.

- **2026-11-11 (desktop, phone on adb).** Read the first device run's logs
  (section above). Bundled StevenMXZ's Turnip v39 as the 360 core's default
  driver on Adreno 7xx and 8xx, with the lock file, the fetch script and the
  sync workflow. Verified: the lock's hashes against the downloads. Not
  verified: the build, and anything on the device.

- **2026-11-11 (cloud).** Created the fork's structure: XenDroid imported
  as the `x360/` subtree, the two Gradle modules and the bridge that put
  both cores in one app, Actions build. Ported hakuX's KGSL priority hook.
  Merged hakuX 1.3.0+14. Recovered Fable II's streaming crash patch from
  XenDroid's history; per-title defaults (config, patches, enabled Canary
  patches) with Fable II's. Added the MSAA log line and
  `tessellation_debug`. Merged the control point tessellation fix from a
  second session. Verified: builds on Actions (run 13, `8345f8d`). Not
  verified: anything on a device.

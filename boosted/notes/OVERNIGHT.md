# Overnight sessions, 2026-11-12 to 13: who owns what

The desktop session that led the work is closed overnight. Six cloud
sessions run without it. Nobody is coordinating you live: this file is the
coordination. Read it before you change anything, and obey the ownership
table even where your own brief is looser.

## Rules for every session

- Base: branch `claude/xbox360-emulator-architecture-eac8l7` at `7648e9c`
  (built green as run 44). Never push to that branch. Push only to your own.
- Edit only the files in your row. If the right fix is in someone else's
  file, do not make it: describe it in your notes, with the exact change,
  for the desktop session to apply at merge.
- Do not edit `boosted/HANDOFF.md`, `boosted/docs/ARCHITECTURE.md` or this
  file. Write to `boosted/notes/<your name>.md`: what you built, the
  settings to switch on and what to look for in `xe.log`, every upstream
  file changed and why, what the build could not verify, what is next.
- Every new behaviour sits behind a setting whose default is today's
  behaviour, so each branch can be merged without changing what runs.
- The only check you have is a green GitHub Actions build of your branch.
  Nothing can be run on a device. Say so; never claim a speed-up, a fix or
  a visual result.
- Commit and push after each working step. If a session hits its usage
  limit it may be restarted after 00:50 and must be able to resume from its
  branch and its notes file alone, so keep the notes file current as you
  go, with a "done / in progress / next" list at the top.
- UK English, no em dashes, commit style `area: what changed and why`,
  upstream-file changes marked with a `hakuX-Boosted:` comment.
- No game data in the repository (no shader microcode, no code dumps).

## Ownership

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless
they start with another directory.

| Branch | Task | Files it alone may edit |
|---|---|---|
| `claude/pixel-stand-in` | Stand-in pixel shader for pipelines still compiling | `gpu/vulkan/vulkan_pipeline_cache.*`, `gpu/vulkan/vulkan_command_processor.*`, `gpu/vulkan/vulkan_shader.*`, `gpu/shaders/**`, `gpu/gpu_flags.*` (its own settings only) |
| `claude/spirv-slimming` | Smaller SPIR-V from the shader translator | `gpu/spirv_shader_translator*`, `gpu/spirv_builder.*`, `gpu/spirv_shader.*`, `gpu/gpu_flags.*` (its own settings only), build files for SPIRV-Tools |
| `claude/thread-placement` | Survey of thread priority, core placement and pacing; ADPF, placement policy, per-second core log | new files `base/host_perf_android.*`; the smallest hook it needs |
| `claude/guest-scheduler` | The guest scheduler's own cost: yield storm, timed sleeps, missed pokes | `kernel/guest_scheduler.*`, `kernel/xobject.cc`, `kernel/xobject.h`, `kernel/xevent.*`, `kernel/xsemaphore.*`, `kernel/xmutant.*`, `kernel/xboxkrnl/xboxkrnl_threading.cc` |
| `claude/host-thread-state` | The current-thread lookups off the dynamic TLS resolver (`TLS_SLOT_APP`) | `kernel/xthread.*`, `cpu/thread.*`, `cpu/thread_state.*`, `base/threading.h`, `base/threading_posix.cc`, `base/fiber*`, a new `base/host_thread_state.*` |
| `claude/cpu-translation` | Study of the PowerPC to ARM64 translator, with safe prototypes | `cpu/backend/a64/**`, `cpu/compiler/**`, `cpu/hir/**`, `cpu/ppc/**` |
| `claude/ui-settings-tabs` | Settings tabs, counter inset, look | `android/app/**`, `x360/app/**` (UI code only) |

`gpu/gpu_flags.*` is shared by two sessions: add settings at the end of the
file only, one block each, so the merge is trivial.

Nobody owns `kernel/xboxkrnl/xboxkrnl_rtl.cc`, `gpu/command_processor.cc`,
`memory.cc` or the build workflow: leave them alone.

## What was measured, in one place

Phone: Xiaomi, Snapdragon SM8850 (2 prime + 6 performance cores), Adreno
840, Turnip v39, Android 17. Test title: Fable II. The aim is every title.

- Guest code is about 76% of the six dispatch threads' time; the emulator's
  own library 14%; libc 7%; the dynamic linker's TLS resolver 2.4% overall
  and about 9% of the busiest thread.
- The busiest guest thread: about 20% in host-call thunks, about 13% in the
  yield path.
- The scheduler: 370,000 fiber switches a second at the median, 1.4 million
  at the peak; each dispatch thread wakes on a timer every 1 to 2 ms;
  about one missed poke a minute (a signalled waiter not re-readied until a
  timed look).
- Host scheduling: the dispatch thread of guest CPU 1 ran 6.3 s and waited
  15.1 s for a core in 200 s; a guest thread on it ran up to 137 ms after
  its signal. Fable II deadlocks when that happens (it joins a worker each
  frame while holding a lock the worker needs). First answer, in `7648e9c`,
  untested: dispatch threads and `GPU Commands` at nice -8, pipeline compile
  threads at nice 8.
- Shaders: Turnip takes a median 56 ms and up to 976 ms per pipeline, in
  step with the SPIR-V's size (up to 282 KB a shader); about 200 pipelines
  in the first ten minutes of the title.
- The title's own audio mixing thread takes about 28% of a core.

Added 2026-11-13 00:55, from the 200 s profile of the six dispatch threads
(1 kHz, Fable II, first minutes of play). Shares are of all six threads'
time unless a thread is named.

- One guest function, `sub_82B9BF90`, is 19.1% of everything and 52.7% of
  the 3D Engine thread (guest CPU 3): a wait loop that spins. For the
  scheduler session: this is the largest single saving on offer, a
  spinning guest wait that should park. Find what it polls (a memory
  word, see the `MemoryPollPark` lines) and why it is not parked already.
- `sub_82C62548` is 6.1% of everything and 29.9% of guest CPU 5: the
  title's audio mixer. For the CPU translation session: a good specimen
  of hot translated code (tight float and vector loops).
- Host-call thunks (`a64_host_code`) 3.7% overall, 6.8% of GameThread.
- `tlsdesc_resolver_dynamic` 2.4% overall, 4.9% of GameThread.
- Translating at run time is visible on GameThread: `SimplificationPass::Run`
  3.8%, `SimplifyBitArith` 1.0%, plus register allocation, context
  promotion and constant propagation, about 6% of that thread; 1.6% + 0.4%
  + 0.3% + 0.3% + 0.3% overall. `SimplificationPass` alone costs more than
  the rest of the passes together: for the CPU translation session, look
  at why (it reruns to a fixed point).
- Scheduler mutex traffic: `pthread_mutex_lock` 1.4% and unlock 1.0%
  overall (2.9% and 2.0% of GameThread), `YieldCurrentThread` 0.6% self,
  clock reads about 1%.
- `mprotect` 0.4% overall, 1.2% of the 3D Engine thread (memory watches).

Detail and evidence: `boosted/HANDOFF.md`, "Right now".

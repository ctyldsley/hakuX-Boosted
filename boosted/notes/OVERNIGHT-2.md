# Overnight sessions, 2026-11-13 to 14: who owns what, and what was measured

The desktop session is closed overnight. Four cloud sessions run without it.
Nobody coordinates you live: this file does. Read it, `CLAUDE.md` and
`boosted/HANDOFF.md` ("Right now" first) before you change anything.

## Rules for every session

- Base: branch `claude/integration-1113` (not the default branch). Check it
  out first. Never push to it. Push only to your own branch.
- Edit only the files in your row. If the right fix is in someone else's
  file, do not make it: write the exact change in your notes for the desktop
  session to apply at merge.
- Do not edit `boosted/HANDOFF.md`, `boosted/docs/ARCHITECTURE.md` or this
  file. Write `boosted/notes/<your branch name>.md`: done / in progress / next
  at the top, the settings, the log lines to compare on device, every
  upstream file changed and why, what the build could not verify.
- Every new behaviour sits behind a setting whose default is today's
  behaviour, unless your row says you own the defaults.
- The only check is a green GitHub Actions build of your branch (`claude/**`
  builds on push; a push cancels a build in progress, so let one finish).
  Nothing can be run on a device. Never claim a speed-up, a fix or a visual
  result.
- Commit and push after each working step and keep your notes current, so
  the work can be resumed from the branch alone.
- UK English, no em dashes, commit style `area: what changed and why`,
  upstream-file changes marked with a `hakuX-Boosted:` comment.
- No game data in the repository: no shader microcode, no code dumps, no
  disassembly of a title. Guest addresses and percentages are fine.
- Be ambitious. The owner wants leaps: first-run smoothness that is close to
  seamless on every title, and headroom for higher resolution, 60 frames a
  second and weaker phones. Prefer the change that fixes a class of title
  over a tweak for Fable II. Say plainly what you could not finish.

## Ownership

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless
they start with another directory.

| Branch | Task | Files it alone may edit |
|---|---|---|
| `claude/shader-first-run` | Nothing drawn wrong and no long frame while shaders compile; less compile work; shaders kept and prepared | `gpu/**` |
| `claude/guest-waits` | The emulator's own cost on guest threads, busy waits, the skew bound at start-up | `kernel/**`, `base/host_thread_state.*`, `base/threading*`, `base/host_perf.*`, `cpu/thread*`, the spin-wait passes under `cpu/compiler/passes/` whose names contain `poll` or `spin`, and their call sites in `cpu/backend/a64/a64_seq_memory.cc` |
| `claude/translator-wide` | Pinned registers on thousands of functions, cheaper translated code, instruction tests in CI | the rest of `cpu/**`, `.github/workflows/**` for a test job only |
| `claude/defaults-config` | The proven settings as the defaults; a config that cannot be lost; the settings screens | `x360/app/**`, `android/app/**`, `x360/emulator-core/src/main/java/**`, the bundled `default_config.toml`, and the default value (only) of a setting in any `.cc` file, listed in its notes |

`claude/defaults-config` changes default values in files other sessions own.
It touches only the default literal on the setting's own line; the other
sessions do not reflow those lines.

## What was measured on 2026-11-13 (phone: Snapdragon SM8850, Adreno 840, Turnip, Android 17)

Evidence is on the desktop PC under `device-logs/` and summarised in
`boosted/HANDOFF.md`. The numbers that matter:

### Shaders (Fable II `4D5307F1`, Banjo-Kazooie: Nuts & Bolts `4D5307ED`, cold cache)

- Fable II, first 13 minutes: 267 real pipelines, 34.7 s of driver compile,
  104 over 100 ms, worst 824 ms. Banjo, first 6 minutes: 819 real pipelines,
  206.6 s of compile, 499 over 100 ms, 136 over 500 ms, worst 1,287 ms; in
  its opening 45 s, 355 pipelines needing 105.8 s between four compile
  threads, 8 s with no frame, then 5 frames a second with frames near 200 ms
  easing to 50 ms as real pipelines replaced placeholders.
- Placeholders (interpreter vertex shader, 51 KB of SPIR-V, with the stand-in
  pixel shader): Fable II 176 in 2.4 s (worst 257 ms, 9 over 100 ms); Banjo
  739, about 1.6 ms each.
- Draws still skipped in Fable II with `vulkan_placeholder_pipelines`,
  `_ps_mode = 2`, `_interpreter_geometry`, `_interpreter_translated_vs`,
  `_depth_only_interpreter`, `_late`, `_translate_first` on and
  `_async = false`: 604 (357 vertex shader not translated, 247 no
  placeholder). Reasons by shader pair: not translated yet 30, vertex shader
  fetches textures 20, loaded inline (`IM_LOAD_IMMEDIATE`) 11, tessellated
  10, loops 4, primitive expanded in the vertex shader 3.
- `vulkan_placeholder_async = true` is worse: 8,281 draws skipped, 7,298 of
  them while the placeholder compiled on a creation thread.
- Look ahead: 237 pairs seen ahead, first pipeline asked for 833 ms later on
  average (the mean includes a 94 s outlier; the median is not known).
  Speculation as built: 74 made, 14 hits, 2 ready in time, 193 misses, 60
  written off, 125 refused by the caps, 10.2 s of compile.
- The first two minutes of Fable II hitch as much with all of this as
  without (15 seconds with a frame over 100 ms; 12 in the baseline). What
  is drawn improved; when the real pipelines arrive did not.
- `spirv_optimize` has never run on the device.
- The shader caches on disc after Banjo: pipeline cache 46 MB, shader store
  1.2 MB, pipeline state list 52 KB.

### The profile (Fable II, warm cache, 180 s at 1 kHz, every thread, run 90 with the settings above, the skew bound and the translator's settings on)

Share of all samples: Guest CPU 3 29.6%, Guest CPU 0 29.1%, Guest CPU 5
14.7%, GPU Commands 13.9% (0.46 of a core), Guest CPU 4 2.8%, nothing else
over 2.5%; the compile threads 0.04% (warm cache).

- **Guest CPU 0 (GameThread, 0.97 of a core) is only 52% translated code.**
  The rest is the emulator: `libe.so` 25%, libc 13%, the dynamic TLS
  resolver 8.2% (`tlsdesc_resolver_dynamic`, the largest single symbol on
  the thread), vdso 1.5%. By symbol: `__aarch64_cas2_acq` 5.0%, the dispatch
  loop (`GuestScheduler::EnsureStarted` lambda) 3.7%,
  `__aarch64_ldadd8_relax` 3.0%, `YieldCurrentThread` 2.3%, `CurrentCpu`
  2.0%, `pthread_mutex_lock` + unlock 3.5%, `set_current_thread_id` 1.7%,
  `clock_gettime` 3.1% with its vdso half, `a64_host_code` 1.6%,
  `EnqueueReady` 1.1%, `jump_fcontext` and `FcontextFiber::SwitchTo` 1.6%.
  The largest guest function is `sub_82276C30` at 8.1%. The per-second line
  of earlier runs showed about 575,000 `NtYieldExecution` round trips a
  second, and the lean yield cut fiber switches from 966,000 to 8,700 a
  second. So GameThread spends a large part of its core yielding and going
  through the dispatcher, and the title's real main-thread work is well
  under a core.
- **Guest CPU 3 (3D Engine, 0.99 of a core) is 61.6% in one function,
  `sub_82B9BF90`**, 93% translated code, almost no calls into the emulator.
  The function is a busy wait: a short counted loop of no-op register moves
  (the console's pause idiom), then loads of memory words and a compare, and
  round again. It reads like the title's Direct3D waiting for the GPU (a
  read pointer or counter it compares against). `MemoryPollPark` and the
  spin-backoff passes did not park it (`guest_scheduler_park_poll_loops`
  parked nothing).
- `guest_scheduler_park_yield_spinners` parks about 9 times a second at
  2 ms: the yielder's runs are ended by its own short waits every 1.5 to
  3 ms (`BlockCurrentThread` clears the run). The threshold is now
  `guest_scheduler_yield_park_after_us`; lower values are untested.
- Guest CPU 5 (audio, 0.49 of a core): `sub_82C62548` 25.7%,
  `sub_82CD6010` 7.8%, then a long tail of mixer functions.
- GPU Commands: `libe.so` 55.5%, Turnip 24.6%, libc 18.8%. Top symbols:
  `__memmove_aarch64_nt` 7.8%, `mprotect` 7.1%, `UpdateBindings` 4.1%,
  `IssueDraw` 3.2%, `WriteRegisterRangeFromRing` 2.9%, `LookAheadRange` 2.8%
  (the look ahead's own cost), `ExecutePacket` 2.2%, `ConfigurePipeline`
  1.9%.
- `host_thread_state_slot` (the current-thread lookup without the TLS
  resolver, branch `claude/host-thread-state`, merged) has never been
  switched on on the device.

### The scheduler bound (`guest_scheduler_skew_bound_us = 2000`, cap multiple 100)

- At frame 23,665 of a Fable II run the phone was at its frequency floor and
  packed the dispatch threads (in one second G0 waited 660 ms, G1 506 ms,
  G3 600 ms for a core); the worker `XThread001B` ran 116.6 ms after its
  start signal; the title carried on. Earlier builds deadlocked at 64 ms.
  One instance.
- Cost: 27 s to the first frame against 8 s without it (in loading 1,221
  holds, 35.3 s held: the progress gate holds the other CPUs while
  GameThread is in host code or the translator). In play 0.8% (6.7 s held
  in 792 s).

### The translator

- `pin_guest_registers`: 17 functions pinned of 24,576 considered (9,085
  had loops) in Fable II, 10 of 12,288 in Banjo. 0 verify mismatches. It has
  barely been exercised.
- `simplification_skip_unchanged_blocks`: 44% of block walks skipped, 0
  mismatches.

### Heat

- The phone lowers its cores' frequency limit as it warms, from 2.75 GHz to
  1.44 GHz (maximum 3.63) on the performance cores, in every run over about
  five minutes. Two dispatch threads at a full core each, mostly waiting, is
  most of the heat the emulator makes.

### The config (the desktop session's own mistake, and what it exposed)

- An in-place edit left the phone with no config file. At the next launch
  the 360 side copied `default_config.toml` in
  (`ConfigStore.ensureLiveFileExists`), which signed the player's profile
  out (no saves shown, cannot save) and dropped every tuned setting; the
  owner called that run a massive regression. So: the defaults are far
  worse than the tuned config, and a lost config silently costs the player
  their profile.
- The tuned config that works is in `boosted/HANDOFF.md` ("Settings") and,
  in full, at the top of any `xe.log` (`CONFIG DUMP`).
- Settings tabs (hakuX's `SettingsScreen.kt`, `boosted/SettingsTabs.kt`):
  switching between Global and Xbox left the previous tab's rows drawn; a
  fix (`key(shown)`) is in the build but has not been confirmed on screen.
- `host perf` reports `GPU missing`: it does not find the GPU command
  thread (its name on the device is `GPU Commands (0`, cut at 15
  characters).

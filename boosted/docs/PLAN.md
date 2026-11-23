# The plan from 2026-11-23

Adopted by the owner on 2026-11-23, from an outside review of a five-part briefing on the project (the review's own
files stay outside the repository). It replaces per-game investigation as the way work is chosen. Every session
reads this before choosing work.

## Why the approach changed

Chasing one game's bug at a time does not scale to a library, and the owner cannot play every game to every point.
Work is now chosen from faults and ceilings that apply to every game, found by hands-off tests:

- **xsdk probes** (`xsdk/probes` branch, `.github/workflows/xsdk-probes.yml`): homebrew test titles whose logs on a
  real console are known. `tools/run-xsdk.sh` runs them on the phone unattended; `tools/xsdk-compare.py` compares
  each with the console's log. First full run (build 149, 61 titles): `device-logs/xsdk/report.md` outside the
  repository.
- **Normalised timing:** `tools/run-one.sh` now writes `perf.txt` (fps, big-core clock, fps per GHz, frame ms at
  2 GHz; `tools/perf-summary.py`) and waits for the battery to cool to `COOL` degrees when that is set.

## Rules

1. No new experimental setting while earlier ones in the same area are unmeasured on the phone. Each setting is
   measured, then made the default or deleted.
2. Nothing is a fix or a speed-up until it is measured on the phone (with fps per GHz and temperature) or passes an
   xsdk comparison with the console.
3. Hands-off first. When the owner must play, bundle two to four runs, each saying beforehand what it proves.
4. The repository rules in `boosted/HANDOFF.md` still hold.

## Phase 0: ground truth (now)

- Build 149's diagnostics: whole-executable translation timing on Gears (hands-off); the owner's two door runs,
  A with defaults (stall watchdog on) and B with `[a64] a64_guest_memory_order = 1` plus a few campaign loads.
- Audit every fork setting that defaults off and was never measured on the phone (translator overhaul steps 1 to 4,
  `host_thread_state_slot`, `spirv_optimize`, `memory_watch_*`, `vulkan_pipeline_libraries`,
  `ppc_mcrf_copies_field`); delete failed ones (`shader_ahead_speculate`).

## Rewrite 1: correctness and stability

- **Self-written code.** `probe_exec` and `xsdk_test` (stops after 90 of 685 checks) crash writing into a code
  section. Two causes, read from the code:
  1. `XexModule` maps `XEX_SECTION_CODE` and read-only data read-only unless `writable_code_segments` or
     `allow_plugins` (`cpu/xex_module.cc`, "Setup memory protection"). On the console the write succeeds: xsdk's
     `.jit` pool is a Code section that reports protection `0x20` and written code runs
     (`ref/xsdk/docs/HARDWARE.md`, milestone 55).
  2. `icbi` is emitted as a no-op (`cpu/ppc/ppc_emit_memory.cc`, `InstrEmit_icbi`), so code rewritten after its
     first translation would keep running the old translation. The console check rewrites code and expects the new
     result.
  The fix needs both: code pages writable as on the console, and `icbi` (or a write watch on translated code
  pages) dropping the translated functions that cover the line.
- Kernel faults from the probes: mixed wait-all that includes a thread that ends never completes;
  `NtProtectVirtualMemory` returns `0xC0000022`; freed and unmapped pages are not returned to the kernel's counts;
  ordinal `0x36D` missing; `KeSave`/`KeRestore` floating point and vector state.
- If build 149's run B implicates memory ordering: a cheaper production form (acquire/release loads and stores,
  skipping stack-relative and read-only accesses; to be measured) and clearing reservations on context switches;
  our own multi-thread probes for ordering and waits.
- Translator and shader correctness: `rldic` and `mcrf` tests; the `probe_loop_break*` and `probe_hiz_pass`
  differences; XMA 192-sample carry.

## Rewrite 2: wasted cores and heat

The phone throttles from about 2.75 to 1.44 GHz within five minutes while about 1.5 cores spin (one guest
spin-wait, yield storms, dynamic TLS at 8.2% of the game thread, scheduler lock contention). Park spin loops by the
memory word they poll, collapse pause idioms, reach host thread state through `PPCContext` instead of TLS, split the
scheduler lock. Measure with fps per GHz and the sustained clock.

## Rewrite 3: shader compile stutter

Monolithic pipelines take 100 to 1,287 ms each on Turnip. Measure the `claude/pipeline-libraries` branch
(`VK_EXT_graphics_pipeline_library`), consider `VK_EXT_shader_object`, measure `spirv_optimize`, and replace the
hundreds of stand-in variants with a few precompiled pipelines that never compile on the GPU command thread.

## Rewrite 4: GPU command thread and ahead-of-time translation

- Per draw, the GPU side costs 0.739 us a cube against the console's 0.285 us (`probe_gpu_draw_cost`), and the
  same probe shows per-draw texture changes not applied (0 of 100 cubes textured, 100 on the console). Fable II's
  command thread is at 97% of a core at about 6,600 draws a frame. Cut `mprotect` and copy costs, batch per-draw
  memory requests, skip unchanged bindings; check GPU trace replay for a hands-off Fable II benchmark; real 1x
  targets so Turnip can render tiled when MSAA is patched off.
- Measure and promote the translator overhaul passes, then the persistent whole-executable code cache, informed by
  `aot_translate_measure`.

# Architecture: two cores, one app, two upstreams

## The rule

hakuX and XenDroid each keep moving upstream. This fork has to take both
in, repeatedly, so:

1. hakuX's tree stays where upstream has it, and this fork changes as few of
   its lines as it can. Each changed place is listed below.
2. XenDroid is a git subtree under `x360/`. This fork's changes to it are
   ordinary commits on top, each small and marked `hakuX-Boosted:` in a
   comment at the place it touches.
3. What belongs to neither upstream lives in `boosted/` (and
   `.github/workflows/boosted.yml`).

## How the app is put together

```
android/                      hakuX's Gradle project (upstream)
  app/                        the hakuX app: library, settings, NativeCoreActivity (:native process)
    .../boosted/X360.kt       the bridge to the 360 side: library, launch, the settings' 360 rows (this fork)
    .../boosted/SettingsTabs.kt  the settings' three tabs (this fork)
    .../boosted/Look.kt       the app's one look, handed to hakuX's theme (this fork)
    .../boosted/SafeArea.kt   the screen's safe area, for hakuX's frame rate counter (this fork)
x360/app/.../core/HostApp.kt  what the 360 side offers the app it is embedded in (this fork)
boosted/android/
  x360-core/build.gradle.kts  library module over x360/emulator-core (CMake -> libe.so, JNI classes)
  x360-ui/build.gradle.kts    library module over x360/app (the 360 side's screens, EmulatorHostActivity)
  x360-ui/src/main/           its manifest: no launcher entry, no Application class
x360/                         XenDroid, also still buildable on its own (x360/settings.gradle)
```

The two modules hold no sources of their own apart from a manifest and one
string: their source sets point into `x360/`. XenDroid's own Gradle files
(`x360/build.gradle`, `x360/app/build.gradle`,
`x360/emulator-core/build.gradle`) are not used by the combined build and are
left as upstream has them, so XenDroid can still be built standalone from
`x360/` to tell a regression of this fork's from one of upstream's.

At run time:

| | hakuX (Xbox) | 360 side |
|---|---|---|
| Process | `:native` | `:emu` |
| Activity | `NativeCoreActivity` (SDL3) | `xendroid.compose.EmulatorHostActivity` |
| Native library | `libhakuxcore.so` (prebuilt into `jniLibs`) | `libe.so` (built by Gradle) |
| Settings, saves | hakuX's preferences and hard disk image | `Android/data/<package>/files/compose/` |
| Vulkan driver loader | adrenotools | adrenotools (the same revision and patch) |

Nothing is shared between the cores at run time. What they share is build
inputs: one copy of adrenotools' hook libraries is packed (both cores build
them from the same patched revision, `app/build.gradle.kts` picks the first).

hakuX's library is the app's only library. hakuX's code reaches the 360 side
through `android/app/src/main/java/com/rfandango/haku_x/boosted/X360.kt`, and
that file reaches it through one object of the 360 side's,
`xendroid.compose.core.HostApp`. The one other place hakuX's package names
the 360 side is the look: `boosted/Look.kt` and `boosted/SettingsTabs.kt`
take the theme and the focus ring from `xendroid.compose.ui.theme`
(below, "One settings screen, one look").

- `LibraryViewModel.refresh` calls `X360.scanLibrary` after its own scan.
  The 360 titles in hakuX's games folders are found by the 360 side's own
  scanner (`GameLibraryRepository.scanRoots`: ISO, ZArchive, XEX folders,
  GOD and STFS packages), which also reads their names, icons and title
  IDs. An image hakuX's scan listed too is the same card with the 360's name
  and icon; the others are added as cards with `file://` URIs. The cards are
  marked (`X360Badge`) and their long-press menu is the 360 side's
  (`X360GameMenu`: per-game settings, patches, add-ons and updates, each one
  of the 360 side's screens shown on its own by `HostApp.show` in
  `xendroid.compose.MainActivity`).
- The 360 side reads real paths, hakuX's folders are SAF trees. A tree of
  the external storage provider maps to a path (`X360.realPath`), and
  reading it needs "All files access": the library asks for it when there
  is a 360 title to show (`X360AccessBanner`), and lists again when it is
  given. Without it only the images hakuX's scan listed can be told apart,
  and they show under their file names. Android 11 and later.
- The 360 core's library is loaded into the app's main process only when
  the folders hold a 360 title (a walk by names first, `X360.hasTitles`).
- The 360 side's own library screen (`ui/library/GameLibraryScreen.kt`) and
  its games folder setting are no longer reachable. Its files are left as
  upstream has them.
- `LauncherActivity.startCore` asks `X360.launchIfXbox360` first. A file is
  an Xbox 360 title if it is an XEX, a ZArchive, an STFS package, or a disc
  image whose root directory holds `default.xex` and no `default.xbe`. Those
  go to `EmulatorHostActivity`; everything else goes to hakuX's core as
  before. This also covers frontends (ES-DE, Daijisho) that launch hakuX with
  a 360 image.

The 360 side is offered on Android 10 and later (its library uses ELF TLS).
hakuX itself still installs on Android 9.

## Every place this fork changes an upstream file

hakuX (`git diff upstream/master -- . ':!x360' ':!boosted' ':!.github'`
shows exactly these):

| File | Change |
|---|---|
| `.gitmodules` | The 360 side's 42 submodules appended (generated: `boosted/tools/sync-x360-gitmodules.py`) |
| `android/settings.gradle.kts` | `:x360-core` and `:x360-ui` included |
| `android/build.gradle.kts` | Their two plugins declared |
| `android/app/build.gradle.kts` | The dependency on `:x360-ui`; `pickFirsts` for the hook libraries |
| `android/app/src/main/AndroidManifest.xml` | `tools:overrideLibrary` for the 360 modules' minSdk |
| `android/app/.../LauncherActivity.kt` | One call to `X360.launchIfXbox360` in `startCore` |
| `android/app/.../ui/library/LibraryScreen.kt` | Three places: `X360AccessBanner()` above the list, `X360Badge` in a card, `X360GameMenu` for a 360 title's long press |
| `android/app/.../ui/library/LibraryViewModel.kt` | `refresh` merges `X360.scanLibrary`'s titles, names and icons into its list; `onResume` also lists again when `X360.accessChanged()` |
| `android/app/.../ui/settings/SettingsScreen.kt` | The three tabs: inserted lines marked `tab:` group upstream's sections by tab (`SettingsTabsFrame`). Global holds only what both consoles share (games folders, theme, updates, logs); Xbox holds this core's sections with its controls and GPU driver; Xbox 360 is `X360SettingsTab` alone. No row is on two tabs. |
| `android/app/.../ui/HakuXApp.kt` | `nav.popBackStack()` replaced by `nav.backOnce()` (at the end of the file): a second back tap during the screen's exit emptied the stack and left a black window. |
| `android/app/.../ui/theme/Theme.kt` | `HakuXTheme`'s body is one call to `BoostedTheme` (the shared look); upstream's colours are left in place, unused |
| `android/app/.../data/AppPrefs.kt` | `theme` is dark until one is chosen (upstream: the phone's) |
| `android/app/.../NativeCoreActivity.kt` | Two calls to `keepInSafeArea`: the frame rate counter and the hitch monitor clear of the cutout and rounded corner |
| `android/app/src/main/res/values/themes.xml`, `colors.xml` | `Theme.HakuX`'s window background dark (upstream: white); the two greens of the framework's widgets are the shared look's accent |
| `android/app/.../NativeCoreActivity.kt` | Three more marked lines, one after each readout is made (`wearReadoutLook`): the frame rate counter, the stutter monitor and the "building shaders" line wear the shared in-game look (`claude/defaults-config`, merged 2026-11-14) |

XenDroid (`x360/`): search for `hakuX-Boosted:`. At the time of writing:

| File | Change |
|---|---|
| `emulator-core/.../third_party/libadrenotools/*` | hakuX's KGSL context priority patch |
| `emulator-core/.../xenia/ui/vulkan/vulkan_instance.cc` | The `kgsl_context_priority` cvar |
| `emulator-core/.../xenia/gpu/vulkan/vulkan_pipeline_cache.cc`, `.h` | Tessellated triangle and quad lists (control-point-indexed, discrete or continuous) get 3 / 4 control points per patch and the 3cp / 4cp control shader, as on D3D12; they had 1. Also `tessellation_debug`'s no-culling bit |
| `emulator-core/src/main/cpp/CMakeLists.txt` | Hook libraries as dependencies of `emu` |
| `emulator-core/.../Application.java`, `DocumentsProvider.java` | `ensure_init` / `attach`, so the class need not be the process's Application |
| `app/.../MainActivity.kt`, `EmulatorHostActivity.kt` | Call `ensure_init`; seed the per-title defaults; install the bundled Turnip driver. `MainActivity` shows one screen on its own when asked (`HostApp.show`) and shares its once-a-process flag with `HostApp.startSession` |
| `app/.../data/GameLibraryRepository.kt` | `scanRoots`: several folders, and a test for which disc images are the 360's |
| `app/.../updater/updater.kt`, `ui/library/GameLibraryScreen.kt` | XenDroid's updater off when embedded |
| `xenia/kernel/xthread.cc`, `guest_scheduler.cc`, `cpu/thread.cc`, `thread_state.cc`, `base/threading*.cc` | The readers of per-thread variables are never inlined: a fiber resumes on another host thread, and an inlined reader keeps the old thread's TLS base (the optimised build's crash in `XObject::Wait`) |
| `xenia/gpu/vulkan/vulkan_pipeline_cache.cc`, `.h`, `vulkan_command_processor.cc` | `vulkan_async_wait_fullscreen_draws`: with draws of unready pipelines dropped, the pipelines of full-screen passes (rectangle lists, unindexed draws of at most four vertices) are still waited for at the submission boundary, up to `vulkan_async_wait_fullscreen_draws_ms` |
| `xenia/gpu/gpu_flags.cc`, `gpu/vulkan/vulkan_command_processor.cc` | `async_shader_translate_on_draw`: a draw's untranslated shaders are translated on the draw thread so the draw is not dropped |
| `xenia/memory.cc` | Physical memory that is committed invalidates the GPU caches' copy of it (pages a draw saw decommitted were left marked valid for ever) |
| `xenia/kernel/guest_scheduler.cc`, `.h`, `xobject.cc`, `.h`, `xthread.h`, `xboxkrnl/xboxkrnl_rtl.cc` | Measurement: every satisfied cooperative wait is counted by the delay from its signal to the waiter running, every contended critical section by the time waited; 16 ms and over is logged by thread; totals in the once-a-second counters and in every thread dump |
| `xenia/kernel/guest_scheduler.cc`, `gpu/command_processor.cc`, `gpu/vulkan/vulkan_pipeline_cache.cc` | `guest_threads_above_helpers`: the guest CPU dispatch threads and the GPU command thread run above normal priority, the pipeline compile threads below |
| `xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc` | A critical section's owner comes from the caller's control region; a contender is queued on the section's event before it increments `lock_count`, so the owner's set cannot be taken back by the owner |
| `app/.../settings/TouchOverlayDefault.kt` | hakuX's test of a real controller, so a phone's built-in pseudo-gamepad doesn't hide the touch controls |
| `emulator-core/.../cpp/emulator_xendroid.cpp`, `xendroid_emu.cpp`, `emulator.h`, `java/xendroid/compose/Emulator.java` | Three JNI calls for the in-game menu: `live_setting`, `set_live_setting` (the cvars the core rereads every frame, a fixed list) and `current_title_id` |
| `app/.../core/EmulatorSession.kt` | Their Kotlin wrappers |
| `app/.../ui/pause/PauseMenuPanel.kt`, `EmulatorHostActivity.kt` | The in-game menu gains "Edit controls" and "Settings": pages of the menu (`MenuPage`), each with its own `PanelNav` |
| `app/.../gamepad/GamepadEditorScreen.kt` | An `inGame` mode: leaves the game window's orientation and system bars alone, shows the game behind |
| `app/.../core/Embedded.kt`, `core/TitleDefaults.kt`, `core/BundledDriver.kt`, `core/HostApp.kt`, `ui/pause/InGameSettings.kt`, `ui/theme/HostTheme.kt`, `ui/settings/HostSettingsPane.kt`, `ui/SafeArea.kt` | New files |
| `app/.../ui/theme/Theme.kt` | `xendroidTheme` hands over to `HostTheme` when embedded: the app's one look (`SharedLook`, defined in `HostTheme.kt` and worn by hakuX's screens too) and hakuX's light or dark setting |
| `app/.../FpsOverlay.kt` | The frame rate counter's area is the window less the display cutout, the rounded corners and the system bars (`ui/SafeArea.kt`); a stored place outside it is brought back in |
| `emulator-core/src/main/java/xendroid/emulator/Emulator.java` | `Config.close_config_file` writes through `ConfigFiles` (whole file, with a backup) (`claude/defaults-config`, merged 2026-11-14, as are the rows down to the default literal) |
| `emulator-core/src/main/java/xendroid/compose/Application.java` | The cached copy of the bundled defaults is written whole |
| `app/.../settings/ConfigStore.kt` | Opens go through `ConfigGuard`; the two silent fallbacks to the bundled defaults are gone |
| `app/.../settings/GameSettingsRepository.kt` | The rebuild of a per-game config is built in memory and written whole |
| `app/.../settings/SettingsSchema.kt`, `SettingDescriptions.kt`, `TouchOverlayDefault.kt` | Two fallbacks show what the core runs (`vulkan_in_pass_resolve`, `framerate_limit`), one row removed ("Readback memexport"), one renamed, one accessor |
| `app/.../core/ProfileBootstrap.kt`, `TitleDefaults.kt`, `HostApp.kt` | The profile check, whole-file writes, the GPU driver row once on its tab |
| `app/.../ui/profile/ProfileManagerViewModel.kt` | Tells the guard the player's choice of profile |
| `app/.../EmulatorHostActivity.kt` | The guard before the core starts, and its toast |
| `app/.../data/GameMetadataCache.kt` | Names by title ID (for the shader cache rows) |
| `app/.../FpsOverlay.kt`, `ui/pause/PauseMenuPanel.kt`, `ui/disc/DiscSwapPanel.kt`, `ui/messagebox/GuestMessageBoxPanel.kt`, `ui/keyboard/GuestKeyboardPanel.kt`, `ui/panel/GuestPanelOption.kt` | The shared in-game look (`GameOverlayLook`): one dimming, one counter, a ring for the highlighted option |
| `app/.../ui/keymap/KeymapScreen.kt`, `ui/profile/ProfilesScreen.kt`, `ui/content/ContentManagerScreen.kt`, `ui/settings/PerGameSettingsScreen.kt`, `ui/settings/SettingsScreen.kt` | Divider lines under rows removed |
| `emulator-core/.../compose/ConfigFiles.java`, `app/.../settings/ConfigGuard.kt`, `app/.../core/ShaderCaches.kt`, `app/src/test/.../ConfigFilesTest.kt` | New files |
| `xenia/gpu/vulkan/vulkan_pipeline_cache.cc` | The default literal of `vulkan_async_skip_draws` is `true`, as the bundled config ships it |
| `xenia/base/host_thread_state.h` | `scheduler_cpu` in use; `HostThreadStateInUse()` (`claude/guest-waits`, merged 2026-11-14, as are the rows down to `a64_seq_memory.cc`) |
| `xenia/base/host_perf.cc` | The GPU command thread is matched by the start of its name (the kernel keeps 15 characters) |
| `xenia/cpu/thread_state.h` | `owner` |
| `xenia/kernel/xthread.h`, `.cc` | `XThread::FromContext`; the owner set where a `ThreadState` is made; the new `SchedulerLinks` fields |
| `xenia/kernel/guest_scheduler.h`, `.cc` | The lookups without the TLS resolver, `TryEmptyYield`, `guest_scheduler_park_waits` and its two times, `PollSiteVisit`, `guest_scheduler_skew_host_progress` |
| `xenia/kernel/xboxkrnl/xboxkrnl_threading.cc` | `NtYieldExecution` takes its context |
| `xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc` | `RtlLeaveCriticalSection` reports a release with nobody queued |
| `xenia/kernel/kernel_state.cc` | `EmulateCPInterruptDPC` reports the interrupt routine having run |
| `xenia/cpu/compiler/passes/memory_poll_park_pass.h`, `.cc` | `park_pause_poll_loops`, `MarkPausePollLoops`, the site's flag |
| `xenia/cpu/backend/a64/a64_seq_memory.cc` | The `SPIN_BACKOFF` sequence: the poll site's call and thunk, the handler pointer; the backoff thunk passes its context. `src1.offset` carries the loop's guest address in its upper half for flagged sites; the x64 backend (not built for the phone) reads the whole value and must not have `park_pause_poll_loops` on without the same mask |
| `xenia/cpu/ppc/ppc_context.h`, `xenia/kernel/xthread.h` | park-hot-loops: `PPCContext::poll_site_skip` (read and written inline by poll and delay sites), `SchedulerLinks::poll_refusals`, `poll_backoff_level` |
| `xenia/cpu/compiler/passes/memory_poll_park_pass.h`, `.cc`, `xenia/cpu/backend/a64/a64_seq_memory.cc` | park-hot-loops: `park_poll_site_backoff`, `park_poll_loops_fixed_addresses`, `park_delay_sleds`, `park_one_block_poll_loops`; delay sites (`kSpinBackoffDelaySite`) and the inline skip counter |
| `xenia/gpu/texture_util.h` | `GetTiledAddressLowerBound3D`: the depth mask is `~(kTextureTileDepth - 1)` (was `~kTextureTileDepth`); found by comparing with rexglue-sdk |
| `xenia/gpu/vulkan/vulkan_pipeline_cache.h`, `.cc`, `vulkan_command_processor.h`, `.cc` | standin-device-lost: `vulkan_dynamic_state_check`, `vulkan_standin_dynamic_state_mask`, `vulkan_standin_shared_forms`, `vulkan_dynamic_color_state_fix` (all off by default) |
| `xenia/gpu/spirv_shader_translator_alu.cc` | `cube` written to `.x` or `.w` without `.y` produced a module with a missing id (`claude/shader-first-run`, merged 2026-11-14, as are the next four rows) |
| `xenia/gpu/vulkan/vulkan_shader.h`, `.cc` | `GetUcodeProgram`: the prepared microcode of a shader for the guest shader interpreter, version 2 |
| `xenia/gpu/vulkan/vulkan_pipeline_cache.h`, `.cc` | `vulkan_placeholder_interpreter` and `vulkan_placeholder_interpreter_tessellation`, their modules, the choice of module and of the stand-in's locations, the new skip reason; in `EnsurePipelineCreated` the tessellation evaluation stage takes the placeholder's module when there is one |
| `xenia/gpu/vulkan/vulkan_command_processor.h`, `.cc` | Which draws the interpreter may stand in for (tessellated ones with the setting), the program and its per-draw word in the bool and loop buffer, the second `UpdateBindings` |
| `xenia/gpu/shaders/ucode_program*.glsl*`, `gpu/ucode_program.h`, `.cc`, `gpu/first_run_rig/*` | New files: the interpreter's shaders, its program builder, and the rig that checks it against the translator off the device |
| `xenia/memory.cc`, `memory.h` | The `memory_watch_*` settings; the log line (`LogWatchStatsIfDue`); registration of the views with userfaultfd; arming ahead and bridging in `PhysicalHeap::EnableAccessCallbacks` and `EnableAccessCallbacksInner`; `WatchHostProtect` and `WatchHostKernelUnprotect`; `TriggerCallbacks` (`from_fault`, counters, lifting by ioctl); `QueryRangeAccess` stops at the first unreadable page; `armed_ahead` and `ahead_spurious` in `SystemPageFlagsBlock` (`claude/memory-watches`, merged 2026-11-14, as are the next three rows) |
| `xenia/base/memory.h`, `memory.cc`, `memory_posix.cc`, `memory_win.cc` | `WatchCounters` and its switch; the three `WriteWatch*` functions (the userfaultfd wrapper on POSIX, stubs on Windows); counting in `Protect` |
| `xenia/base/exception_handler.h`, `_posix.cc`, `_win.cc` | `SetNoStackOverflowRange`; the timing wrapper around the signal handler (`ExceptionHandlerCallbackTimed`), which is what is now installed |
| `xenia/base/memory_watch_test.cc` | New file, the test of the watches. The build skips `*_test.cc` and the file is empty without `XE_MEMORY_WATCH_HARNESS` |
| `xenia/gpu/spirv_shader_translator_fetch.cc`, `spirv_shader_translator.cc`, `gpu_flags.h`, `.cc` | `spirv_tfetch_vector_signs` (the vector form of a fetch's sign step, beside the switch); the assertion in `PWLGammaToLinear` (`claude/shader-first-run` at `b718ad5`, merged 2026-11-14 into `claude/integration-1115`, as are the next two rows) |
| `xenia/gpu/command_processor.h` | `GetShaderPreparationProgress` |
| `xenia/gpu/vulkan/vulkan_pipeline_cache.h`, `.cc`, `vulkan_command_processor.h`, `.cc` (further) | The pixel interpreter (`vulkan_placeholder_ps_mode = 3`), its textures, samplers and float constants; `vulkan_placeholder_depth_only_async`; the vertex texture switch and the vertex budget; the `first run split:` line; the creation threads' adaptive policy; the driver cache check, the progress counters, import and export of the shader record |
| `.github/workflows/boosted.yml` | The job "Translator tests" (`claude/translator-wide` at `0665311`, merged 2026-11-14 into `claude/integration-1115`, as are the rows down to `entry_table.cc`; the full table is in `boosted/notes/translator-wide.md`) |
| `xenia/cpu/ppc/testing/ppc_testing_main.cc`, `testing/ci/**`, `testing/seq_*.s` | The test runner's state dump, self-check counters and `seq_*.s` files; new: the small ARM64 build, its scripts and matrix, seven test files |
| `xenia/cpu/ppc/ppc_emit_control.cc` | `ppc_mcrf_copies_field` |
| `xenia/cpu/hir/opcodes.h`, `hir_builder.h`, `.cc`, `cpu/ppc/ppc_hir_builder.cc` | `CONTEXT_BARRIER_FOR_BRANCH`; `ContextBarrier` takes flags; the per-instruction sync masks; the barriers of branch instructions marked |
| `xenia/cpu/compiler/passes/guest_register_pinning_pass.h`, `.cc` | `RunWide` and `pin_guest_registers_wide`, `_flags`, `_max_int`, `_max_vec` |
| `xenia/cpu/compiler/passes/dead_context_store_pass.*`, `context_constant_pass.*` | New files: `eliminate_dead_context_stores`, `propagate_context_constants` |
| `xenia/cpu/compiler/translation_stats.h`, `compiler.cc`, `compiler_passes.h`, `cpu/ppc/ppc_translator.cc` | The new counters and `SelfCheckMismatches`; the new passes in the pipeline; the translate-ahead queueing |
| `xenia/cpu/backend/a64/a64_emitter.h`, `.cc`, `a64_sequences.cc`, `a64_seq_control.cc`, `a64_seq_util.h` | Masked write-back and reload, 8-bit pins, block counters; fused compare and branch; short branches and the two-attempt loop in `Emit`; linked call literals; the stack fix-up left out or counted; the verify modes |
| `xenia/cpu/backend/a64/a64_code_cache.h`, `.cc`, `a64_function.h` | `LinkCallSites` and the literal list a function carries to it |
| `xenia/cpu/processor.h`, `.cc`, `cpu/entry_table.cc` | The translate-ahead helper, its stop in `~Processor`, the priority boost from the entry table's wait |
| `xenia/config.cc` | `SaveConfig` and `SaveGameConfig` write a `.tmp`, flush (and `fsync` in `SaveConfig`) and rename over the file (asked for by `claude/defaults-config`; applied on `claude/integration-1115`, as are the next two rows) |
| `xenia/gpu/command_processor.cc` | `NoteWaitEvent()` after the read pointer write-back, so a parked busy wait ends with the GPU's progress (asked for by `claude/guest-waits`) |
| `xenia/cpu/backend/a64/a64_emitter.cc` | `a64_guest_memory_order` (default 0): `dmb ishst` before every guest store sequence and `dmb ishld` after every guest load sequence, in `Emit`'s instruction loop (level 1 only since `claude/tso-order`) |
| `xenia/cpu/backend/a64/a64_emitter.cc`, `.h`, `a64_seq_util.h`, `a64_seq_memory.cc`, `a64_seq_vector.cc`, `cpu/compiler/translation_stats.h`, `compiler.cc` | `a64_guest_memory_order = 2` (`claude/tso-order`, default stays 0): guest loads as `LDAPR`, stores as `STLR` through `GuestLdr`/`GuestStr`; stack-relative and read-only constant accesses plain; 128-bit accesses, `lvlx`/`stvlx` and friends and dcbz with `dmb ishld` after or `dmb ishst` before; level 1 where the host lacks FEAT_LRCPC (`GuestMemoryOrderLevel`); four counters in the TranslationStats line. `boosted/notes/tso-order.md` |
| `xenia/cpu/backend/backend.h`, `a64/a64_backend.h`, `.cc`, `kernel/guest_scheduler.cc` | `Backend::ClearReservation`, called by `GuestScheduler::SwitchTo` for the thread it dispatches: a `lwarx` reservation does not survive a context switch, as on the console (not behind a setting) |
| `xenia/cpu/aot_translate_measure.cc`, `.h` (new), `cpu/processor.h`, `.cc`, `cpu/cpu_flags.h`, `.cc` | `aot_translate_measure` (default off): after the executable loads, one nice-19 thread translates every .pdata and load-declared function start through `ResolveFunction` and logs progress and a timing and memory summary ("AOT measure:"); stopped on executable unload and in `~Processor` |
| `xenia/cpu/xex_module.cc` | Collects the function starts for `aot_translate_measure` at the end of `Precompile`; stops it at the top of `Unload` |
| `xenia/cpu/backend/code_cache.h`, `code_cache_base.h` | `used_size()`: bytes of the code region handed out so far |
| `xenia/kernel/stall_watchdog.cc`, `.h` (new), `kernel/kernel_state.cc` | `stall_watchdog` (default on, observes only) and `stall_watchdog_seconds` (10): a 1 s host thread writes one `STALL REPORT` when frames present but no guest thread other than the swap thread has made a kernel call for that long; started in `KernelState`'s constructor, stopped first in its destructor |
| `xenia/kernel/util/shim_utils.h`, `kernel/xthread.h` | `X::Trampoline` records every kernel call in the calling thread's 16-entry ring; `XThread::kernel_call_trace_` holds it |
| `xenia/base/mutex.cc` | On Android `gettid()` is bionic's, which does not go through the TLS resolver (asked for by `claude/guest-waits`) |

## Taking a newer hakuX

```
git remote add hakux https://github.com/rfandango/hakuX.git   # once
git fetch hakux master
git merge hakux/master
```

Conflicts can only come from the hakuX files above (fourteen with the
settings' tabs and the look). If hakuX's native
dependencies changed (`android/README.md`'s table), update
`boosted/tools/fetch-native-deps.sh` to match.

## Taking a newer XenDroid

```
git subtree pull --prefix=x360 https://github.com/rfandango/XenDroid main --squash
python3 boosted/tools/sync-x360-gitmodules.py     # if x360/.gitmodules changed
git submodule update --init --recursive --depth 1 -- x360
```

Then compare XenDroid's own Gradle files with the two modules in
`boosted/android/`: a dependency, an asset directory or a native build flag
that upstream added to `x360/app/build.gradle` or
`x360/emulator-core/build.gradle` has to be added there too, and an activity
or permission added to `x360/app/src/main/AndroidManifest.xml` has to be
added to `boosted/android/x360-ui/src/main/AndroidManifest.xml`.

If upstream adopts hakuX's adrenotools patch itself, the subtree pull will
conflict in `third_party/libadrenotools`: take upstream's.

## One settings screen, one look

hakuX's settings screen has three tabs (`boosted/SettingsTabs.kt`):

- **Global**: what both consoles share or both have one of. Games folders
  and "All files access", each console's controls, the app's theme, each
  console's GPU driver, updates, logs.
- **Xbox**: hakuX's emulation settings.
- **Xbox 360**: every setting of the 360 core's schema, drawn in hakuX's
  rows by `x360/app/.../ui/settings/HostSettingsPane.kt` through
  `HostApp.Settings`. The rows read and write through XenDroid's own
  `SettingsViewModel` and `SettingsRepository`: the same config file, the
  same flush on pause. XenDroid's `SettingsScreen.kt`, `SettingRows.kt` and
  `SettingsSchema.kt` are as upstream has them; the per-game editor and
  the other screens of the 360 side (patches, content, profiles, key map,
  touch controls, about) are still XenDroid's files, shown one at a time
  (`HostApp.show`).

The look is defined once, in `x360/app/.../ui/theme/HostTheme.kt`
(`SharedLook`: colours, shapes, type, the focus ring). It lives on the 360
side because the 360's screens and its in-game menu run without hakuX's
classes (the game is in its own process); hakuX's `HakuXTheme` calls it
through `boosted/Look.kt`. Change the look there and both sides change.

## What is still two

The library is one and the settings screen is one (above). Each core keeps
its own settings store, per-game settings format, data directory, controls
and driver choice: the two cores share no code, and a setting of one means
nothing to the other. The settings' Global tab therefore shows two rows
where each console has its own of a thing (controls, GPU driver), under the
console's name.

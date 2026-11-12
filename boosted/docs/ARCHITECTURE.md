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
    .../boosted/X360.kt       the one bridge to the 360 side (this fork)
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
`xendroid.compose.core.HostApp`:

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
| `android/app/.../ui/settings/SettingsScreen.kt` | One call to `X360Settings()`: the Xbox 360 section |

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
| `app/.../settings/TouchOverlayDefault.kt` | hakuX's test of a real controller, so a phone's built-in pseudo-gamepad doesn't hide the touch controls |
| `emulator-core/.../cpp/emulator_xendroid.cpp`, `xendroid_emu.cpp`, `emulator.h`, `java/xendroid/compose/Emulator.java` | Three JNI calls for the in-game menu: `live_setting`, `set_live_setting` (the cvars the core rereads every frame, a fixed list) and `current_title_id` |
| `app/.../core/EmulatorSession.kt` | Their Kotlin wrappers |
| `app/.../ui/pause/PauseMenuPanel.kt`, `EmulatorHostActivity.kt` | The in-game menu gains "Edit controls" and "Settings": pages of the menu (`MenuPage`), each with its own `PanelNav` |
| `app/.../gamepad/GamepadEditorScreen.kt` | An `inGame` mode: leaves the game window's orientation and system bars alone, shows the game behind |
| `app/.../core/Embedded.kt`, `core/TitleDefaults.kt`, `core/BundledDriver.kt`, `core/HostApp.kt`, `ui/pause/InGameSettings.kt`, `ui/theme/HostTheme.kt` | New files |
| `app/.../ui/theme/Theme.kt` | `xendroidTheme` hands over to `HostTheme` when embedded: hakuX's palette (a copy of hakuX's `ui/theme/Theme.kt`, to be kept in step by hand) and hakuX's light or dark setting |

## Taking a newer hakuX

```
git remote add hakux https://github.com/rfandango/hakuX.git   # once
git fetch hakux master
git merge hakux/master
```

Conflicts can only come from the nine hakuX files above. If hakuX's native
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

## What is still two

The library is one (above). Each core keeps its own settings store, per-game
settings format, data directory and driver choice: the two cores share no
code, and a setting of one means nothing to the other. hakuX's settings
screen has an Xbox 360 section (`X360Settings`) whose rows open the 360
side's screens; those are XenDroid's own files, shown one at a time
(`HostApp.show`) in hakuX's colours (`ui/theme/HostTheme.kt`).

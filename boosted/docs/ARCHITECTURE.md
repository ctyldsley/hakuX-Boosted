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

The app reaches the 360 side in two places, both through
`android/app/src/main/java/com/rfandango/haku_x/boosted/X360.kt`:

- The library's top row has an "Xbox 360" button that opens the 360 side's
  own library (`xendroid.compose.MainActivity`).
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
| `android/app/.../ui/library/LibraryScreen.kt` | One call to `X360LibraryButton()` |

XenDroid (`x360/`): search for `hakuX-Boosted:`. At the time of writing:

| File | Change |
|---|---|
| `emulator-core/.../third_party/libadrenotools/*` | hakuX's KGSL context priority patch |
| `emulator-core/.../xenia/ui/vulkan/vulkan_instance.cc` | The `kgsl_context_priority` cvar |
| `emulator-core/src/main/cpp/CMakeLists.txt` | Hook libraries as dependencies of `emu` |
| `emulator-core/.../Application.java`, `DocumentsProvider.java` | `ensure_init` / `attach`, so the class need not be the process's Application |
| `app/.../MainActivity.kt`, `EmulatorHostActivity.kt` | Call `ensure_init`; seed the per-title defaults |
| `app/.../updater/updater.kt`, `ui/library/GameLibraryScreen.kt` | XenDroid's updater off when embedded |
| `app/.../core/Embedded.kt`, `core/TitleDefaults.kt` | New files |

## Taking a newer hakuX

```
git remote add hakux https://github.com/rfandango/hakuX.git   # once
git fetch hakux master
git merge hakux/master
```

Conflicts can only come from the seven hakuX files above. If hakuX's native
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

## What a deeper merge would look like

The two frontends are separate on purpose in this version. Folding the 360
library into hakuX's library screen (one list, one settings tree, one driver
manager) is frontend work with no effect on emulation, and every line of it
is a line that conflicts with upstream hakuX's UI. It is in the roadmap as a
later step, after the 360 core's performance work, and should be done by
adding a second data source to hakuX's library rather than by moving
XenDroid's screens.

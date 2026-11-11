# hakuX-Boosted

hakuX-Boosted is hakuX (original Xbox, `native/`, `hw/`) with an Xbox 360
side next to it: XenDroid, a Xenia fork with an ARM64 JIT, under `x360/`. One
Android app holds both cores. Each core keeps its own process, settings and
data; the app's library opens the 360 library, and a game image that turns
out to be an Xbox 360 one is sent to the 360 core.

Everything this fork adds on top of the two upstreams is either in this
directory or listed in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), so that
a newer hakuX and a newer XenDroid can each be merged in without touching
the other.

## Where things are

| Path | What |
|---|---|
| `native/`, `hw/`, `android/`, `mods/`, `tools/`, `tests/` | hakuX, as upstream (`rfandango/hakuX`) |
| `x360/` | XenDroid (`rfandango/XenDroid`) as a git subtree, plus this fork's changes to it |
| `boosted/android/` | The two Gradle modules that build the 360 side inside hakuX's app |
| `boosted/x360-titles/` | Per-title defaults for 360 games (configs, patches) |
| `boosted/tools/` | Build scripts: hakuX's core on any host, its dependencies, the submodule list |
| `boosted/docs/` | Architecture, the hakuX to 360 crossover study, the roadmap, Fable II notes |
| `.github/workflows/boosted.yml` | The build of the whole app on GitHub Actions |

## Status

Read [docs/STATUS.md](docs/STATUS.md) first: it says what has been built,
what has been run on a device (nothing yet), and what each change is expected
to do.

## Building

GitHub Actions builds the debug APK on every push to a `claude/**` or
`boosted/**` branch (`.github/workflows/boosted.yml`); the APK is the run's
artifact. The same steps by hand, on Linux, macOS or Windows (Git Bash):

```
# sources
git submodule update --init --depth 1 external/fex
git -C external/fex submodule update --init --depth 1 External/fmt External/range-v3 \
  External/rpmalloc External/unordered_dense External/xxhash Source/Common/cpp-optparse
git submodule update --init --recursive --depth 1 -- x360

# hakuX's core into the app's jniLibs (NDK 29.0.14206865)
ANDROID_NDK=<ndk> boosted/tools/build-xbox-core.sh

# the app; Gradle builds the 360 core (CMake 3.30.3, glslang-tools, spirv-tools, python3)
cd android && ./gradlew assembleDebug
```

Toolchain: JDK 21, Android SDK 36 with Build Tools 36.1.0, NDK 29.0.14206865,
CMake 3.30.3 (from the SDK manager), Ninja, Python 3, `glslangValidator`,
`spirv-opt` and `spirv-dis` on the PATH. `android/README.md` and
`x360/BUILD.md` are the two upstreams' own build notes.

## Licences

hakuX is GPL-3.0-or-later (`COPYING`, `LICENSE`). Xenia, the base of the 360
core, is BSD 3-Clause (`x360/emulator-core/src/main/cpp/xenia/LICENSE`),
which combines with the GPL. XenDroid's own Android files carry either a
WTFPL header or none, and XenDroid has no top-level licence file: see
[docs/STATUS.md](docs/STATUS.md) before distributing a build.

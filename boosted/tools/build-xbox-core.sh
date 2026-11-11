#!/usr/bin/env bash
# Builds hakuX's own native core (libhakuxcore.so, libSDL3.so, adrenotools'
# hook libraries) with the NDK and puts the libraries, stripped, into the
# app's jniLibs, where Gradle packs them from. The steps of android/README.md
# on any host: the dependencies fetched (fetch-native-deps.sh), the core
# configured and built, the libraries copied (what tools/native/app-libs.sh
# does on the upstream author's Windows machine).
#
#   ANDROID_NDK=<ndk> boosted/tools/build-xbox-core.sh [build dir]
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-$here/build/native/core}
ndk=${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}
if [ -z "$ndk" ] || [ ! -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
  echo "ANDROID_NDK (or ANDROID_NDK_HOME) must name an NDK (29.0.14206865)" >&2
  exit 1
fi
deps=${NC_DEPS:-$here/build/native-deps}

"$here/boosted/tools/fetch-native-deps.sh" "$deps"

launcher=()
if [ "${USE_CCACHE:-}" = "true" ]; then
  launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
cmake -S "$here/native" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DNC_DEPS="$deps" "${launcher[@]}"
ninja -C "$build" hakuxcore

strip=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-strip* | head -1)
out="$here/android/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$out"
"$strip" -o "$out/libhakuxcore.so" "$build/libhakuxcore.so"
"$strip" -o "$out/libSDL3.so" "$build/sdl3/libSDL3.so"
for h in main_hook file_redirect_hook gsl_alloc_hook kgsl_context_hook hook_impl; do
  if [ -f "$build/adrenotools/src/hook/lib$h.so" ]; then
    "$strip" -o "$out/lib$h.so" "$build/adrenotools/src/hook/lib$h.so"
  fi
done
ls -la "$out"

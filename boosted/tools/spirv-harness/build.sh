#!/usr/bin/env bash
# Builds the SPIR-V harness (README.md here) on a Linux desktop with clang,
# cmake, ninja and python3. Usage: build.sh BUILD_DIR
# Needs these submodules of the 360 side checked out (from the repository's
# root):
#   T=x360/emulator-core/src/main/cpp/xenia/third_party
#   git submodule update --init --depth 1 -- $T/SPIRV-Tools $T/SPIRV-Headers \
#     $T/glslang $T/fmt $T/Vulkan-Headers $T/tomlplusplus $T/cxxopts \
#     $T/utfcpp $T/xxhash $T/VulkanMemoryAllocator $T/date
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
xenia=$root/x360/emulator-core/src/main/cpp/xenia
build=${1:?usage: build.sh BUILD_DIR}
mkdir -p "$build/obj" "$build/spirv-tools"
build=$(cd "$build" && pwd)
jobs=$(nproc)
cxx=${CXX:-clang++}

# SPIRV-Tools: the optimiser library the app links, and the command-line
# tools to validate and disassemble what the harness writes.
if [ ! -f "$build/spirv-tools/build.ninja" ]; then
  cmake -G Ninja -S "$xenia/third_party/SPIRV-Tools" -B "$build/spirv-tools" \
    -DSPIRV-Headers_SOURCE_DIR="$xenia/third_party/SPIRV-Headers" \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$cxx" > "$build/spirv-tools/configure.log"
fi
ninja -C "$build/spirv-tools" SPIRV-Tools-opt SPIRV-Tools-static \
  spirv-opt spirv-val spirv-dis > "$build/spirv-tools/build.log"

printf '#define XE_BUILD_BRANCH "harness"\n#define XE_BUILD_COMMIT "0"\n#define XE_BUILD_COMMIT_SHORT "0"\n#define XE_BUILD_DATE __DATE__\n' > "$build/version.h"

# The flags xenia's own CMake gives its sources, -Werror included, so that a
# warning the Android build would stop on stops this one too.
flags="-std=c++20 -O2 -I$xenia -I$xenia/src -I$build
  -isystem $xenia/third_party -isystem $xenia/third_party/glslang
  -isystem $xenia/third_party/Vulkan-Headers/include
  -isystem $xenia/third_party/SPIRV-Tools/include
  -DVULKAN_HPP_NO_TO_STRING -D_LIB -DUSE_CPP17 -DXE_GPU_SPIRV_TOOLS_OPT=1
  -Wno-switch -Wno-attributes -Wno-unknown-warning-option
  -Wno-deprecated-register -Wno-deprecated-volatile
  -Wno-deprecated-enum-enum-conversion -Wno-absolute-value -Werror"
case $(uname -m) in x86_64) flags="$flags -mavx" ;; esac

sources="
  $xenia/src/xenia/gpu/spirv_shader_translator.cc
  $xenia/src/xenia/gpu/spirv_shader_translator_alu.cc
  $xenia/src/xenia/gpu/spirv_shader_translator_fetch.cc
  $xenia/src/xenia/gpu/spirv_shader_translator_memexport.cc
  $xenia/src/xenia/gpu/spirv_shader_translator_rb.cc
  $xenia/src/xenia/gpu/spirv_shader_translator_optimize.cc
  $xenia/src/xenia/gpu/spirv_builder.cc
  $xenia/src/xenia/gpu/spirv_shader.cc
  $xenia/src/xenia/gpu/shader.cc
  $xenia/src/xenia/gpu/shader_translator.cc
  $xenia/src/xenia/gpu/shader_translator_disasm.cc
  $xenia/src/xenia/gpu/ucode.cc
  $xenia/src/xenia/gpu/xenos.cc
  $xenia/src/xenia/gpu/gpu_flags.cc
  $xenia/src/xenia/base/string_buffer.cc
  $xenia/src/xenia/base/cvar.cc
  $xenia/src/xenia/base/filesystem.cc
  $xenia/src/xenia/base/filesystem_posix.cc
  $xenia/src/xenia/base/string.cc
  $xenia/src/xenia/base/utf8.cc
  $xenia/src/xenia/base/memory.cc
  $here/stubs.cc
  $here/harness.cc"
third_party="
  $xenia/third_party/glslang/SPIRV/InReadableOrder.cpp
  $xenia/third_party/glslang/SPIRV/Logger.cpp
  $xenia/third_party/glslang/SPIRV/SpvBuilder.cpp
  $xenia/third_party/glslang/SPIRV/SpvPostProcess.cpp
  $xenia/third_party/glslang/SPIRV/SpvTools.cpp
  $xenia/third_party/glslang/SPIRV/disassemble.cpp
  $xenia/third_party/glslang/SPIRV/doc.cpp
  $xenia/third_party/fmt/src/format.cc"

compile() {  # compile SOURCE FLAGS...
  local source=$1; shift
  local object="$build/obj/$(echo "$source" | sed 's|.*/xenia/||; s|[/.]|_|g').o"
  if [ ! -f "$object" ] || [ "$source" -nt "$object" ] || [ -n "${REBUILD:-}" ]; then
    "$cxx" "$@" -c "$source" -o "$object"
  fi
}
export -f compile; export build cxx
# A changed header does not rebuild anything: run with REBUILD=1 after one.
echo $sources | tr ' ' '\n' | xargs -P "$jobs" -I{} bash -c "compile {} $(echo $flags)"
echo $third_party | tr ' ' '\n' | xargs -P "$jobs" -I{} bash -c \
  "compile {} -std=c++20 -O2 -w -I$xenia/third_party/glslang -I$xenia/third_party/fmt/include"

"$cxx" "$build"/obj/*.o "$build/spirv-tools/source/opt/libSPIRV-Tools-opt.a" \
  "$build/spirv-tools/source/libSPIRV-Tools.a" -lpthread -o "$build/spirv-harness"
echo "built $build/spirv-harness; spirv-val and spirv-dis are in $build/spirv-tools/tools"

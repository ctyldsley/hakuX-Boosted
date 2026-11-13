#!/usr/bin/env bash
# Builds the first-run rig (README.md here) on a Linux desktop or cloud
# machine. Usage: build.sh BUILD_DIR
# Needs clang, cmake, ninja, python3, glslang-tools, spirv-tools, libvulkan-dev
# and a Vulkan device (mesa-vulkan-drivers gives lavapipe, which needs no GPU),
# and the submodules boosted/tools/spirv-harness/build.sh names: it is run
# first, for the translator's objects.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../../../../../../../.." && pwd)
xenia=$root/x360/emulator-core/src/main/cpp/xenia
build=${1:?usage: build.sh BUILD_DIR}
mkdir -p "$build/harness" "$build/rig" "$build/spv"
build=$(cd "$build" && pwd)
cxx=${CXX:-clang++}

"$root/boosted/tools/spirv-harness/build.sh" "$build/harness"

# The interpreter's shaders, compiled as tools/build/compile_shader_spirv.py
# compiles them for the app (glslangValidator, then spirv-opt -O -O), and the
# rig's own.
shaders=$xenia/src/xenia/gpu/shaders
for source in "$shaders"/ucode_program*.glsl "$here"/rig_*.glsl; do
  name=$(basename "$source" .glsl)
  case ${name##*.} in
    vs) stage=vert ;;
    ds) stage=tese ;;
    gs) stage=geom ;;
    ps) stage=frag ;;
    *) continue ;;
  esac
  glslangValidator -S $stage -DSHADING_LANGUAGE_GLSL_XE=1 -V \
    -o "$build/spv/$name.unoptimised.spv" "$source" > "$build/spv/$name.log" \
    || { cat "$build/spv/$name.log"; exit 1; }
  spirv-opt -O -O "$build/spv/$name.unoptimised.spv" -o "$build/spv/$name.spv"
  spirv-val "$build/spv/$name.spv"
done

flags="-std=c++20 -O2 -I$xenia -I$xenia/src -I$build/harness
  -isystem $xenia/third_party -isystem $xenia/third_party/glslang
  -isystem $xenia/third_party/Vulkan-Headers/include
  -DVULKAN_HPP_NO_TO_STRING -D_LIB -DUSE_CPP17
  -Wno-switch -Wno-attributes -Wno-unknown-warning-option
  -Wno-deprecated-register -Wno-deprecated-volatile
  -Wno-deprecated-enum-enum-conversion -Wno-absolute-value -Werror"
case $(uname -m) in x86_64) flags="$flags -mavx" ;; esac

for source in "$here/rig.cc" "$xenia/src/xenia/gpu/ucode_program.cc"; do
  "$cxx" $flags -c "$source" -o "$build/rig/$(basename "$source" .cc).o"
done
objects=$(ls "$build"/harness/obj/*.o | grep -v harness_cc)
"$cxx" "$build"/rig/*.o $objects \
  "$build/harness/spirv-tools/source/opt/libSPIRV-Tools-opt.a" \
  "$build/harness/spirv-tools/source/libSPIRV-Tools.a" \
  -lvulkan -lpthread -o "$build/first-run-rig"
echo "built $build/first-run-rig; run it with --spv $build/spv"

#!/usr/bin/env bash
# hakuX-Boosted bench: builds the primitives benchmark with plain compiler
# commands, for places without CMake. The workflow uses it inside a Termux
# container, where the compiler targets Android and the program runs against
# bionic, Android's own libc and dynamic linker.
#
#   build-plain.sh <output directory> [C compiler] [C++ compiler]
#
# Run from anywhere; the fcontext assembly is taken from the core's
# boost_context submodule, which must be checked out.
set -euo pipefail
out="${1:?output directory}"
cc="${2:-clang}"
cxx="${3:-clang++}"
here="$(cd "$(dirname "$0")" && pwd)"
asm="$here/../../../x360/emulator-core/src/main/cpp/xenia/third_party/boost_context/context/src/asm"
flags=(-O2 -DNDEBUG -march=armv8-a -fno-strict-aliasing -fno-omit-frame-pointer)
# PRIM_EXTRA_FLAGS: more flags for every compile and link, space separated
# (the workflow passes -fno-emulated-tls inside the Termux container).
mkdir -p "$out"
# libc++ where the compiler has it, as in the app and in the CMake build.
if echo 'int main(){}' | "$cxx" -x c++ -stdlib=libc++ - -o "$out/.probe" 2> /dev/null; then
  flags+=(-stdlib=libc++ '-DBENCH_CXX_LIBRARY="libc++"')
fi
rm -f "$out/.probe"
read -r -a extra <<< "${PRIM_EXTRA_FLAGS:-}"
flags+=("${extra[@]}")
mkdir -p "$out"

"$cc" "${flags[@]}" -fPIC -shared "$here/tls_lib.c" \
  -DPRIM_PREFIX=prim_tls_dyn -DPRIM_TLS_PAD=65536 -o "$out/libprim_tls_dyn.so"
"$cc" "${flags[@]}" -fPIC -shared "$here/tls_lib.c" \
  -DPRIM_PREFIX=prim_tls_static -o "$out/libprim_tls_static.so"
"$cxx" "${flags[@]}" -std=c++20 -moutline-atomics -DPRIM_PREFIX=prim_outline \
  -c "$here/atomics.cc" -o "$out/atomics_outline.o"
"$cxx" "${flags[@]}" -std=c++20 -march=armv8.1-a -mno-outline-atomics \
  -DPRIM_PREFIX=prim_lse -c "$here/atomics.cc" -o "$out/atomics_lse.o"
"$cxx" "${flags[@]}" -std=c++20 -mno-outline-atomics -DPRIM_PREFIX=prim_llsc \
  -c "$here/atomics.cc" -o "$out/atomics_llsc.o"
"$cxx" "${flags[@]}" -std=c++20 -I"$here/../common" "$here/prim_main.cc" \
  "$out/atomics_outline.o" "$out/atomics_lse.o" "$out/atomics_llsc.o" \
  "$asm/make_arm64_aapcs_elf_gas.S" "$asm/jump_arm64_aapcs_elf_gas.S" \
  "$asm/ontop_arm64_aapcs_elf_gas.S" \
  -L"$out" -lprim_tls_static -Wl,-rpath,'$ORIGIN' -o "$out/bench_prim"
echo "built $out/bench_prim"

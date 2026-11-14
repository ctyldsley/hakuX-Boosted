#!/usr/bin/env bash
# hakuX-Boosted bench: one build of the rig against one tree of sources, for
# the workflow.
#
#   ci-build.sh <harness dir (boosted/bench)> <tree> <build dir> <output dir>
#
# The build directory is thrown away afterwards and the programs are copied
# to the output directory. Both builds of a job use the same build directory
# and the same tree path (a link the workflow repoints), so that the compiler
# cache sees identical commands and file names for every source the two
# branches have in common and compiles only what differs.
#
# The primitives and the core benchmarks are built as two steps: if a branch
# changes the core so that the harness no longer builds against it, the
# primitives still report, and the script says which part failed (exit
# status 0 with a marker file, so the job can go on and report it).
set -u
harness="${1:?harness}"
tree="${2:?tree}"
build="${3:?build dir}"
out="${4:?output dir}"

rm -rf "$build" "$out"
mkdir -p "$build" "$out/prim" "$out/core"
launcher=()
if command -v ccache >/dev/null 2>&1; then
  launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
if ! cmake -S "$harness" -B "$build" -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_ASM_COMPILER=clang -DBENCH_TREE="$tree" "${launcher[@]}" \
    > "$out/configure.log" 2>&1; then
  tail -40 "$out/configure.log"
  echo "configure" > "$out/FAILED"
  exit 0
fi

if ninja -C "$build" bench_prim prim_tls_dyn > "$out/build-prim.log" 2>&1; then
  cp "$build/prim/bench_prim" "$build"/prim/libprim_tls_*.so "$out/prim/"
else
  tail -60 "$out/build-prim.log"
  echo "primitives" >> "$out/FAILED"
fi

if ninja -C "$build" bench_core bench_core_static > "$out/build-core.log" 2>&1; then
  cp "$build/core/bench_core" "$build/core/bench_core_static" \
     "$build/core/libxecore.so" "$out/core/"
else
  grep -E "error|FAILED|undefined" -A6 "$out/build-core.log" | head -120
  echo "core" >> "$out/FAILED"
fi
rm -rf "$build"
exit 0

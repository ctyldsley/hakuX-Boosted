#!/usr/bin/env bash
# hakuX-Boosted bench: runs every benchmark program found in a build
# directory and appends their result lines to one file.
#
#   boosted/bench/run.sh <build dir> <results file> [rounds]
#
# A program that is missing (its build failed on this branch) is recorded as
# a note in the results and skipped, so the others still report.
set -u
build="${1:?build directory}"
out="${2:?results file}"
rounds="${3:-1}"
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$(dirname "$out")"
export BENCH_OUT="$out"

note() {
  printf '{"section":"%s","name":"%s","note":"%s"}\n' "$1" "$2" "$3" >> "$out"
  echo "  $1 $2: $3" >&2
}

status=0
for round in $(seq 1 "$rounds"); do
  echo "== round $round of $rounds" >&2
  if [ -x "$build/prim/bench_prim" ]; then
    (cd "$build/prim" && LD_LIBRARY_PATH=. ./bench_prim ./libprim_tls_dyn.so) \
      || { note prim run "bench_prim exited with an error"; status=1; }
  else
    note prim build "bench_prim was not built"
    status=1
  fi
  if [ -x "$build/core/bench_core" ]; then
    "$here/core/run-core.sh" "$build/core" || status=1
  else
    note sched build "the core benchmarks were not built on this tree (see the build log)"
    note xlat build "the core benchmarks were not built on this tree (see the build log)"
    status=1
  fi
done
exit $status

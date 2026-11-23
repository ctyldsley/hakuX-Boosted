#!/usr/bin/env bash
# hakuX-Boosted jit bench: host instructions that translated code runs for
# each workload's checked (short) call, counted under qemu-user. For a
# machine whose kernel gives no instruction counter (a cloud session's
# container); the workflow's runner has one and counts the timed calls.
#
#   jit-count-qemu.sh <directory holding bench_core> <workloads.manifest> [name=value ...]
#
# qemu logs every instruction executed inside the code cache's address
# range. The guest work of a call is the same whatever the settings, so the
# ratio of two counts is the ratio of host instructions per guest
# instruction. Code the translator calls out to (host functions) is outside
# the range. Environment: QEMU (default "qemu-aarch64 -L
# /usr/aarch64-linux-gnu"), WORKLOADS (default: all in the manifest).
set -uo pipefail
dir=${1:?directory holding bench_core}
manifest=${2:?manifest}
shift 2
qemu=${QEMU:-qemu-aarch64 -L /usr/aarch64-linux-gnu}
args=()
for setting in "$@"; do args+=(--set "$setting"); done
workloads=${WORKLOADS:-$(awk '$1 == "run" && $NF == "check" { print $2 }' "$manifest")}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
total=0
status=0
one() {
  local w=$1 out range
  out=$(BENCH_JIT_ONLY=$w BENCH_JIT_MANIFEST=$manifest $qemu "$dir/bench_core" --suite=jit --label=count "${args[@]}" 2> /dev/null | grep '^jit-count' || true)
  range=$(echo "$out" | sed -n 's/^jit-count code_cache=\(0x[0-9a-f]*\)\.\.\(0x[0-9a-f]*\)$/\1..\2/p')
  if [ -z "$range" ] || ! echo "$out" | grep -q ' ok=1$'; then
    echo "NOT MEASURED"
    return 1
  fi
  mkfifo "$work/$w.log"
  grep -c '^Trace' "$work/$w.log" > "$work/$w.count" &
  BENCH_JIT_ONLY=$w BENCH_JIT_MANIFEST=$manifest $qemu -one-insn-per-tb -d exec,nochain -dfilter "$range" \
    -D "$work/$w.log" "$dir/bench_core" --suite=jit --label=count "${args[@]}" > "$work/$w.out" 2> /dev/null
  wait
  grep -q ' ok=1$' "$work/$w.out" || { echo "NOT MEASURED"; return 1; }
  cat "$work/$w.count"
}
export -f one
export dir manifest qemu work
declare -a pids
for w in $workloads; do
  ( BENCH_ARGS=("${args[@]}"); one "$w" > "$work/$w.result" ) &
  while [ "$(jobs -rp | wc -l)" -ge "${JOBS:-$(nproc)}" ]; do sleep 0.2; done
done
wait
for w in $workloads; do
  n=$(cat "$work/$w.result")
  printf '%-10s %12s\n' "$w" "$n"
  case $n in *[!0-9]*|'') status=1 ;; *) total=$((total + n)) ;; esac
done
printf '%-10s %12s\n' total "$total"
exit $status

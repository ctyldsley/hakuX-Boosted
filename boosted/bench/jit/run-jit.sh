#!/usr/bin/env bash
# hakuX-Boosted jit bench: runs core/bench_jit.cc over every image in a
# directory, for one build of the rig.
#
#   run-jit.sh <directory holding bench_core and libxecore.so> <images dir> <results file> [name=value ...]
#
# Each image is one process (the image is loaded at its own address). The
# settings after the results file are the core's own, as for run-core.sh.
# ROUNDS (3): how often each image is run; the report takes the lowest of
# each number. PAGE_16K=1 adds a run under qemu-user with 16 KiB pages, the
# phone's page size, at which the translator emits other code for guest
# memory accesses: only the code size and the checked calls of that run mean
# anything, so its timed calls are left out.
set -u
dir="${1:?directory}"
images="${2:?images}"
out="${3:?results file}"
shift 3
args=(--set break_on_unimplemented_instructions=false)
for setting in "$@"; do
  args+=(--set "$setting")
done
mkdir -p "$(dirname "$out")"
export BENCH_OUT="$out"
status=0
shopt -s nullglob
manifests=("$images"/*.manifest)
if [ ${#manifests[@]} -eq 0 ]; then
  echo "run-jit: no image in $images" >&2
  exit 1
fi
note() {
  printf '{"section":"jit","name":"%s","note":"%s"}\n' "$1" "$2" >> "$out"
  echo "  $1: $2" >&2
}
for round in $(seq 1 "${ROUNDS:-3}"); do
  for manifest in "${manifests[@]}"; do
    name=$(basename "$manifest" .manifest)
    # Status 75: the translator faulted on a function, whose address is now
    # in <manifest>.skip; the next try leaves it out (bench_jit.cc).
    for try in $(seq 1 40); do
      BENCH_JIT_MANIFEST="$manifest" "$dir/bench_core" --suite=jit --label=4k "${args[@]}" > /dev/null 2>&1
      code=$?
      [ $code -eq 75 ] || break
    done
    if [ $code -ne 0 ]; then
      note "$name (4k)" "NOT MEASURED or WRONG: bench_core stopped with status $code"
      status=1
    fi
  done
done
if [ -n "${PAGE_16K:-}" ]; then
  if command -v qemu-aarch64 > /dev/null; then
    for manifest in "${manifests[@]}"; do
      name=$(basename "$manifest" .manifest)
      for try in $(seq 1 40); do
        BENCH_JIT_NO_TIMED=1 BENCH_JIT_MANIFEST="$manifest" \
          qemu-aarch64 -p 16384 "$dir/bench_core" --suite=jit --label=16k "${args[@]}" > /dev/null 2>&1
        code=$?
        [ $code -eq 75 ] || break
      done
      if [ $code -ne 0 ]; then
        note "$name (16k)" "NOT MEASURED or WRONG: bench_core under qemu with 16 KiB pages stopped with status $code"
        status=1
      fi
    done
  else
    note "(16k)" "not measured: no qemu-aarch64 on this machine"
  fi
fi
for skipped in "$images"/*.skip; do
  echo "run-jit: functions the translator faulted on, left out: $(basename "$skipped" .manifest.skip) $(tr '\n' ' ' < "$skipped")" >&2
done
exit $status

#!/usr/bin/env bash
# hakuX-Boosted bench: runs the core benchmarks with each set of settings.
# One process per set, because the core reads several settings at start-up.
#
#   run-core.sh <directory holding bench_core and libxecore.so>
#
# The settings are named as in the core's config file. A set that names a
# setting this tree does not have is skipped with a note (exit status 3 of
# bench_core), which is how a branch without a setting still reports the rest.
set -u
dir="${1:?directory}"
status=0

run() {  # run <launcher> <suite> <label> [name=value ...]
  local launcher="$1" suite="$2" label="$3"
  shift 3
  local args=()
  for setting in "$@"; do
    args+=(--set "$setting")
  done
  echo "-- $suite: $label" >&2
  "$dir/$launcher" "--suite=$suite" "--label=$label" "${args[@]}"
  local code=$?
  if [ $code -ne 0 ] && [ $code -ne 3 ]; then
    printf '{"section":"%s","name":"(%s)","note":"NOT MEASURED: the benchmark program stopped with status %s"}\n' \
      "$suite" "$label" "$code" >> "${BENCH_OUT:-/dev/stdout}"
    status=1
  fi
}

# --- 3. The translator ---------------------------------------------------------
run bench_core xlat "defaults"
run bench_core xlat "pin_guest_registers" pin_guest_registers=true
run bench_core xlat "simplification_skip_unchanged_blocks" simplification_skip_unchanged_blocks=true
run bench_core xlat "a64_thunk_fpcr_compare" a64_thunk_fpcr_compare=true
run bench_core xlat "a64_lean_extern_thunk" a64_lean_extern_thunk=true
run bench_core xlat "all four" pin_guest_registers=true \
  simplification_skip_unchanged_blocks=true a64_thunk_fpcr_compare=true \
  a64_lean_extern_thunk=true

exit $status

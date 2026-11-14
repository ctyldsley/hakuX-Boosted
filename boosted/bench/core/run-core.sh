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

# --- 1b. The core's Fiber class ------------------------------------------------
run bench_core fiber "defaults"
run bench_core fiber "host_thread_state_slot" host_thread_state_slot=true
run bench_core_static fiber "static TLS (not Android's)"

# --- 2. The guest scheduler ----------------------------------------------------
lean="guest_scheduler_lean_yield=true"
park="guest_scheduler_park_yield_spinners=true"
slot="host_thread_state_slot=true"
skew="guest_scheduler_skew_bound_us=2000 guest_scheduler_skew_cap_multiple=100"
run bench_core sched "defaults"
run bench_core sched "lean_yield" $lean
run bench_core sched "park_yield_spinners" $park
run bench_core sched "host_thread_state_slot" $slot
run bench_core sched "skew_bound_us=2000" $skew
run bench_core sched "lean_yield + slot" $lean $slot
run bench_core sched "all of the above" $lean $park $slot $skew
run bench_core sched "a64_thunk_fpcr_compare" a64_thunk_fpcr_compare=true
run bench_core sched "a64_lean_extern_thunk" a64_lean_extern_thunk=true
run bench_core_static sched "static TLS (not Android's)"

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

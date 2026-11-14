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
# BENCH_BUSY_WAITS=1 adds the busy_wait_* runs (a thread that busy-waits
# beside unrelated events; boosted/notes/park-rework.md) to a row. They are
# many, so only the rows they are about get them.
BENCH_BUSY_WAITS=1 run bench_core sched "defaults"
run bench_core sched "lean_yield" $lean
run bench_core sched "park_yield_spinners" $park
run bench_core sched "host_thread_state_slot" $slot
run bench_core sched "skew_bound_us=2000" $skew
run bench_core sched "lean_yield + slot" $lean $slot
run bench_core sched "all of the above" $lean $park $slot $skew
BENCH_BUSY_WAITS=1 run bench_core sched "park_waits" guest_scheduler_park_waits=true
# claude/park-rework (boosted/notes/park-rework.md). The busy_wait_* rows are
# what these are for: "defaults" is a busy wait left alone, "first gate" is
# the park as first built (no park if any event happened since the last
# look: what the phone ran on 2026-11-14), the next two are the adaptive
# gate without and with the translator's poll sites. A tree without the
# setting skips its row.
waits="guest_scheduler_park_waits=true"
BENCH_BUSY_WAITS=1 run bench_core sched "park_waits, first gate + poll sites" $waits \
  guest_scheduler_park_wait_adaptive=false park_pause_poll_loops=true
BENCH_BUSY_WAITS=1 run bench_core sched "park_waits + poll sites" $waits park_pause_poll_loops=true
BENCH_BUSY_WAITS=1 run bench_core sched "park_waits + poll sites, max 500 us" $waits \
  park_pause_poll_loops=true guest_scheduler_park_wait_max_us=500
BENCH_BUSY_WAITS=1 run bench_core sched "park_waits + poll sites + skew bound" $waits \
  park_pause_poll_loops=true $skew
run bench_core sched "a64_thunk_fpcr_compare" a64_thunk_fpcr_compare=true
run bench_core sched "a64_lean_extern_thunk" a64_lean_extern_thunk=true
run bench_core_static sched "static TLS (not Android's)"
# BENCH_TIMER_SLACK_NS=1 before any of these lines runs it with the kernel's
# timer slack at 1 ns instead of the default 50 us: a test of whether a path
# is waiting on a short timed sleep. (None of these is: tried on 2026-11-14.)

# --- 3. The translator ---------------------------------------------------------
run bench_core xlat "defaults"
run bench_core xlat "pin_guest_registers" pin_guest_registers=true
run bench_core xlat "simplification_skip_unchanged_blocks" simplification_skip_unchanged_blocks=true
run bench_core xlat "a64_thunk_fpcr_compare" a64_thunk_fpcr_compare=true
run bench_core xlat "a64_lean_extern_thunk" a64_lean_extern_thunk=true
run bench_core xlat "all four" pin_guest_registers=true \
  simplification_skip_unchanged_blocks=true a64_thunk_fpcr_compare=true \
  a64_lean_extern_thunk=true
# claude/translator-wide's settings (boosted/notes/translator-wide.md, section
# 5). The baseline has none of them, so its column is skipped for these rows:
# compare each with this branch's own "defaults" row.
wide="pin_guest_registers=true pin_guest_registers_wide=true"
short="a64_short_backward_branches=true a64_short_forward_branches=true"
run bench_core xlat "short branches" $short
run bench_core xlat "a64_linked_calls" a64_linked_calls=true
run bench_core xlat "dead context stores 2" eliminate_dead_context_stores=2
run bench_core xlat "a64_fuse_compare_branch" a64_fuse_compare_branch=true
run bench_core xlat "pin wide" $wide
run bench_core xlat "pin wide + flags" $wide pin_guest_registers_flags=true
run bench_core xlat "propagate_context_constants" propagate_context_constants=true
run bench_core xlat "translator-wide together" $wide \
  pin_guest_registers_flags=true $short a64_linked_calls=true \
  eliminate_dead_context_stores=2 a64_fuse_compare_branch=true \
  propagate_context_constants=true

exit $status

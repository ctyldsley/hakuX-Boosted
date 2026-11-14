#!/usr/bin/env bash
# hakuX-Boosted: runs the PowerPC instruction tests in parallel shards and
# prints one total.
#
#   run-tests.sh <runner binary> <directory of .bin/.map files> [runner flags...]
#
# Environment:
#   JOBS     shards to run at once (default: the number of cores)
#   MAX_TESTS  instead of one list of suites per job: pack the suites into
#            as few processes as hold at most this many tests each, and run
#            a suite with more tests than that as several processes that
#            each take every n-th of its tests. For runs where a process
#            must stay short (qemu with 16 KiB pages). A process costs about
#            14 s of start-up under qemu (the guest's memory being set up),
#            so fewer, fuller processes are much the faster.
#   WRAPPER  a command to run the binary under, for example
#            "qemu-aarch64 -L /usr/aarch64-linux-gnu" on a host that is not ARM64
#   SUITES   a regular expression over suite names (instr_add, seq_branch_carry):
#            only those run (default: all)
#   LABEL    printed with the total
#   STATE_OUT  a file to write every test's final state to (one line a test,
#              sorted: registers, flags and memory as hashes)
#   STATE_REF  such a file from another run: this run's states must equal it,
#              test for test (the tests named in nondeterministic.txt apart)
#   STATE_KEEP_CODESIZE  compare the size of each test function's machine
#              code too (for two builds of the translator, same settings)
#   STATE_IGNORE  a field of the state lines to leave out of that comparison
#              ("flags": the run changes condition register bits that the
#              ABI leaves undefined at a return; "abiflags" is still compared)
#
# Exit status 0 only if every shard ran to its end and no test failed.
set -uo pipefail
runner=$1
bins=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
tests=$(cd "$here/.." && pwd)
jobs=${JOBS:-$(nproc)}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Suites largest first, dealt round the shards, so that the shards take about
# the same time.
ls -S "$bins"/*.bin | xargs -n1 basename | sed 's/\.bin$//' \
  | grep -E "${SUITES:-.}" > "$work/all"
if [ ! -s "$work/all" ]; then echo "no test suite matches"; exit 1; fi
# Work items: files shard.<name>.s<k>of<n> listing suites; the process for
# one runs slice k of n of each suite's tests (0of1: all of them).
if [ -n "${MAX_TESTS:-}" ]; then
  while read -r suite; do
    echo "$(grep -c ' t test_' "$bins/$suite.map") $suite"
  done < "$work/all" | sort -rn | awk -v max="$MAX_TESTS" -v dir="$work" '
    $1 > max {
      n = int(($1 + max - 1) / max)
      for (k = 0; k < n; k++) print $2 > (dir "/shard.big" big ".s" k "of" n)
      big++; next
    }
    {
      # first fit, largest first
      for (b = 0; b < bins; b++) if (used[b] + $1 <= max) break
      if (b == bins) bins++
      used[b] += $1
      print $2 >> (dir "/shard." b ".s0of1")
    }'
else
  i=0
  while read -r suite; do
    echo "$suite" >> "$work/shard.$((i % jobs)).s0of1"
    i=$((i + 1))
  done < "$work/all"
fi

# Upstream's skip list and this fork's own (known-failures.txt) as one file.
# A line "name unless <flag>" of known-failures.txt applies only to runs
# without that flag, "name if <flag>" only to runs with it.
grep -h -v '^#' "$tests/skip.txt" | grep . > "$work/skip"
grep -v '^#' "$here/known-failures.txt" | grep . | while read -r name when flag; do
  has=0
  if echo " $* " | grep -q -F -- " $flag "; then has=1; fi
  case "$when" in
    '') echo "$name" ;;
    unless) [ "$has" = 0 ] && echo "$name" ;;
    if) [ "$has" = 1 ] && echo "$name" ;;
  esac
done >> "$work/skip"
# eliminate_dead_context_stores=3 takes cr0, cr1 and cr5 to cr7 for
# undefined when a function returns, as the ABI has it. A test that asserts
# the whole condition register after its return ("REGISTER_OUT cr") is then
# a caller reading those fields, and is skipped in such a run. Tests that
# read the condition register themselves (mfcr) are not affected.
if echo " $* " | grep -q -F -- " --eliminate_dead_context_stores=3 "; then
  awk '/^ *test_[A-Za-z0-9_]*:/ { name = $1; sub(/^test_/, "", name); sub(/:.*/, "", name) }
       /#_ REGISTER_OUT cr / { if (name != "") print name }' "$tests"/instr_*.s "$tests"/seq_*.s \
    | sort -u >> "$work/skip"
fi

pids=()
for shard in "$work"/shard.*; do
  slice=${shard##*.s}
  slice=${slice/of//}
  (
    # shellcheck disable=SC2086
    ${WRAPPER:-} "$runner" --test_path="$tests" --test_bin_path="$bins" \
      --test_skip_file="$work/skip" --log_file="$shard.xe.log" \
      --test_state_dump="$shard.state" --test_slice="$slice" "$@" \
      $(cat "$shard") > "$shard.out" 2>&1
    echo $? > "$shard.status"
  ) &
  pids+=($!)
  # At most $jobs at a time.
  while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n || true; done
done
wait "${pids[@]}" 2>/dev/null || true

total=0; passed=0; failed=0; bad=0
for shard in "$work"/shard.*; do
  case $shard in *.out|*.status|*.log|*.state) continue ;; esac
  status=$(cat "$shard.status" 2>/dev/null || echo 255)
  t=$(sed -n 's/^Total tests: //p' "$shard.out" | tail -1)
  p=$(sed -n 's/^Passed: //p' "$shard.out" | tail -1)
  f=$(sed -n 's/^Failed: //p' "$shard.out" | tail -1)
  # A slice of a few small suites can hold no test at all.
  if [ -z "$t" ] && grep -q '^Running .* 0 test cases' "$shard.out" && [ "$status" = 0 ]; then continue; fi
  if [ -z "$t" ]; then
    echo "::error::a shard did not finish (exit status $status); its last lines:"
    tail -25 "$shard.out"
    bad=1
    continue
  fi
  total=$((total + t)); passed=$((passed + p)); failed=$((failed + f))
  if [ "$f" != "0" ] || [ "$status" != "0" ]; then
    grep -E -B4 'FAILED|assert failed|mismatch|PinVerify|Verify:' "$shard.out" | head -120
  fi
done
# Lines the translator's self-check modes print when they find a difference.
# The runner does not show the log, so the count comes from the mismatch
# counters it prints at its end ("Self-check mismatches: N"), which also fail
# the shard; the grep stays for a build whose log is visible.
checks=$(cat "$work"/shard.*.out "$work"/shard.*.xe.log 2>/dev/null \
  | grep -c -E 'PinVerify:|SimplifySkipVerify:|TranslatorVerify:' || true)
for n in $(sed -n 's/^Self-check mismatches: //p' "$work"/shard.*.out); do
  checks=$((checks + n))
done
if [ -n "${KEEP_LOGS:-}" ]; then mkdir -p "$KEEP_LOGS"; cp "$work"/shard.*.out "$work"/shard.*.xe.log "$KEEP_LOGS"/ 2>/dev/null; fi
# Every test's final state, against the reference run's.
state_diff=0
cat "$work"/shard.*.state 2>/dev/null | sort > "$work/state"
if [ -n "${STATE_OUT:-}" ]; then cp "$work/state" "$STATE_OUT"; fi
if [ -n "${STATE_REF:-}" ]; then
  grep -v '^#' "$here/nondeterministic.txt" | grep . > "$work/nondet" || true
  names() { cut -d' ' -f1 "$1" | sort -u; }
  # only the tests both runs have (a run may cover a subset of the suites)
  join <(names "$work/state") <(names "$STATE_REF") | grep -v -x -F -f "$work/nondet" > "$work/common" || true
  # The code size is for comparing two builds with the same settings; with
  # different settings it differs by design.
  strip() {
    if [ -n "${STATE_KEEP_CODESIZE:-}" ]; then cat; else sed -E 's/ codesize=[0-9]+//'; fi \
      | if [ -n "${STATE_IGNORE:-}" ]; then sed -E "s/ ${STATE_IGNORE}=[0-9a-f]+//"; else cat; fi
  }
  join "$work/common" "$work/state" | strip > "$work/a"
  join "$work/common" "$STATE_REF" | strip > "$work/b"
  if ! cmp -s "$work/a" "$work/b"; then
    state_diff=$(diff "$work/a" "$work/b" | grep -c '^<' || true)
    echo "::error::$state_diff tests left a different state than the reference run; the first:"
    diff "$work/a" "$work/b" | head -20
  fi
  echo "${LABEL:-tests}: $(wc -l < "$work/common") states compared with the reference, $state_diff differ"
fi
echo "${LABEL:-tests}: $total run, $passed passed, $failed failed, $checks self-check mismatches [$*]"
[ "$bad" = 0 ] && [ "$failed" = 0 ] && [ "$checks" = 0 ] && [ "$total" -gt 0 ] && [ "$state_diff" = 0 ]

#!/usr/bin/env bash
# hakuX-Boosted: runs the PowerPC instruction tests in parallel shards and
# prints one total.
#
#   run-tests.sh <runner binary> <directory of .bin/.map files> [runner flags...]
#
# Environment:
#   JOBS     shards to run at once (default: the number of cores)
#   WRAPPER  a command to run the binary under, for example
#            "qemu-aarch64 -L /usr/aarch64-linux-gnu" on a host that is not ARM64
#   SUITES   a regular expression over suite names (instr_add, seq_branch_carry):
#            only those run (default: all)
#   LABEL    printed with the total
#   STATE_OUT  a file to write every test's final state to (one line a test,
#              sorted: registers, flags and memory as hashes)
#   STATE_REF  such a file from another run: this run's states must equal it,
#              test for test (the tests named in nondeterministic.txt apart)
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
i=0
while read -r suite; do
  echo "$suite" >> "$work/shard.$((i % jobs))"
  i=$((i + 1))
done < "$work/all"

# Upstream's skip list and this fork's own (known-failures.txt) as one file.
# A line "name unless <flag>" of known-failures.txt applies only to runs
# without that flag.
grep -h -v '^#' "$tests/skip.txt" | grep . > "$work/skip"
grep -v '^#' "$here/known-failures.txt" | grep . | while read -r name unless flag; do
  if [ -z "$unless" ] || ! echo " $* " | grep -q -F -- " $flag "; then echo "$name"; fi
done >> "$work/skip"

pids=()
for shard in "$work"/shard.*; do
  (
    # shellcheck disable=SC2086
    ${WRAPPER:-} "$runner" --test_path="$tests" --test_bin_path="$bins" \
      --test_skip_file="$work/skip" --log_file="$shard.xe.log" \
      --test_state_dump="$shard.state" "$@" \
      $(cat "$shard") > "$shard.out" 2>&1
    echo $? > "$shard.status"
  ) &
  pids+=($!)
done
wait "${pids[@]}"

total=0; passed=0; failed=0; bad=0
for shard in "$work"/shard.[0-9]*; do
  case $shard in *.out|*.status|*.log|*.state) continue ;; esac
  status=$(cat "$shard.status" 2>/dev/null || echo 255)
  t=$(sed -n 's/^Total tests: //p' "$shard.out" | tail -1)
  p=$(sed -n 's/^Passed: //p' "$shard.out" | tail -1)
  f=$(sed -n 's/^Failed: //p' "$shard.out" | tail -1)
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
checks=$(cat "$work"/shard.*.out "$work"/shard.*.xe.log 2>/dev/null \
  | grep -c -E 'PinVerify:|SimplifySkipVerify:|TranslatorVerify:' || true)
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
  join "$work/common" "$work/state" > "$work/a"
  join "$work/common" "$STATE_REF" > "$work/b"
  if ! cmp -s "$work/a" "$work/b"; then
    state_diff=$(diff "$work/a" "$work/b" | grep -c '^<' || true)
    echo "::error::$state_diff tests left a different state than the reference run; the first:"
    diff "$work/a" "$work/b" | head -20
  fi
  echo "${LABEL:-tests}: $(wc -l < "$work/common") states compared with the reference, $state_diff differ"
fi
echo "${LABEL:-tests}: $total run, $passed passed, $failed failed, $checks self-check lines [$*]"
[ "$bad" = 0 ] && [ "$failed" = 0 ] && [ "$checks" = 0 ] && [ "$total" -gt 0 ] && [ "$state_diff" = 0 ]

#!/usr/bin/env bash
# hakuX-Boosted bench: host instructions run per guest instruction on the
# translator's bench loops, counted under qemu-user. For a machine whose
# kernel gives no instruction counter (a cloud session's container): the
# workflow's runner has one and uses bench_core's own measurement.
#
#   xlat-count-qemu.sh <directory holding bench_core> [name=value ...]
#
# Each program is run twice (64 and 2,064 passes) with qemu logging every
# instruction executed inside the code cache's address range; the difference
# of the two counts over the difference in guest instructions is the figure.
# It counts translated code and the thunks only: what a host function does
# after the guest-to-host thunk has called it is outside the range, so the
# host_calls figure is lower than the runner's, which counts everything.
#
# Environment: QEMU (default "qemu-aarch64 -L /usr/aarch64-linux-gnu"),
# PROGRAMS (default: all), DUMP (a directory: each function's machine code
# is written there, and disassembled if aarch64 objdump is found).
set -euo pipefail
dir=${1:?directory holding bench_core}
shift
qemu=${QEMU:-qemu-aarch64 -L /usr/aarch64-linux-gnu}
programs=${PROGRAMS:-integer float vector calls copy host_calls}
args=()
for setting in "$@"; do args+=(--set "$setting"); done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
small=64
large=2064

count() {  # count <program> <passes>: prints "<host instructions> <guest instructions>"
  local range out
  # A first run for the code cache's address range (the same every run:
  # qemu-user lays the address space out the same way each time).
  out=$(BENCH_XLAT_TRACE="$1:$2" BENCH_XLAT_DUMP="${DUMP:-}" $qemu "$dir/bench_core" --suite=xlat --label=count "${args[@]}" 2>/dev/null | grep '^xlat-trace' || true)
  range=$(echo "$out" | sed -n 's/^xlat-trace code_cache=\(0x[0-9a-f]*\)\.\.\(0x[0-9a-f]*\)$/\1..\2/p')
  if [ -z "$range" ] || ! echo "$out" | grep -q ' ok=1$'; then
    echo "FAILED: $out" >&2
    return 1
  fi
  BENCH_XLAT_TRACE="$1:$2" $qemu -one-insn-per-tb -d exec,nochain -dfilter "$range" -D "$work/log" \
    "$dir/bench_core" --suite=xlat --label=count "${args[@]}" > "$work/out" 2> "$work/err"
  if ! grep -q "code_cache=${range/../\\.\\.}" "$work/out"; then
    echo "FAILED: the counting run stopped, or the code cache moved between two runs:" >&2
    grep -v MIDR_EL1 "$work/err" | tail -3 >&2
    return 1
  fi
  echo "$(grep -c '^Trace' "$work/log") $(sed -n 's/.* guest_insns=\([0-9]*\) ok=1$/\1/p' "$work/out")"
}

status=0
for program in $programs; do
  if ! a=$(count "$program" $small) || ! b=$(count "$program" $large); then
    printf '%-12s NOT MEASURED\n' "$program"
    status=1
    continue
  fi
  set -- $a $b
  awk -v p="$program" -v h1="$1" -v g1="$2" -v h2="$3" -v g2="$4" \
    'BEGIN { printf "%-12s %7.3f host instructions per guest instruction (%d over %d)\n", p, (h2 - h1) / (g2 - g1), h2 - h1, g2 - g1 }'
done
if [ -n "${DUMP:-}" ] && command -v aarch64-linux-gnu-objdump > /dev/null; then
  for bin in "$DUMP"/*.bin; do
    aarch64-linux-gnu-objdump -D -b binary -m aarch64 "$bin" | sed '1,7d' > "${bin%.bin}.s"
  done
fi
exit $status

#!/usr/bin/env bash
# hakuX-Boosted: runs the differential test for a set time and writes the
# report.
#
#   run.sh <build dir> <out dir> [minutes] [first seed] [cases a batch]
#
# The build directory holds dt_ref.bin (build-ref.sh) and xenia-jit-difftest
# (the CMake project in the core's cpu/ppc/testing/ci with -DDT_GEN). A batch
# is one seed and tier: the reference runs it under qemu-system-ppc64, then
# the runner runs it and compares. Seeds count up from the first one, each as
# a batch of single instructions and a batch of sequences, until the time is
# up; the same seeds give the same cases on any machine, so a failure is
# reproduced with its seed, tier and case number:
#
#   run.sh <build dir> <out dir> 0 <seed>      (0 minutes: that one seed)
#
# DT_WRAPPER runs the runner when it was cross-compiled (for instance
# "qemu-aarch64 -L /usr/aarch64-linux-gnu"). Exit status 1 when anything
# differs that known-issues.txt does not list.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=$1
out=$2
minutes=${3:-30}
seed=${4:-1}
batch=${5:-20000}
wrapper=${DT_WRAPPER:-}
qemu=${DT_QEMU:-qemu-system-ppc64}
mkdir -p "$out"
rm -f "$out/state.json" "$out/failures.txt" "$out/regressions.log"
deadline=$(( $(date +%s) + minutes * 60 ))

# The fixed cases first: what was found and fixed stays fixed.
$wrapper "$build/xenia-jit-difftest" --dt_regress="$here/regressions.txt" > "$out/regressions.log" 2>/dev/null
tail -1 "$out/regressions.log"

run_batch() {  # seed tier
  python3 -c "
import struct,sys
open(sys.argv[1],'wb').write(struct.pack('>5Q',0x4454504152414D53,$1,0,$batch,$2))" "$out/params.bin"
  rm -f "$out/ref.out" "$out/batch.jsonl"
  if ! timeout 300 "$qemu" -M pseries -cpu 970_v2.2 -m 512M -nographic -nodefaults \
      -monitor none -serial "file:$out/ref.out" -bios "$build/dt_ref.bin" \
      -device "loader,file=$out/params.bin,addr=0x800000,force-raw=on" 2> /dev/null; then
    echo "seed $1 tier $2: the reference did not finish" >> "$out/failures.txt"
    return
  fi
  timeout 600 $wrapper "$build/xenia-jit-difftest" --dt_seed="$1" --dt_tier="$2" --dt_start=0 \
      --dt_count="$batch" --dt_ref="$out/ref.out" --dt_out="$out/batch.jsonl" 2> "$out/batch.err"
  status=$?
  if [ -s "$out/batch.jsonl" ]; then python3 "$here/dt_report.py" merge "$out/state.json" "$out/batch.jsonl"; fi
  if [ $status -ne 0 ]; then
    echo "seed $1 tier $2: the runner ended with status $status: $(tail -1 "$out/batch.err")" >> "$out/failures.txt"
  fi
}

while :; do
  run_batch "$seed" 1
  run_batch "$seed" 2
  seed=$((seed + 1))
  [ "$(date +%s)" -ge "$deadline" ] && break
done
rm -f "$out/ref.out" "$out/batch.jsonl" "$out/batch.err" "$out/params.bin"
python3 "$here/dt_report.py" render "$out/state.json" "$here/known-issues.txt" "$out"

#!/usr/bin/env bash
# hakuX-Boosted: assembles the PowerPC instruction tests (../*.s) into the
# .bin and .map files the runner loads. The same steps as `xb gentests`
# (xenia-build.py), without the rest of that script.
#
#   gen-tests.sh <directory holding powerpc-none-elf-as and friends> <output directory>
#
# The assembler must be binutils 2.24 with third_party/binutils'
# vmx128 patch (build-binutils.sh beside this file builds it).
set -euo pipefail
bin=$1
out=$2
src=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
one() {
  local s=$1 name
  name=$(basename "$s" .s)
  "$bin/powerpc-none-elf-as" -a32 -be -mregnames -ma2 -maltivec -mvsx -mvmx128 -R \
    -o "$out/$name.o" "$s"
  "$bin/powerpc-none-elf-ld" -A powerpc:common32 -melf32ppc -EB -nostdlib \
    --oformat=binary -Ttext=0x80000000 -e0x80000000 -o "$out/$name.bin" "$out/$name.o"
  "$bin/powerpc-none-elf-nm" --numeric-sort "$out/$name.o" > "$out/$name.map"
}
export -f one
export bin out
ls "$src"/instr_*.s "$src"/seq_*.s | xargs -P "$(nproc)" -I{} bash -c 'one "$1"' _ {}
echo "assembled $(ls "$out"/*.bin | wc -l) test files into $out"

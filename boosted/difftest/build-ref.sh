#!/usr/bin/env bash
# hakuX-Boosted: builds the reference harness (dt_ref.bin), a bare-metal
# big-endian PowerPC image, with clang and lld (no PowerPC C library needed).
#
#   build-ref.sh <ppc-instructions.xml> <output directory>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
xml=$1
out=$2
mkdir -p "$out"
python3 "$here/dt_table.py" "$xml" "$out/dt_table.inc" "$here/known-issues.txt"
flags=(--target=powerpc64-unknown-linux-gnu -mabi=elfv2 -mcpu=970 -mno-altivec
  -ffreestanding -fno-pic -fno-stack-protector -fno-builtin -O2 -I"$out" -I"$here")
clang "${flags[@]}" -c "$here/dt_ref_start.S" -o "$out/dt_ref_start.o"
clang "${flags[@]}" -Wall -c "$here/dt_ref.c" -o "$out/dt_ref.o"
ld.lld -static -nostdlib -T "$here/dt_ref.ld" "$out/dt_ref_start.o" "$out/dt_ref.o" -o "$out/dt_ref.elf"
llvm-objcopy -O binary "$out/dt_ref.elf" "$out/dt_ref.bin"
ls -l "$out/dt_ref.bin"

#!/usr/bin/env python3
"""hakuX-Boosted jit bench: builds workloads.c into the image the benchmark
loads, with the results the host's C compiler gives for the same code.

    build-workloads.py <output directory>

Writes workloads.img (a flat image for guest address 0x82000000) and
workloads.manifest (text; core/bench_jit.cc reads it). Needs
gcc-powerpc-linux-gnu and a host cc. PPC_CC and HOST_CC name others.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PPC_CC = os.environ.get("PPC_CC", "powerpc-linux-gnu-gcc")
PREFIX = PPC_CC[: -len("gcc")] if PPC_CC.endswith("gcc") else "powerpc-linux-gnu-"
HOST_CC = os.environ.get("HOST_CC", "cc")

COMMON = ["-O2", "-fno-asynchronous-unwind-tables",
          "-fno-stack-protector", "-ffp-contract=off", "-fno-tree-vectorize",
          "-fno-builtin-memset", "-fno-builtin-memcpy", "-fno-tree-loop-distribute-patterns",
          "-fno-strict-aliasing"]
# The console's code: 64-bit registers in a 32-bit address space, AltiVec.
PPC = ["-ffreestanding", "-fno-pic", "-m32", "-mcpu=cell", "-mpowerpc64", "-maltivec", "-mabi=altivec", "-mno-sdata", "-G", "0",
       "-fno-optimize-sibling-calls"]
SEED = 0x1234ABCD

# name -> (n for the checked short run, n for the timed run). The timed
# lengths aim at some tens of milliseconds of translated code a run.
RUNS = {
    "integer": (2, 3000), "int64": (2, 24000), "branchy": (2, 120), "interp": (2, 600),
    "memory": (3, 9000), "strings": (2, 1200), "calls": (2, 3000), "virtual": (2, 480),
    "float": (2, 600), "single": (2, 600), "particles": (3, 800), "vector": (2, 1500),
}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    names = [l.strip()[2:-1] for l in open(os.path.join(HERE, "workloads.def")) if l.startswith("W(")]
    assert set(names) == set(RUNS), "workloads.def and RUNS disagree"

    host = os.path.join(out, "workloads_host")
    run([HOST_CC] + COMMON + ["-I", HERE, "-o", host, os.path.join(HERE, "host_main.c"),
                              os.path.join(HERE, "workloads.c"), "-lm"])
    args = []
    for name in names:
        for n in RUNS[name]:
            args += [name, str(n), str(SEED)]
    expected = {}
    for line in run([host] + args, capture_output=True, text=True).stdout.splitlines():
        name, n, _seed, result = line.split()
        expected[(name, int(n))] = int(result)

    elf = os.path.join(out, "workloads.elf")
    run([PPC_CC] + COMMON + PPC + ["-nostdlib", "-static", "-Wl,--build-id=none", "-Wl,-T," + os.path.join(HERE, "workloads.ld"),
                                   "-o", elf, os.path.join(HERE, "workloads.c")])
    run([PREFIX + "objcopy", "-O", "binary", elf, os.path.join(out, "workloads.img")])
    symbols = {}
    functions = []
    for line in run([PREFIX + "nm", "-n", elf], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        symbols[parts[2]] = int(parts[0], 16)
        if parts[1] in "tT":
            functions.append(int(parts[0], 16))
    undefined = run([PREFIX + "nm", "-u", elf], capture_output=True, text=True).stdout.strip()
    assert not undefined, "the image calls a library: " + undefined

    base = 0x82000000
    with open(os.path.join(out, "workloads.manifest"), "w") as m:
        m.write("image workloads\nfile workloads.img\n")
        m.write("base %08X\nfile_base %08X\nsize %X\n" % (base, 0x82001000, symbols["__image_end"] - base))
        m.write("text %08X %08X\n" % (0x82001000, symbols["__data_start"]))
        for address in sorted(set(functions)):
            m.write("fn %08X\n" % address)
        for name in names:
            short, timed = RUNS[name]
            m.write("run %s %08X %u %u %u check\n" % (name, symbols["wl_" + name], short, SEED, expected[(name, short)]))
            m.write("run %s %08X %u %u %u timed\n" % (name, symbols["wl_" + name], timed, SEED, expected[(name, timed)]))
    print("workloads: %d functions, %d bytes of code" % (len(set(functions)), symbols["__data_start"] - 0x82001000))


if __name__ == "__main__":
    main()

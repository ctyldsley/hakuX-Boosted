#!/usr/bin/env python3
"""hakuX-Boosted: generates the seq_cc_*.s sequence tests from C.

Each C file beside this script is compiled twice: for PowerPC (the code under
test, written into ../../seq_cc_<name>.s with a small wrapper per test case)
and for the host, where the same functions are run with the same arguments to
get the expected results. The translator is therefore checked against the
host's C compiler, not against itself.

Needs gcc-powerpc-linux-gnu and a host cc. The generated files are checked
in, so CI does not run this; run it again after changing a kernel or a case:

    python3 gen-seq-tests.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", ".."))

PPC_CC = os.environ.get("PPC_CC", "powerpc-linux-gnu-gcc")
HOST_CC = os.environ.get("HOST_CC", "cc")

COMMON = ["-O2", "-ffreestanding", "-fno-pic", "-fno-asynchronous-unwind-tables",
          "-fno-stack-protector", "-ffp-contract=off", "-fno-tree-vectorize",
          "-fno-builtin-memset", "-fno-builtin-memcpy",
          "-fno-tree-loop-distribute-patterns"]
PPC = ["-m32", "-mcpu=cell", "-mno-sdata", "-G", "0", "-mregnames"]

U32, U64, F32, F64 = "u32", "u64", "f32", "f64"

# file -> (extra PowerPC flags, has host oracle, [(function, result type,
#          [argument types], [argument tuples])])
FILES = {
    "int": ("kernels.c", [], True, [
        ("k_call_loop", U32, [U32, U32], [(0, 5), (1, 7), (2, 0x1234), (9, 0xDEADBEEF), (40, 1)]),
        ("k_cond_call_loop", U32, [U32, U32], [(0, 4), (1, 4), (7, 3), (33, 0x80000004), (64, 0x55AA55AA)]),
        ("k_collatz", U32, [U32, U32], [(1, 10), (7, 100), (27, 200), (97, 30), (0xFFFF, 500)]),
        ("k_gcd", U32, [U32, U32], [(12, 18), (17, 5), (0, 9), (9, 0), (0xFFFFFFFF, 0x10001), (1071, 462)]),
        ("k_crc32", U32, [U32, U32], [(0, 1), (1, 1), (17, 0x31), (64, 0x1234567), (200, 0xFFFFFFFF)]),
        ("k_sort", U32, [U32, U32], [(0, 1), (1, 2), (5, 3), (16, 0xC0FFEE), (32, 99), (50, 7)]),
        ("k_matrix", U32, [U32, U32], [(0, 1), (3, 2), (8, 0xABCDEF), (20, 5)]),
        ("k_fib", U32, [U32], [(0,), (1,), (2,), (9,), (15,)]),
        ("k_ack", U32, [U32, U32], [(0, 0), (1, 3), (2, 2), (2, 5)]),
        ("k_dispatch", U32, [U32, U32], [(0, 9), (1, 0), (12, 0x12345), (80, 0xFEEDFACE)]),
        ("k_switch", U32, [U32, U32], [(0, 9), (1, 3), (8, 0), (50, 0x1F2E3D4C), (300, 77)]),
        ("k_mix64", U64, [U64, U64], [(0, 0), (1, 2), (0xFFFFFFFFFFFFFFFF, 1), (0x0123456789ABCDEF, 0xFEDCBA9876543210)]),
        ("k_search", U32, [U32, U32], [(0, 1), (0x42, 2), (0xFF, 0x999), (0x17, 0xDEADBEEF)]),
        ("k_pressure", U32, [U32, U32], [(0, 1), (1, 2), (13, 0xCAFEBABE), (100, 0x10203040)]),
        ("k_classify", U32, [U32, U32], [(0, 1), (5, 2), (64, 0x77777777), (257, 0xFFFFFFFF)]),
        ("k_signed", U32, [U32, U32], [(0, 1), (1, 0), (9, 0x7FFFFF), (77, 0x123456), (300, 0xFFFFFFFF)]),
        ("k_mixer", F32, [U32, F32, F32], [(0, 1.0, 0.0), (5, 0.5, 0.125), (32, 1.5, -0.03125), (100, 0.25, 0.0078125)]),
        ("k_dot", F32, [U32, F32], [(0, 1.0), (3, 0.5), (16, 0.25), (50, 0.0625)]),
        ("k_horner", F64, [U32, F64], [(0, 2.0), (4, 0.5), (40, 1.5), (100, -1.25)]),
        ("k_fcall_loop", F64, [U32, F64], [(0, 1.0), (1, 3.0), (20, 150.0), (99, -7.5)]),
        ("k_convert", U32, [U32, F32], [(0, 1.0), (3, 1.5), (40, 2.0), (100, 0.75)]),
        ("k_fcompare", U32, [U32, F64], [(0, 1.0), (9, 4.0), (50, -3.5), (128, 12.0)]),
    ]),
    "64": ("kernels64.c", ["-mpowerpc64"], True, [
        ("k64_mul", U32, [U32, U32, U32], [(0, 0, 0), (1, 2, 1), (0xDEADBEEF, 0x12345678, 7), (0xFFFFFFFF, 0xFFFFFFFF, 40)]),
        ("k64_div", U32, [U32, U32, U32], [(0, 1, 0), (1, 1, 1), (0x80000000, 3, 9), (0xFFFFFFFF, 0xFFFFFFFE, 33)]),
        ("k64_shift", U32, [U32, U32, U32], [(0, 0, 0), (0x80000000, 1, 1), (0x7FFFFFFF, 0xFFFFFFFF, 20), (0xCAFEBABE, 0x0BADF00D, 70)]),
        ("k64_compare", U32, [U32, U32, U32], [(0, 0, 0), (1, 1, 3), (0x80000000, 0, 30), (0x13579BDF, 0x2468ACE0, 90)]),
    ]),
    "vec": ("kernels_vec.c", ["-maltivec", "-mabi=altivec"], False, [
        ("kv_mixer", U32, [U32, U32], [(0, 1), (1, 2), (16, 3), (100, 0x1234)]),
        ("kv_int", U32, [U32, U32], [(0, 1), (1, 2), (9, 0xDEADBEEF), (64, 0x80000001)]),
    ]),
}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def host_results(src, cases):
    """Runs every case on the host; returns {(function, index): text}."""
    lines = ['#include <stdio.h>', '#include <string.h>', '#include "%s"' % src, 'int main(void) {']
    for fn, rtype, atypes, tuples in cases:
        for n, args in enumerate(tuples):
            parts = []
            for t, a in zip(atypes, args):
                if t == U32:
                    parts.append("0x%Xu" % a)
                elif t == U64:
                    parts.append("0x%Xull" % a)
                elif t == F32:
                    parts.append("%sf" % float(a).hex())
                else:
                    parts.append("%s" % float(a).hex())
            call = "%s(%s)" % (fn, ", ".join(parts))
            if rtype == U32:
                lines.append('  printf("%s %d %%08x\\n", (unsigned)%s);' % (fn, n, call))
            elif rtype == U64:
                lines.append('  printf("%s %d %%016llx\\n", (unsigned long long)%s);' % (fn, n, call))
            else:
                # both float kinds come back in an FPR as a double
                lines.append('  { double d = (double)%s; unsigned long long u; memcpy(&u, &d, 8);'
                             ' printf("%s %d %%016llx\\n", u); }' % (call, fn, n))
    lines += ['  return 0;', '}']
    with tempfile.TemporaryDirectory() as tmp:
        main_c = os.path.join(tmp, "host.c")
        with open(main_c, "w") as f:
            f.write("\n".join(lines) + "\n")
        exe = os.path.join(tmp, "host")
        # -O1 and no contraction: IEEE results, the same as the PowerPC code's
        run([HOST_CC, "-O1", "-ffp-contract=off", "-fno-tree-vectorize", "-msse2", "-mfpmath=sse",
             "-I", HERE, main_c, "-o", exe, "-lm"])
        out = run([exe], capture_output=True, text=True).stdout
    results = {}
    for line in out.splitlines():
        fn, n, value = line.split()
        results[(fn, int(n))] = value
    return results


DROP = re.compile(r"^\s*\.(file|machine|type|size|ident|gnu_attribute|globl|section\s+\.note)\b")


def ppc_asm(src, extra):
    with tempfile.TemporaryDirectory() as tmp:
        s = os.path.join(tmp, "k.s")
        run([PPC_CC] + COMMON + PPC + extra + ["-S", os.path.join(HERE, src), "-o", s])
        text = open(s).read()
    keep = []
    for line in text.splitlines():
        if DROP.match(line):
            continue
        # the tests' assembler takes r3, not %r3
        keep.append(line.replace("%", ""))
    return "\n".join(keep) + "\n"


def wrapper(fn, n, rtype, atypes, args, expected):
    out = ["test_%s_%d:" % (fn, n)]
    gpr, fpr = 3, 1
    for t, a in zip(atypes, args):
        if t == U32:
            out.append("  #_ REGISTER_IN r%d 0x%08X" % (gpr, a))
            gpr += 1
        elif t == U64:
            # the 32-bit ABI: an even/odd register pair, high half first
            if gpr % 2 == 0:
                gpr += 1
            out.append("  #_ REGISTER_IN r%d 0x%08X" % (gpr, a >> 32))
            out.append("  #_ REGISTER_IN r%d 0x%08X" % (gpr + 1, a & 0xFFFFFFFF))
            gpr += 2
        else:
            v = float(a)
            if t == F32:
                v = struct.unpack("f", struct.pack("f", v))[0]
            out.append("  #_ REGISTER_IN f%d %s" % (fpr, repr(v)))
            fpr += 1
    out += [
        "  mflr r0",
        "  stwu r1, -64(r1)",
        "  stw r0, 68(r1)",
        "  bl %s" % fn,
        "  lwz r0, 68(r1)",
        "  addi r1, r1, 64",
        "  mtlr r0",
        # 32-bit results: the upper half of the register is not defined by
        # the ABI, so it is cleared before it is compared
        "  clrldi r3, r3, 32",
        "  clrldi r4, r4, 32",
        "  blr",
    ]
    if expected is not None:
        if rtype == U32:
            out.append("  #_ REGISTER_OUT r3 0x%s" % expected)
        elif rtype == U64:
            out.append("  #_ REGISTER_OUT r3 0x%s" % expected[:8])
            out.append("  #_ REGISTER_OUT r4 0x%s" % expected[8:])
        else:
            out.append("  #_ REGISTER_OUT f1 0x%s" % expected)
    return "\n".join(out) + "\n"


def main():
    for name, (src, extra, oracle, cases) in FILES.items():
        expected = host_results(src, cases) if oracle else {}
        parts = [
            "# hakuX-Boosted: generated by ci/gen/gen-seq-tests.py from ci/gen/%s.\n"
            "# Do not edit: change the C source or the case table and run the script.\n"
            "# The expected values were computed by the same C compiled for the host%s\n\n"
            % (src, "." if oracle else
               ";\n# this file has none (no host equivalent): its results are compared between runs.")
        ]
        count = 0
        for fn, rtype, atypes, tuples in cases:
            for n, args in enumerate(tuples):
                parts.append(wrapper(fn, n, rtype, atypes, args, expected.get((fn, n))))
                parts.append("\n")
                count += 1
        parts.append("# ---- compiled from %s ----\n" % src)
        parts.append(ppc_asm(src, extra))
        path = os.path.join(OUT, "seq_cc_%s.s" % name)
        with open(path, "w") as f:
            f.write("".join(parts))
        print("%s: %d cases" % (os.path.relpath(path, OUT), count))


if __name__ == "__main__":
    sys.exit(main())

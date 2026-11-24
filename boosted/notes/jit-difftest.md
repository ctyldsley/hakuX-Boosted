# The translator against a reference: the differential test

Branch `claude/jit-difftest`, cut from `claude/merge-1123` at `bd5d108` (the
code on the phone as build 159). Written 2026-11-23 to 24 by a cloud session.
Not merged anywhere.

**Nothing here has run on a device.** What is called fixed below is fixed in
the sense of this test: the ARM64 back end now gives what the reference gives
for the failing case, which is kept as a fixed case. Whether any title changes
is for the phone to say.

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless they
start with another directory.

## Done / in progress / next

Done:
- The rig (`boosted/difftest/`), the workflow
  (`.github/workflows/jit-difftest.yml`, 30 minutes on every push to
  `claude/**`, on GitHub's ARM64 runner, no phone).
- Thirteen commits to the translator, one per bug, 54 fixed cases in
  `boosted/difftest/regressions.txt` (section 3). Nine are in the front
  end or the passes (so upstream's x64 back end has them too), three in the
  ARM64 back end, one implements thirteen instructions that trapped.
- Everything else that differs is listed with its reason in
  `boosted/difftest/known-issues.txt` and in section 4.

Next, in order:
1. Put the fixes on the phone. They are small and independent. The first
   is the one that can end a hang (section 3); the thirteen instructions
   are the one that can let a title run that stopped.
2. Read through the ARM64 back end's vector sequences for the pattern three
   of the bugs share (section 3, "the pattern"): the test finds them one
   sequence at a time, a reading finds them all.
3. The things the rig does not generate yet (section 6): branches backwards
   and calls, `vpkd3d128`/`vupkd3d128`, the string instructions.
4. A second column: the same cases with the fork's translator settings on
   (`pin_guest_registers_wide`, `eliminate_dead_context_stores`, ...), which
   is one more argument to the runner (section 2).

## 1. What it is

Many short PowerPC programs, made from a seed, are run twice: through our
translator (the PowerPC front end, the passes and the ARM64 back end, as on
the phone) and on a reference CPU. After each, everything it could have
changed is compared: the 32 general registers, CTR, the 32 float registers,
the vector registers, CR, XER, FPSCR's control bits, VSCR and a 2 KiB window
of memory that every load and store is aimed at. A difference is written to
the report with the instructions, the registers they name before the run,
our result and the reference's.

Two tiers:
- **Single instructions** (tier 1): one instruction a case (a `lwarx` or
  `ldarx` with its `stwcx.`/`stdcx.` and up to two instructions between
  counts as one). Every difference is put down to that instruction.
- **Sequences** (tier 2): two to six instructions around a theme (integer;
  integer with memory; with reservations; float; float with memory; float
  with integer; VMX; VMX with VMX128; float vectors; VMX128 with memory;
  mixed), with forward conditional branches over one instruction (CR bits,
  and the CTR-decrementing forms), and one time in three a constant loaded
  first into a register the later instructions favour. These reach what one
  instruction cannot: the passes working across instructions, the back end's
  code for constant operands, more than one block.

419 of the core's 455 instructions are generated. The 36 that are not are
listed with the reason at the end of `dt_table.py` (`NOT_COVERED`) and in
section 6.

### The reference

`qemu-system-ppc64` with its PowerPC 970 (64-bit, big-endian, AltiVec,
nothing later: the nearest model to the console's CPU), running a bare-metal
harness of ours (`dt_ref.c`, `dt_ref_start.S`) as the machine's firmware.

Not QEMU's user mode, which the task named: **the console runs titles in the
CPU's 32-bit mode with 64-bit registers**, and the translator is written for
that (the carry, CR0 of the record forms and the CTR test of `bdnz` come from
the low 32 bits; `AddDidCarry`, `UpdateCR` and `InstrEmit_bcx` all truncate).
A Linux user process on a 64-bit PowerPC runs in 64-bit mode and cannot leave
it, so in user mode every record form and every carry would differ for
reasons that are not the translator's. The harness clears `MSR[SF]` itself.
It also owns the whole machine, so all 32 general registers belong to the
case (its own state is addressed absolutely) and an exception a case takes is
caught and reported.

For VMX128, which QEMU does not have:
- An instruction with a plain VMX equivalent runs as that equivalent on the
  reference, with the registers renamed: the case's vector registers are
  "slots" 0 to 31, v0 to v31 on the reference and spread over v0 to v127 on
  our side. `vmulfp128` runs as `vmaddfp` with -0.0 added.
- The console-only ones with no equivalent (`lvlx`, `lvrx`, `stvlx`, `stvrx`
  and their `128` and `l` forms, `vpermwi128`, `vrlimi128`, `vmsum3fp128`,
  `vmsum4fp128`) are computed by C in `dt_pseudo.h`, written from the
  instruction descriptions and from what the front end's own comments in
  `cpu/ppc/ppc_emit_altivec.cc` say they do. For these the test checks the
  back end against that reading, not against a console.

### What is left out of the comparison, and why

- FPSCR's status bits (everything above the low byte), and CR1 after a float
  record form: the translator keeps no float status (`UpdateFPSCR`).
- The upper 32 bits of the result of `mulhw`, `mulhwu`, `divw`, `divwu`,
  `fctiw`, `fctiwz`: undefined by the architecture. Such a register is also
  not read again in the case.
- The other elements after `lvebx`, `lvehx`, `lvewx`: undefined.
- Divisions by zero and of the most negative number by -1 are not generated
  (result undefined): the operands are changed.
- The estimates (`fres`, `frsqrte`, `vrefp`, `vrsqrtefp`, `vexptefp`,
  `vlogefp`) and the dot products are compared within 1/32, same class
  (zero, infinity, NaN) required.
- Denormal inputs to float vector instructions are not generated: QEMU does
  not treat them as the console does in non-Java mode, which the back end
  imitates on purpose (`VectorDenormFlush`).
- In sequences only (single instructions are compared in full, so that the
  report shows it): CR's summary overflow bits, XER[SO] and XER[OV], VSCR's
  saturation bit, and the sign and payload of a NaN where both sides have a
  NaN. The translator keeps none of the first three (section 4), and they
  are kept from reaching anything else in a sequence (no branch on an SO
  bit, no `mfcr`, no OE form, no `mtspr XER`, no NaN among the inputs).

## 2. Running it

In CI: push to a `claude/**` branch, or run the workflow "hakuX-Boosted JIT
difftest" by hand (inputs: minutes, first seed). The run's summary page shows
the report; the artifact `jit-difftest-report` holds `report.md`,
`regression-candidates.txt` (each new difference as a line ready for
`regressions.txt`), `summary.json` and `state.json` (the counts and the first
three examples of every group).

**Green** means: nothing differed that `known-issues.txt` does not list, every
case of `regressions.txt` passed, every batch ran to its end.

The seeds are fixed: the run starts at seed 1 and counts up until the time is
over, each seed as 20,000 single instructions and 20,000 sequences. The same
seed, tier and case number are the same case on any machine, so a line of the
report ("seed 7, tier 2, case 5593") is reproduced with

    boosted/difftest/run.sh <build> <out> 0 7        # that seed, both tiers

In a cloud session's container (x86): Ubuntu's `qemu-user`,
`qemu-system-ppc`, `clang`, `lld`, `llvm`, `g++-aarch64-linux-gnu`,
`ninja-build`, `ccache`, then

    XE=x360/emulator-core/src/main/cpp/xenia; ci=$XE/src/xenia/cpu/ppc/testing/ci
    git submodule update --init --depth 1 -- $XE/third_party/{xbyak_aarch64,fmt,capstone,cxxopts,date,tomlplusplus,utfcpp,xxhash,disruptorplus,boost_context/context}
    boosted/difftest/build-ref.sh $XE/tools/ppc-instructions.xml /tmp/dt
    cmake -S $ci -B /tmp/dt -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/$ci/aarch64-cross.cmake -DDT_GEN=/tmp/dt
    ninja -C /tmp/dt xenia-jit-difftest
    DT_WRAPPER="qemu-aarch64 -L /usr/aarch64-linux-gnu" boosted/difftest/run.sh /tmp/dt /tmp/dt-out 0 1

(about five minutes a seed there; the runner under `qemu-aarch64` is the slow
part. On the ARM64 runner it is native.)

One case from the report or from `regressions.txt`, with the translator's
intermediate code and machine code beside it:

    echo '<the line>' > one.txt
    xenia-jit-difftest --dt_regress=one.txt --dump_translated_hir_functions=true

The runner takes every setting of the core on its command line, so a
translator setting is tried by adding it to the two `xenia-jit-difftest`
lines of `run.sh` (not yet a second column of the workflow).

### The files

| File | What |
|---|---|
| `dt_table.py` | The instruction table: one line an instruction (class, operands, the reference's equivalent for VMX128), encodings read from the core's `tools/ppc-instructions.xml` |
| `dt_gen.h` | The generator: a case from (seed, tier, index). Plain C, compiled into both sides, so nothing but results passes between them |
| `dt_pseudo.h` | C results for the console-only instructions |
| `dt_ref.c`, `dt_ref_start.S`, `dt_ref.ld`, `build-ref.sh` | The reference harness (bare metal, clang and lld, no PowerPC C library) |
| `dt_jit.cc` | The runner: our translator, the comparison, the JSON lines, the fixed cases. Built by the instruction tests' CMake project (`cpu/ppc/testing/ci`) with `-DDT_GEN` |
| `dt_report.py` | Merges the batches and writes the report |
| `run.sh` | A timed run |
| `known-issues.txt` | Differences the run does not fail on, each with its reason |
| `regressions.txt` | Fixed cases: instructions, inputs, expected state. Run first in every run, no reference needed |

When a difference is fixed: take its line from `regression-candidates.txt`
into `regressions.txt`, check that it fails before the change and passes
after, and remove its line from `known-issues.txt` if it had one (that also
lets the instruction back into the sequences).

## 3. Found and fixed

Each is its own commit with its cases in `regressions.txt`. "Sequence" in
the last column means no single instruction shows it: it needs a constant
operand or a value carried from another instruction.

| Commit | What was wrong | Where | Found by |
|---|---|---|---|
| `86dcc71` | **A `lwarx` or `ldarx` whose loaded value is never read was deleted**, and the `stwcx.`/`stdcx.` after it then failed every time. The dead code pass spares an instruction with an effect of its own when nothing reads its result, but its recursion did not ask: when the last reader of the result (the byte swap) was removed, the reserving load went with it | `cpu/compiler/passes/dead_code_elimination_pass.cc` | single |
| `97fa0d2` | `mtfsfi` wrote twice its immediate: `mtfsfi 7,1` (round toward zero) set the rounding mode to "toward +infinity" | `cpu/ppc/ppc_emit_fpu.cc` | single |
| `c792e01` | `mtfsf` applied its field mask mirrored: `mtfsf 0x01` wrote FPSCR's top four bits and left the rounding mode; only `0xFF` and palindromic masks were right | `cpu/ppc/ppc_emit_fpu.cc` | single |
| `1a7f621` | `addco`, `addmeo`, `addzeo`, `subfco`, `subfeo`, `subfmeo`, `subfzeo` did not write the carry | `cpu/ppc/ppc_emit_alu.cc` | single |
| `a75c8eb` | `vcmpbfp`/`vcmpbfp128` set one of the two out-of-bounds bits for a NaN, not both; the record form set `CR6[0]` when every element was out of bounds | `cpu/ppc/ppc_emit_altivec.cc` | single |
| `0250a58`, `6beec23` | `sraw` by 32 or more of exactly `0x80000000`, and `srad` by 64 or more of exactly `0x8000000000000000`, cleared the carry | `cpu/ppc/ppc_emit_alu.cc` | sequence |
| `6e38f5f` | **Thirteen AltiVec instructions were not implemented and trapped**: `vmuleub`, `vmuleuh`, `vmulesb`, `vmulesh`, `vmuloub`, `vmulouh`, `vmulosb`, `vmulosh`, `vmhaddshs`, `vmhraddshs`, `vmladduhm`, `vupkhpx`, `vupklpx`. Written element by element with scalar operations: right, not fast | `cpu/ppc/ppc_emit_altivec.cc` | single |
| `dc55c89` | `vrlb`, `vrlh`, `vrlw` of a constant by a register were wrong (the pattern below) | `cpu/backend/a64/a64_seq_vector.cc` | sequence |
| `2cad28a` | `vsl`, `vsr` of a constant by a register were wrong (the pattern below) | `cpu/backend/a64/a64_sequences.cc` | sequence |
| `10168e7` | The constant pass folded a vector insert as one byte whatever its size, and a vector extract from the wrong byte or halfword of its word (and up to 15 bytes past the vector) | `cpu/compiler/passes/constant_propagation_pass.cc`, `cpu/hir/value.cc` | sequence |
| `29b8a88` | The constant pass folded a float vector equality as a comparison of bit patterns: a NaN equal to itself, +0 not equal to -0 | `cpu/hir/value.cc` | sequence |
| `ae5d763` | An invalid scalar float operation (inf - inf, 0 * inf, 0 / 0) made x86's NaN, sign set, where PowerPC's has it clear. Upstream's own tests taken from a console expect the positive one in all 40 such cases and are all in its `skip.txt` | `cpu/backend/a64/a64_sequences.cc` | sequence |

### The pattern

Three of the back end's bugs are one mistake: a sequence loads a constant
operand into scratch register v0 (`SrcVReg(e, i.src1, 0)`), then uses v0 as
a temporary, then reads the operand again. With the operand in a register of
its own everything is right, which is why the instruction tests, whose
operands are never constants, pass. A vector constant comes from `vspltis*`,
`vxor x,x,x` and the like, so real code reaches these paths.

The test finds such a bug only when a sequence happens to feed a constant to
the instruction (a third of the sequences start with one for this reason).
Reading every `Emit` in `a64_seq_vector.cc` and `a64_sequences.cc` that
calls `SrcVReg(..., 0)` or `(..., 1)` and writes `VReg(0)` or `VReg(1)`
before its last read of the operand would find the rest at once. Not done
here.

The constant pass is the same story one level up: its folding code
(`hir/value.cc`) is only run when every operand is a constant, and three of
its vector folds were wrong.

### On the first one and the Gears of War freeze

It is the only finding that turns a loop into one that cannot end, which is
what a freeze with busy threads looks like, so it is worth saying exactly
when it bites and that **it is not shown to be the Gears freeze**.

It needs the loaded value to be dead inside the translator: the register
`lwarx` loads into must be overwritten before anything reads it, with the
overwriting store close enough for the context promotion pass to see (the
same block by default; across blocks with the fork's
`eliminate_dead_context_stores` at 2 or 3, off by default). The usual
compare-and-swap and increment loops read the value and are not affected. A
store that does not care what was there (`lwarx r11,0,r3` / `li r11,...` or a
move into r11 / `stwcx. r4,0,r3` / `bne`) is.

Whether Gears has such a loop can be answered without playing: a scan of the
title's executable on the owner's PC for a `lwarx`/`ldarx` whose target
register is written again before it is read and before the next branch. That
scan is not written. The cheaper check is the build: if the freeze at the
cell door is this, it is gone with `86dcc71` and nothing else.

## 4. Found and not fixed

All are in `known-issues.txt`. A single instruction that differs only in
these ways does not fail the run.

State the translator does not keep (upstream's design; each would cost
instructions in every translated compare or vector operation, and no title
is known to read them):
- **CR's summary overflow bits.** `UpdateCR`, the compares and `stwcx.` write
  three of a field's four bits and leave the fourth as it was; the console
  copies XER[SO] into it. Not fixed because the fix is a load and a store
  more in every compare.
- **XER[SO] and XER[OV].** The OE forms (`addo`, `mullwo`, `divwo` ...)
  neither set nor clear them; `mtspr XER` drops them; `mfspr XER` returns
  the carry alone. Could be done for the OE forms alone at no cost to
  anything else (they are rare); not done here.
- **VSCR[SAT].** No saturating vector instruction sets it and `mtvscr` does
  not carry it; `mfvscr` therefore never shows it.
- **A NaN's sign and payload.** Both sides agree which results are NaNs. They
  differ in which operand's payload wins, in whether a signalling NaN is
  quieted on a float load (`lfs` converts with the host's instruction, which
  quiets), in the sign after `fnmadd`/`fnmsub` and `vnmsubfp` (the host
  negates a NaN, the console does not), and in the NaN a float *vector*
  operation makes from no NaN (inf - inf): ours has the sign set
  (`FixupVmxNan_V128`, behind `a64_vmx_nan_fixup`, which calls 0xFFC00000
  "the PPC default"), the reference's is 0x7FC00000. The scalar one was the
  same and is fixed (`ae5d763`), on the evidence of upstream's console
  tests; there are no such tests for the vector one, so it is left.

Wrong, small, not fixed here:
- **`mcrf`** is only right from cr0 holding "less than" as upstream
  translates it. The fork already has the exact form behind
  `ppc_mcrf_copies_field` (default off, in PLAN's list of settings to
  measure). Run with that setting, this test agrees with the reference (seed
  77, 20,000 single instructions: 47 `mcrf` cases differ with it off, none
  with it on, which is evidence for that audit); the
  default was not changed here because the instruction tests' matrix
  compares runs with and without it and its lists would need redoing.
- **`stfs`, `stfsu`, `stfsx`, `stfsux`** of a double that is not exactly a
  single: the host conversion rounds (and saturates), the console takes the
  bits (truncates the mantissa). Compiled code rounds with `frsp` first, so
  this needs hand-written code to show.

- **`fmadds`, `fmsubs`, `fnmadds`, `fnmsubs`** are computed as a double
  fused multiply-add and the result rounded to single, which rounds twice.
  For add, subtract, multiply and divide that is exact; for a fused
  multiply-add it is one unit of a single off in rare cases (two sequences in
  a million cases here, both with operands that were not singles; it can
  happen with singles too). The host has a single-precision fused
  instruction, but it is only right when the three operands are singles,
  which the translator does not know: a fix costs a check on a common
  instruction and wants measuring.

Where the reference is not an authority (our result is the defensible one,
or neither is the console's):
- `fnmadd` and `fnmsub` (and the single forms, listed above for another
  reason) in the two directed rounding modes: QEMU rounds after negating; the architecture says the result is
  that of `fmadd` negated, which is what we do.
- `fres` of -0: QEMU returns -0.5; ours is -infinity.
- `vexptefp`, `vlogefp` (and `128`): QEMU's own approximations differ from
  ours at overflow and at exactly representable results.
- `vmsum3fp128`, `vmsum4fp128`: the back end returns a NaN when a term is
  infinite or the sum overflows, upstream's reading of the console; the
  reference here is plain arithmetic.

## 5. What the runs say

Before any fix, one seed (20,000 single instructions and 20,000 sequences)
had 216 groups of differing single instructions: most of them the unkept
state above (1,280 cases of VSCR[SAT], 656 of CR's SO bits), 626 traps from
the thirteen unimplemented instructions, and the bugs of section 3 among the
rest.

With everything of section 3 in, in the cloud session's container (the
runner under `qemu-aarch64`): seeds 1 to 9, 20 to 28 and 40 to 48, 1,080,000
cases, nothing differs that `known-issues.txt` does not list, and of the
540,000 sequences none differs at all. The CI run's own numbers are in the
run's report (the Actions page of the branch's head).

On GitHub's ARM64 runner (run 5 of the workflow, `e537fb5`): 4,960,000
single-instruction cases in the 30 minutes, nothing differing that is not
listed. That run had no sequences: QEMU 8.2 itself aborts on an ARM64 host
(`tcg/optimize.c`, `do_constant_folding_2`, "code should not be reached")
somewhere in every batch of them, which it does not on x86. `run.sh` now
runs the reference for sequences with one guest instruction a translation
block, which keeps QEMU's optimiser from seeing constants across
instructions; the results are byte for byte the same on x86.

Three mistakes of the rig itself were found on the way and are worth
knowing if it is changed: the reference harness's C code ran in 32-bit mode
until a case had returned (the machine starts that way), which made the
first case of a run differ; two random draws in one C expression were
ordered differently by the two compilers; and slots of a VMX128 case used by
plain VMX instructions must keep their register number.

## 6. Not covered

- Branches other than a forward `bc` over one instruction: no loops, no
  calls, no `bctr`, no `blr` but the last. The instruction tests'
  `seq_*.s` files cover calls and loops against fixed expectations.
- `lmw`, `lswi`, `lswx`, `stswi`, `stswx`, `dcbz`, `dcbz128` (its block is 32
  bytes on the console and 128 on the reference CPU), `mcrfs`, `mffs`,
  `mtfsb0`, `mtfsb1` (the last two not implemented in the front end either),
  the trap instructions, `sc`, the cache hints with no visible state.
- `vpkd3d128` and `vupkd3d128`: no reference written.
- A store conditional to another address than the one reserved (the
  instruction tests list `stwcx_address_mismatch` as known).
- An index register whose value is a 32-bit negative number zero-extended:
  the update forms would leave different upper halves in the base register
  on QEMU (it truncates the address before writing it back), which says
  nothing about the translator.
- Denormal inputs to float vector instructions, FPSCR's status bits, the
  float exception enables.
- The translator's optional settings (every case runs with the defaults).
- Anything outside the CPU core: no kernel, no threads (so nothing about
  memory ordering or the scheduler), no GPU.

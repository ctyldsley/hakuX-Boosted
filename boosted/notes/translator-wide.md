# The translator, wide: tests in CI, pinning for most functions, cheaper code

Branch `claude/translator-wide`, cut from `claude/integration-1113` at
`8e03766`. Written 2026-11-13 to 14 by a cloud session. It follows
`boosted/notes/cpu-translation.md` (the study) and
`boosted/notes/cpu-translator.md` (the first pinning).

**Nothing here has run on a device.** No speed-up is claimed anywhere in this
file: every "saves" is a count of instructions read off the emitter, not a
measurement. Every setting is off by default and the defaults leave
behaviour as it was.

What is new compared with the earlier notes: the translator can now be run
and checked without the phone. The instruction tests build for ARM64 Linux
and run under qemu, in GitHub Actions and in a cloud session's own
container. Everything below that says "tested" means those tests, with the
setting off and on.

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless
they start with another directory.

## Done / in progress / next

Done:
- Step 1, the tests (section 1). In the workflow as the job "Translator
  tests".
- Step 2, pinning for most functions (section 2): `pin_guest_registers_wide`,
  `pin_guest_registers_flags`, and why the first form pinned 17 functions.

In progress:
- Step 3 (dead flag stores, memory access, calls, branches, constants over
  blocks).

Next:
- Step 4 (translating ahead on a helper thread).

## 1. The tests

### What runs

`cpu/ppc/testing/ci/` holds a build of the test runner that compiles only
what the translator needs: `base/`, `cpu/` with the ARM64 back end,
`memory.cc`, and `stubs.cc` for the three `XexModule` members and the one
setting of other subsystems that those sources name. It links upstream's
runner source, `cpu/ppc/testing/ppc_testing_main.cc`. The core's own
`XENIA_BUILD_TESTS` build was not used: it links the GPU, audio, SDL and GTK
into the runner and would need the whole Linux desktop build of the core to
work first.

The code under test is the phone's: the same front end, passes, emitter and
code cache (the two code cache files are compiled with the fork's
`XE_PLATFORM_xendroid` path, as on Android). What differs from the phone is
listed under "What the tests do not cover".

The tests themselves:

| Tests | Count | What they are |
|---|---|---|
| upstream `instr_*.s` | 141,588 cases in 568 files run (28,713 more are in upstream's `skip.txt`) | one instruction each with given register inputs and expected outputs: integer, float, VMX and VMX128, condition register, load and store |
| upstream `seq_branch_carry.s`, `seq_jumptable_constants.s` | 3 | were in the tree and never run: the runner only looked for `instr_` files |
| `seq_cc_int.s`, `seq_cc_64.s`, `seq_cc_vec.s` | 123 | new. C compiled for PowerPC by gcc (`ci/gen/*.c`, 28 functions): loops, calls inside loops, recursion, indirect calls, jump tables, 64-bit arithmetic in halves and whole, float loops (a mixer among them), AltiVec loops. The expected values of the first two files come from the same C compiled for the host, so they do not depend on this translator at all |
| `seq_regs_across_calls.s` | 11 | new, by hand: registers live over calls where the callee changes them or reads them, conditional and indirect and tail calls, recursion, a return from inside a loop, float and vector registers over calls |
| `seq_flags_across_blocks.s` | 18 | new, by hand: condition register bits, the carry, CTR and LR set in one block and used in another or after a call |
| `seq_mcrf_fields.s` | 3 | new: see "Found" below |

Each run of the tests also writes, per test, a hash of everything the test
left behind: r0 to r31 and CTR, f0 to f31, v0 to v127, the condition
register bytes, XER, FPSCR, VSCR, the scratch memory and the top 32 KiB of
the stack (`--test_state_dump`, new in the runner). The runner now clears
that memory before each test so that the state depends on the test alone.

`ci/run-matrix.sh` runs everything once with all settings at their defaults
(the reference), a second time to prove the comparison is stable, and then
once for each line of `ci/matrix.txt`, which names a setting or a
combination. A line passes only if every test's own assertions pass, no
self-check line (`PinVerify:`, `SimplifySkipVerify:`, `TranslatorVerify:`)
appears in the output or the log, and every test's state hash equals the
reference's. So a setting is checked not only against the one or two
registers a test asserts but against every register and flag and the memory.

### How to run them

In CI: the job "Translator tests (PowerPC on the ARM64 back end)" of
`.github/workflows/boosted.yml`, on every push. It cross-compiles the runner
for ARM64 on the x86 runner and runs it under `qemu-aarch64`.

In a cloud session (this is new: the earlier notes say a session cannot
build or run anything; for the translator that is no longer true):

```bash
apt-get install -y --no-install-recommends qemu-user g++-aarch64-linux-gnu clang ninja-build ccache texinfo flex bison
XE=x360/emulator-core/src/main/cpp/xenia; CI=$XE/src/xenia/cpu/ppc/testing/ci
git submodule update --init --depth 1 -- $XE/third_party/{xbyak_aarch64,fmt,capstone,cxxopts,date,tomlplusplus,utfcpp,xxhash,disruptorplus,boost_context/context}
$CI/build-binutils.sh ~/ppc-binutils          # once, 3 minutes
$CI/gen-tests.sh ~/ppc-binutils/bin /tmp/ppc-test-bin
cmake -S $CI -B /tmp/ppc-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/$CI/aarch64-cross.cmake
ninja -C /tmp/ppc-tests                        # 10 minutes the first time
WRAPPER="qemu-aarch64 -L /usr/aarch64-linux-gnu" $CI/run-matrix.sh /tmp/ppc-tests/xenia-cpu-ppc-tests /tmp/ppc-test-bin
```

One pass over all tests takes about five minutes on four cores under qemu.
`SUITES='^seq_'` (a regular expression over file names) narrows a run of
`run-tests.sh`; `ONLY='pinning'` narrows the matrix to lines whose label
matches.

`ci/gen/gen-seq-tests.py` regenerates the `seq_cc_*.s` files from the C
sources (needs `gcc-powerpc-linux-gnu`); the generated files are checked in.

### What the tests do not cover

- **Anything on the phone.** qemu's ARM64 is not the phone's core: timing,
  FPCR behaviour that qemu models loosely (flush-to-zero of inputs), and
  16 KiB pages (the runner under qemu sees 4 KiB pages, so the address
  fix-up for guest addresses from 0xE0000000 on a 16 KiB host is never
  emitted in a test).
- **Kernel calls.** The tests load a raw module with no imports, so
  `CALL_EXTERN`, both extern thunks (`a64_lean_extern_thunk`) and
  `inline_extern_calls` are not exercised.
- **`longjmp`.** The back end follows a non-local jump through return-site
  records that only `XexModule` keeps; a test that made one crashed the
  runner on the unmodified translator. So the stackpoint path and pinning's
  resume stubs are not exercised.
- **The scheduler.** `--guest_scheduler=true` makes the translator emit the
  preemption checks, and the tests run through their usual path; nothing
  ever raises the flag, so the yield path is not run.
- **MMIO, self-modifying code, threads.** One guest thread, no devices.
- **Whole titles.** The largest function in the tests is a few hundred
  instructions. Translation of very large functions (far branches, register
  pressure over hundreds of blocks) is only as covered as `k_pressure`,
  `k_switch` and the like make it.
- 28,713 upstream cases are skipped by upstream's own `skip.txt` (mostly
  FPSCR and CR1 results of float instructions that the translator does not
  model). `ci/known-failures.txt` adds one more, see below.

### Found by the tests on the unmodified translator

- **`mcrf` is translated wrongly** (`InstrEmit_mcrf`,
  `cpu/ppc/ppc_emit_control.cc`). It copies a condition register field. The
  translation stores zeros into the destination and then overwrites them
  with a compare of the source field against zero, so the destination gets
  "greater" whenever the source field is not empty and is not cr0 holding
  "less". Upstream's own tests expect that wrong answer (`mcrf_3` copies a
  field holding "equal" and expects "greater"). The exact translation is
  behind `ppc_mcrf_copies_field` (off, as every setting here); three tests
  in `seq_mcrf_fields.s` cover it, and upstream's four tests that expect the
  wrong value are skipped when it is on. Whether any title depends on it is
  not known; compilers emit `mcrf` rarely.
- `stwcx.` to an address other than the reserved one stores anyway with
  `a64_native_reserved_ops` (the default). Listed in `known-failures.txt`,
  not changed: titles pair the two on one address.
- All 115 C-derived cases with host-computed expected values pass on the
  unmodified translator, as do all 141,588 upstream cases bar the one above.

## 2. Pinning for most functions (`pin_guest_registers_wide`)

### Why the first form pinned 17 functions of 24,576

The compiled-C tests reproduce the device's number: with
`pin_guest_registers` alone, 2 of 204 functions are pinned (44 have loops).
The cost model was not the cause, and neither was the loop requirement. The
cause is one line of the front end: `PPCHIRBuilder::Emit` puts a
`CONTEXT_BARRIER` in front of **every PowerPC branch instruction** (`b`,
`bc`, `bclr`, `bcctr` are of type `kSync` in the opcode table). The barrier
is there for the per-block passes: a conditional branch does not end a HIR
block, so without it context promotion would carry a value, or drop a store,
across a branch in the middle of a block. The first pinning read the barrier
as "host code needs the context here" and wrote every pinned register back
and read every one again at each. A loop is at least one branch a trip, so in
any function with branches the cost of a pin was at least its benefit, and
the model (correctly, on those numbers) declined. The 17 functions were the
ones whose pinned registers were float or vector registers used many times
between two branches.

### What the wide form does

`GuestRegisterPinningPass::RunWide`
(`cpu/compiler/passes/guest_register_pinning_pass.cc`; the long comment
above it has the argument in full):

- The front end now marks the barriers it emits for a branch
  (`CONTEXT_BARRIER_FOR_BRANCH`, `hir/opcodes.h`). In the wide form these are
  not sync points. The pass follows the branch itself, and the call or
  return a branch instruction may turn into is a sync point in its own right
  (`CALL`, `CALL_INDIRECT`, `RETURN`). Barriers for `mfmsr`, `mtmsr` and `sc`
  stay sync points.
- For every candidate register the pass computes over the function's control
  flow, mid-block branches included, where it is **dirty** (the host register
  may differ from the context: stored to since the context was last current)
  and where it is **live** (its value may still be read, or written back).
  At each sync point the backend stores exactly the dirty registers and, if
  control returns, loads exactly the live ones; at entry it loads the ones
  live there. The first form stored every written register and loaded every
  register at every sync point.
- So a register held over a call costs at most what the per-block code
  already pays, and functions with calls and functions without loops
  qualify. A register is pinned when that at least halves its loads and
  stores (`benefit >= 2 * cost + 2`, weighted by loop depth as before).
- New candidates with `pin_guest_registers_flags`: the link register, and
  the condition register and carry bytes (8-bit values in an integer host
  register, kept zero-extended as a load from the context would give them).
  The count register was already a candidate.
- `pin_guest_registers_max_vec` may now go to 16 (was 12).
  `pin_guest_registers_max_int` stays at most 3: the pins come out of the
  seven registers the allocator has (x22 to x28), an instruction can need
  four of them, and the only other integer registers are the scratch
  registers sequences use freely (the study's 175-use audit). That audit is
  now testable but was not done here.

What it does not do: the load of a pinned register is still a `mov` from the
pin into the value's own register (the allocator does not know the pin could
serve as the value's home). So pinning removes memory operations, not
instructions: per access one `ldr`/`str [x20]` becomes one register move,
which the phone's core removes at rename, and adds one `ldr` or `str` per
register at the places counted below.

### On the test corpus (static counts and block counts, not speed)

`seq_cc_*`, `seq_regs_*`, `seq_flags_*` (204 functions, most of them
single-block test wrappers and leaves with nothing to pin):

| | `pin_guest_registers` | + `_wide` | + `_flags`, 3 integer, 16 vector |
|---|---|---|---|
| functions pinned (of 204; 44 have loops) | 2 | 41 | 44 |
| of those, without a loop | 0 | 10 | 12 |
| slots: integer / float / vector / flag | 0 / 2 / 0 / 0 | 65 / 26 / 12 / 0 | 86 / 26 / 14 / 16 |
| context loads + stores that became moves | 26 | 597 | 759 |
| loads + stores emitted to keep in step | 31 | 185 | 249 |
| blocks translated in pinned functions | 5.2% | 53.3% | 55.6% |
| blocks executed in pinned functions | 8.0% | 69.9% | 70.0% |

A title is not this corpus. The third `TranslationStats:` line now prints the
same figures on the device (see "What `log_translation_stats` prints").

### Correctness, and how it is checked

The argument is in the comment above `RunWide`. In short: liveness here is
"a read or a write-back is reachable without passing a store or a reload",
over a set of edges that contains every edge the code can take, and every
instruction counts as a possible write-back of what is dirty before it
(because any sequence may call a host helper, which writes back the dirty
registers). So whenever a pinned register is read or written back, the last
thing before it on the path taken was a store to it, a reload that was
emitted because it was live, or the entry load. Over-estimating either set
only adds a store or a load of a correct value.

The list of sync points is the first form's (`A64Emitter::ClassifyPinSync`),
less the branch barriers. Host helpers called from inside a sequence, the
preemption check's yield and the two bare host calls use the masks of the
instruction being emitted.

Checked by:
- All 141,886 tests with wide pinning on, with `_flags` and the largest
  register counts, each with and without `a64_pin_verify`: every test's
  assertions and every test's full state equal to the reference.
- `pin_guest_registers_stress` (a test setting): pins the most used
  registers of every function whether or not it pays. Without it the cost
  model keeps registers that are busy round calls out of pins, and the
  write-back and reload paths are hardly run. With it all 141,886 pass, again
  with and without verify, and with 1 integer pin and 4 vector pins.
- Three deliberate faults, to see that the tests notice: no reload after a
  sync point, no write-back before one, no load at entry. Without stress and
  without the fix below, all three went unnoticed. With stress, each one
  crashes or hangs the plain run, and `a64_pin_verify` reports the two it
  can see (392 and 558 mismatches; a missing write-back is invisible to
  verify by design, since verify keeps the context current).
- That exercise also showed that the self-check lines were never visible in
  the test runner (it does not set up the log), so a verify run that found
  mismatches still passed. The runner now reads the mismatch counters
  (`compiler::SelfCheckMismatches`) and fails on any.

Not checked: `longjmp` into a pinned function (the resume stubs now reload
the registers live after that call; the path cannot run in the tests), the
preemption yield with pins, kernel calls with pins.

## The settings

(Completed as the steps land. So far:)

| Setting | Default | What it changes | Expected effect | Risk | Tested by |
|---|---|---|---|---|---|
| `pin_guest_registers_wide` | false | With `pin_guest_registers`: the wide form of section 2 | Pins in most functions that have more than one block; fewer context loads and stores in loops and between calls | Medium. A wrong write-back or reload corrupts a guest register silently. Tested as section 2 describes; `a64_pin_verify` works with it | matrix lines "wide pinning", "wide stress" |
| `pin_guest_registers_flags` | false | With `_wide`: LR and the condition register and carry bytes may be pinned | A compare in one block and its branch in another stop going through memory | As above; an 8-bit pin takes one of the integer pins | "wide pinning, all", "wide stress" |
| `pin_guest_registers_max_vec` | 8 | now 0 to 16 with `_wide` | | | "wide pinning, all" |
| `pin_guest_registers_stress` | false | Test setting: pins regardless of benefit | None: slower code. Not for play | | |
| `ppc_mcrf_copies_field` | false | `mcrf` copies the four bits of the source field | Correctness only | Low: a title that relied on the wrong result would have to be broken on a console | `seq_mcrf_fields.s`, matrix line "mcrf, exact" |

## What `log_translation_stats` prints now

The third `TranslationStats:` line (pinning) ends with a new part:

```
...; wide: A pinned functions have no loop, B flag slots, sync loads C and
stores D emitted; blocks translated in pinned functions E of F (x%); blocks
executed in pinned functions G of H (y%)
```

"functions considered / with loops / pinned" before it are as they were. E of
F is static. G of H is counted by the translated code itself, in code
translated while `log_translation_stats` was on (four instructions at each
block head, no lock, so the threads lose some counts to each other: read it
as a share, not a total). "context loads and stores became register moves"
against "sync loads and stores emitted" is the memory traffic removed
against the traffic added, counted once per instruction, not per execution.

## Upstream files changed, and why

All marked `hakuX-Boosted:` at each change.

| File | Change |
|---|---|
| `.github/workflows/boosted.yml` | the job "Translator tests" |
| `cpu/ppc/testing/ppc_testing_main.cc` | `--test_state_dump`; memory cleared before each test; `seq_*.s` files are run as well as `instr_*.s` |
| `cpu/ppc/testing/ci/**` | new: the small build, the scripts, the matrix, the C sources of the generated tests |
| `cpu/ppc/testing/seq_*.s` (six new files) | new tests |
| `cpu/ppc/ppc_emit_control.cc` | `ppc_mcrf_copies_field` |
| `cpu/hir/opcodes.h`, `hir_builder.h`, `.cc` | `CONTEXT_BARRIER_FOR_BRANCH`; `ContextBarrier` takes flags; the per-instruction sync masks (`PinSyncState`) |
| `cpu/ppc/ppc_hir_builder.cc` | marks the barriers emitted for branch instructions |
| `cpu/compiler/passes/guest_register_pinning_pass.h`, `.cc` | `RunWide` and the three new settings |
| `cpu/backend/a64/a64_emitter.h`, `.cc` | write-back and reload take a mask; the masks of the current instruction; 8-bit pins; the block counters |
| `cpu/backend/a64/a64_sequences.cc` | `LOAD_CONTEXT` and `STORE_CONTEXT` for I8 ask for a pin |
| `cpu/compiler/translation_stats.h`, `compiler.cc` | the new counters, `FormatTranslationStats`, `SelfCheckMismatches` |

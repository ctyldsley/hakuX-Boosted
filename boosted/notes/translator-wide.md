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

In progress:
- Step 2, pinning for most functions.

Next:
- Step 3 (dead flag stores, memory access, calls, branches, constants over
  blocks), step 4 (translating ahead on a helper thread).

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

## The settings

(Completed as the steps land. So far:)

| Setting | Default | What it changes | Expected effect | Risk | Tested by |
|---|---|---|---|---|---|
| `ppc_mcrf_copies_field` | false | `mcrf` copies the four bits of the source field | Correctness only | Low: a title that relied on the wrong result would have to be broken on a console | `seq_mcrf_fields.s`, matrix line "mcrf, exact" |

## Upstream files changed, and why

All marked `hakuX-Boosted:` at each change.

| File | Change |
|---|---|
| `.github/workflows/boosted.yml` | the job "Translator tests" |
| `cpu/ppc/testing/ppc_testing_main.cc` | `--test_state_dump`; memory cleared before each test; `seq_*.s` files are run as well as `instr_*.s` |
| `cpu/ppc/testing/ci/**` | new: the small build, the scripts, the matrix, the C sources of the generated tests |
| `cpu/ppc/testing/seq_*.s` (six new files) | new tests |
| `cpu/ppc/ppc_emit_control.cc` | `ppc_mcrf_copies_field` |

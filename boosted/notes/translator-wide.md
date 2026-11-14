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

Done, all behind settings that default to off, all tested as section 1
describes:
- Step 1, the tests (section 1). The job "Translator tests" of the workflow.
- Step 2, pinning for most functions (section 2):
  `pin_guest_registers_wide`, `pin_guest_registers_flags`, and why the first
  form pinned 17 functions.
- Step 3, four of its five items (section 3): dead flag and register stores
  across blocks (`eliminate_dead_context_stores`), compares fused with their
  branch and short forward branches (`a64_fuse_compare_branch`,
  `a64_short_forward_branches`), linked call sites (`a64_linked_calls`), the
  address fix-up left out of stack accesses on 16 KiB hosts
  (`a64_stack_access_no_fixup`).
- Step 4, translating ahead on a helper thread (section 4):
  `translate_ahead`.
- A translation bug of upstream's found by the tests (`mcrf`), fixed behind
  `ppc_mcrf_copies_field`.

Not done (section 6 says why and how to go on):
- Constant propagation across blocks (the fifth item of step 3).
- More than three integer pins.
- One open question from the tests: with every setting on at once and
  16 KiB pages, the test run aborts inside glibc's allocator under qemu
  (section 1, "Open").

Next, in order:
1. The device runs of section 5, one setting at a time, each with its
   verify run first where it has one.
2. If `pin_guest_registers_wide` holds up: the allocator change that lets a
   loaded value live in its pin (section 6), which turns the saved memory
   operations into saved instructions.
3. Constant propagation across blocks.

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
| `seq_flags_across_blocks.s` | 19 | new, by hand: condition register bits, the carry, CTR and LR set in one block and used in another or after a call |
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

### 16 KiB pages

qemu can give the program 16 KiB pages (`qemu-aarch64 -p 16384`), the
phone's page size. The translator then emits the address fix-up for guest
addresses from 0xE0000000 at every computed load and store, as it does on
the phone and does not on a 4 KiB host. Lines of the matrix that end in
`#16k` run that way. With every setting at its default, all 141,886 tests
pass at 16 KiB and leave the same state as at 4 KiB.

### Open: one abort at 16 KiB with everything on

With every setting on together and 16 KiB pages, one of the four shards
aborts part-way through, always at the same place, inside glibc's allocator
(`sysmalloc: assertion failed`, the check on the heap top's page alignment).
What is known:
- the same settings at 4 KiB pages: all 141,886 pass;
- at 16 KiB, the defaults, and each of four groups of the settings on its
  own (pinning; dead stores with fusion and short branches; linked calls;
  the rest): all pass on that shard;
- at 16 KiB with everything on and glibc told to keep allocations of 8 KiB
  and more off the `brk` heap (`GLIBC_TUNABLES=glibc.malloc.mmap_threshold=8192`):
  all 141,886 pass with equal states. That is how the matrix line
  "everything, 16k" runs (`#16k-mmap`).

So it follows the amount of heap the run uses at a page size that is not
the host's, which is what a fault in qemu's emulation of `brk` would look
like, and it does not follow any one setting. It was not proved to be
qemu's. A heap overrun in the translator that only shows with that layout
is the other possibility, and the way to rule it out is a run of the tests
built with AddressSanitizer, which this session did not set up. Until then,
"everything on" has been checked at 4 KiB without reservation and at 16 KiB
only with that allocator setting.

### What the tests do not cover

- **Anything on the phone.** qemu's ARM64 is not the phone's core: timing,
  and FPCR behaviour that qemu models loosely (flush-to-zero of inputs).
- **Kernel calls.** The tests load a raw module with no imports, so
  `CALL_EXTERN`, both extern thunks (`a64_lean_extern_thunk`) and
  `inline_extern_calls` are not exercised.
- **`longjmp`.** The back end follows a non-local jump through return-site
  records that only `XexModule` keeps; a test that made one crashed the
  runner on the unmodified translator. So the stackpoint path and pinning's
  resume stubs are not exercised.
- **Module unloading and shutdown under load** for the helper thread: the
  tests make and destroy a processor per suite (so the helper is started,
  used and joined 576 times a run), but nothing unloads a module.
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

And two faults in this branch's own work that only the tests showed, both
fixed:
- The emitter could not emit a function twice: laying out the stack slots
  of spilled values turns each into a constant and loses its type, so a
  second pass sized them all as 32-bit and overlapped them. Short forward
  branches need a second pass for a function that proves too large; the
  test setting that forces one on every function failed 7 tests until the
  layout was made once per function.
- The first cut of level 3 of the dead store pass treated every tail call
  as a return. The translator's functions are not the guest's: the scanner
  ends a function at an unconditional branch and the branch becomes a tail
  call into the rest, which reads the flags like any other code after a
  branch. Six tests failed until tail calls counted as reading everything.

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

## 3. Cheaper code, each behind its own setting

"Saves" is read off the emitter for one occurrence. How often each occurs
in a title is what the device's `TranslationStats:` line says; the counts
given here are from the test corpus (the `seq_` files) and say only that
the code is reached.

### 3.1 Dead stores across blocks (`eliminate_dead_context_stores`)

`cpu/compiler/passes/dead_context_store_pass.cc`, run before the last
simplification and dead code passes. `ContextPromotionPass` drops a store
only when the same block overwrites it; this pass does it over the
function: a store goes when, on every path from it, the slot is stored
again before a load of it or a point where other code may look at the
context.

| Level | Slots | What counts as reading everything |
|---|---|---|
| 1 | the 32 condition register bytes, XER carry, overflow and summary bytes | every call, return, trap, scheduler check, non-branch barrier, the end of the function |
| 2 | also r0, r3 to r12, r14 to r31, CTR | the same |
| 3 | as 2 | the same, except that a call that sets LR, a kernel call, and a return (`blr`: a jump to LR) do not read cr0, cr1, cr5, cr6, cr7, which the ABI leaves undefined there. A tail call is not a return (see "Found", above) |

Saves per dead flag bit: the `cset` and the `strb` (2 instructions), and
the `cmp` as well when nothing else uses that compare (3). A `cmpw` whose
branch reads one bit costs 9 instructions today; with the other two bits
dead it costs 3. Per dead register store at level 2: one `str`.

On the corpus: level 1 removes 95 of 503 candidate flag stores, level 2
165 of 1,395 stores, level 3 441 of 1,941 (condition register stores left
in the code fall from 490 to 94).

Level 3 is the one that gets most of the study's R2, and it is wrong for a
hand-written routine that returns an answer in cr0: the test
`cr_returned_by_callee_with_branch` is exactly that, fails at level 3 by
design, and is skipped there. `dead_context_store_verify` is how to find
out whether a title has such routines: nothing is removed, a store the pass
took for dead stores a marker value instead (0xA5 in a flag byte, which
otherwise holds 0 or 1), and every load of a flag byte or register checks
for the marker and logs `TranslatorVerify:`. On that test it reports 1
mismatch. A function translated in this mode checks loads of what another
function left behind too, so a cross-function read is caught on either
side.

### 3.2 Compare and branch (`a64_fuse_compare_branch`)

An integer compare whose only use is the conditional branch that follows
(with only context stores, spills, source offsets and the branch's own
barrier between) is emitted as `cmp` + `b.cond`. Today it is `cmp`, `cset`
into a register, and `cbnz` on the register. Saves one instruction and one
host register per fused pair. It only applies when the store of the bit is
gone, so it is the second half of 3.1: on the corpus 28 compares fuse
without it and 77 with level 3.

### 3.3 Short forward branches (`a64_short_forward_branches`)

Every conditional branch is emitted as an inverted branch over an
unconditional one, so that any distance encodes. `a64_short_backward_branches`
(the study's) uses one instruction when the target is already placed and
near. This does the same for a target that comes later in the function,
whose distance is not yet known: the function is emitted with short forms,
and if its code turns out longer than a short branch is certain to span
(32 KiB where a bit test was used, 1 MiB otherwise) it is emitted again
from the same HIR with long forms. Saves one instruction per conditional
branch, and a taken branch whenever the guest's branch is not taken.
`a64_short_forward_branches_test_retry` (tests only) forces the second
emission on every function.

### 3.4 Linked call sites (`a64_linked_calls`)

A call to a function that had no code when its caller was translated (the
usual case: callers are translated first) looks the callee up in the
indirection table on every call, for ever. On Android the table is in its
encoded form, and that is 13 instructions with two taken branches before
the call. With the setting on, the call site loads the target from an
8-byte literal in its own function's tail code: `mov w16, #guest`
(two instructions), `ldr x9, <literal>`. The literal starts as the resolve
thunk's address and the code cache points it at the callee when the callee
is placed (`A64CodeCache::LinkCallSites`, from `OnCodePlaced`): one aligned
8-byte store to data, no instruction is patched and no cache is flushed. A
call that reads the literal before that goes through the thunk, as an
unlinked call to an untranslated function does today. Saves 10
instructions, two taken branches and trades a table load for a literal
load, per call. On the corpus all 33 linked call sites end up pointing at
their callee.

The setting must be on from launch: a function placed while it is off is
not known to the linker, and call sites linked to it later stay on the
thunk (slow, not wrong).

### 3.5 Stack accesses on 16 KiB hosts (`a64_stack_access_no_fixup`)

On the phone every load and store with a computed address carries four
instructions (`mov w17, #0xE0000000; cmp; cset; add`) that move addresses
from 0xE0000000 up by a page. An address formed from the guest stack
pointer (r1 loaded from the context, plus or minus constants under 1 MiB)
cannot need it if the stack is where the kernel puts stacks. With the
setting on those four instructions are left out for such addresses. Saves
4 instructions per stack access; nothing on a 4 KiB host. On the corpus,
which is heavy on stack arrays, 578 of 654 computed accesses.

The assumption fails for a title that runs on a stack it placed itself in
the 0xE0000000 range. `a64_stack_access_verify` keeps the fix-up and counts
every stack-based address that needed it (the `TranslationStats:` line,
"stack accesses at 0xE0000000+"; no log line, the count is made by the
translated code itself). It must read 0.

What was not done under "memory access": nothing was found to fold between
address translation and byte swap on ARM64 (there is no load that swaps;
`ldr` + `rev` is already the minimum), and the fix-up itself cannot be
made shorter without a host register to hold 0xE0000000, and there is none
free (section 6).

## 4. Translating ahead (`translate_ahead`)

`Processor::QueueTranslateAhead`, `TranslateAheadMain` (`cpu/processor.cc`,
`processor.h`), called from `PPCTranslator::Translate` once a function has
code.

- When a function of the title's executable has been translated, each
  function it calls directly (`CALL`, `CALL_TRUE`) in the same module that
  has no code yet is queued. One helper thread takes them in order and
  calls `Processor::ResolveFunction`, as a guest thread would.
- One level ahead: what the helper translates queues nothing.
- The queue holds `translate_ahead_queue` addresses (1,024); when it is
  full the oldest is dropped.
- The helper runs at nice `translate_ahead_nice` (5). A guest thread that
  needs the function the helper is in the middle of waits in
  `EntryTable::GetOrCreate` as it would for another guest thread, and first
  raises the helper to its own priority (`Processor::BoostTranslateAhead`,
  `setpriority` on the helper's thread id); the helper goes back down when
  that function is done. On the tests, where the caller arrives at once,
  this happens for most of the queued functions.
- A failed resolve marks an address as failed for good, so the helper only
  tries an address at which `LookupFunction` already finds an ordinary guest
  function.
- Shutdown: `Processor::~Processor` stops and joins the helper as its first
  statement, before the modules are freed. The helper finishes the function
  it is on first (other threads may be waiting on its entry).
- Only the executable: a library module can be unloaded
  (`Processor::RemoveModule`), nothing there waits for the helper, and
  making it wait safely needs to know whether the caller holds the global
  lock. `translate_ahead_any_module` lifts the restriction for the tests,
  whose module is not an executable.

What it does to translated code: nothing. What it risks: a function
translated before the title has written its final code there would be
translated from the wrong bytes; a guest thread would have translated it
later, at the call. Direct calls within a loaded executable do not behave
like that, which is why only those are followed, but it is the reason this
is off by default. It also translates functions that are never called;
their code stays in the cache.

The third `TranslationStats:` line says how it went: queued, dropped,
already done, not attempted, translated by the helper, priority boosts, and
the number of functions translated on the thread that needed them.

## 5. The settings, in the order to try them on the device

One at a time, against a run of the same build with everything off, with
`log_translation_stats = true` in every run. All take effect for functions
translated after they are set: set them in the config before launch.
"Verify first" means one run through the usual scenes in the setting's
self-check mode before a run without it.

| # | Setting(s) | What changes | Expected effect (from the emitted code, not measured) | Risk | Verify first |
|---|---|---|---|---|---|
| 0 | (none), `log_translation_stats` | nothing but the counters; four instructions at each block head for the block counts | none; the baseline | none | |
| 1 | `a64_short_backward_branches`, `a64_short_forward_branches` | conditional branches within a function in one instruction | 1 instruction per conditional branch, one taken branch fewer where the guest's is not taken | low: the second emission of large functions is the new part, tested on every function | |
| 2 | `a64_linked_calls` | call sites load their target from a literal the cache fills in | 10 instructions and 2 taken branches per call to a function translated after its caller | low to medium: a literal is patched while other threads may read it (one aligned store) | |
| 3 | `a64_stack_access_no_fixup` | no 0xE0000000 fix-up on stack accesses | 4 instructions per stack access on this phone | medium, and per title: wrong if a title's stack is at 0xE0000000 or above | `a64_stack_access_verify`: "stack accesses at 0xE0000000+" must be 0 |
| 4 | `eliminate_dead_context_stores = 1`, then `2` | flag stores, then register stores, that every path overwrites | 2 to 3 instructions per dead flag bit, 1 per dead register store | medium: a store wrongly taken for dead leaves a stale flag or register | `dead_context_store_verify`: no `TranslatorVerify:` line |
| 5 | `a64_fuse_compare_branch` | `cmp` + `b.cond` | 1 instruction and a register per fused pair; little without 4, most with 8 | low | |
| 6 | `pin_guest_registers` + `pin_guest_registers_wide` | section 2 | context loads and stores become register moves in most multi-block functions | medium to high: the largest change to what registers hold | `a64_pin_verify`: no `PinVerify:` line, "verify mismatches" 0 |
| 6b | + `pin_guest_registers_flags`, `pin_guest_registers_max_int = 3`, `_max_vec = 16` | more pins: LR, flag bytes, a third integer, more vectors | more of the same; three integer pins leave the allocator four registers and may cost spills | as 6 | as 6 |
| 7 | `translate_ahead` | section 4 | translation moves off the title's threads where the helper is first: watch first-run hitches and "functions translated on the thread that needed them" | medium: timing, an extra thread's heat, more code in the cache | |
| 8 | `eliminate_dead_context_stores = 3` | flags the ABI leaves undefined are dead at calls and returns | most of what a compare costs today: 6 of its 9 instructions, 7 with 5 | high, per title: wrong for hand-written code that returns a flag | `dead_context_store_verify` at level 3 on that title: no `TranslatorVerify:` line |
| 9 | `ppc_mcrf_copies_field` | `mcrf` copies the field | correctness only | low | |

The earlier branches' settings (`simplification_skip_unchanged_blocks`,
`a64_thunk_fpcr_compare`, `a64_compact_stackpoint_push`,
`a64_lean_extern_thunk`, `inline_extern_calls`) are unchanged and combine
with these; the first three are in the matrix, the last two cannot be
tested here (no kernel calls).

Not for play: `pin_guest_registers_stress`,
`a64_short_forward_branches_test_retry`, `translate_ahead_any_module`. They
exist so that the tests reach code a sensible run seldom does.

Report for each: the three `TranslationStats:` lines after five minutes,
fps from `device-watch.py`, and for 2, 6 and 8 a simpleperf profile
(translated code's share, `sub_82C62548`, the two guest-to-guest call
paths).

## 6. Not done, and how to go on

- **Constant propagation across blocks.** The design: a forward data-flow
  over the same edges as section 2 that carries, per context slot, "holds
  this constant" from each block's end to its successors (the meet is
  "same constant on every edge"; anything volatile clears all), then
  replaces a `LOAD_CONTEXT` of such a slot with the constant before the
  simplification group runs. Its self-check would keep the load and compare
  it with the constant. It was left for last because it changes values the
  optimiser then folds further, so its testing is the widest of the five,
  and the session's time went to the items above and to chasing the faults
  the tests found.
- **More than three integer pins.** Pins come out of the allocator's seven
  registers, x22 to x28. An instruction can need four. The other integer
  registers are scratch in the sequences (the study's 175 uses of x9 to
  x15). Moving the allocator's values to those and pinning in the
  callee-saved ones is the study's R1 in full; the audit it needs can now be
  checked by the tests but was not begun.
- **A loaded value living in its pin.** A load of a pinned register is a
  `mov` from the pin to the value's own register. If the allocator gave the
  value the pin's register for as long as no store to the pin intervenes,
  the move would go and pinning would save instructions, not only memory
  operations. It is an allocator change (`RegisterAllocationPass`), per
  block, and the tests would cover it.
- **The scheduler's yield through the lean thunk, scalar NaN handling as a
  setting, a translation cache on disc**: the study's R4.3, R8 and R6,
  untouched.
- **The abort at 16 KiB with everything on** (section 1, "Open"): an
  AddressSanitizer build of the test runner would settle it.
- **A native ARM64 runner.** The job runs under qemu on the x86 runner
  because whether this repository can use GitHub's `ubuntu-24.04-arm`
  runners was not known, and a job waiting for a runner that never comes
  is never green. On one, `WRAPPER` is left unset and the `#16k` lines skip
  themselves.

## What `log_translation_stats` prints now

Three lines every 2,048 functions, as before. The third has grown:

```
TranslationStats: pinning: A functions considered, B with loops, C pinned;
slots gpr D fpr E vr F; context loads G and stores H became register moves;
I sync sites, J resume stubs, K resume entries, L verify mismatches;
pinned gprs: r3 n ... ctr n lr n;
wide: M pinned functions have no loop, N flag slots, sync loads O and
stores P emitted; blocks translated in pinned functions Q of R (x%);
blocks executed in pinned functions S of T (y%);
simplification: ... ;
translate ahead: queued, dropped, already done, not attempted, translated
by the helper, priority boosts; functions translated on the thread that
needed them;
dead context stores: removed of candidates; compares fused with their
branch; functions emitted twice for short forward branches or linked
calls; linked call sites, of which point at their callee; address fix-ups
emitted and left out for stack accesses; stack accesses at 0xE0000000+;
self-check mismatches (TranslatorVerify)
```

C of A is how many functions are pinned; y% is the share of executed
blocks that ran in a pinned function. S and T are counted by the translated
code itself, in code translated while `log_translation_stats` was on (four
instructions at each block head, no lock, so the threads lose some counts
to each other: read it as a share, not a total). G + H against O + P is
the memory traffic removed against the traffic added, counted once per
instruction, not per execution. Everything else on the line is static too,
except K, L, the priority boosts, the stack access count and the
self-check mismatches.

## Upstream files changed, and why

All marked `hakuX-Boosted:` at each change.

| File | Change |
|---|---|
| `.github/workflows/boosted.yml` | the job "Translator tests" |
| `cpu/ppc/testing/ppc_testing_main.cc` | `--test_state_dump`; memory cleared before each test; `seq_*.s` files are run as well as `instr_*.s`; the log is started when `--log_file` is given; the self-check counters are printed and fail the run; the `TranslationStats:` lines are printed with `--log_translation_stats` |
| `cpu/ppc/testing/ci/**` | new: the small build, the scripts, the matrix, the C sources of the generated tests |
| `cpu/ppc/testing/seq_*.s` (six new files) | new tests |
| `cpu/ppc/ppc_emit_control.cc` | `ppc_mcrf_copies_field` |
| `cpu/hir/opcodes.h`, `hir_builder.h`, `.cc` | `CONTEXT_BARRIER_FOR_BRANCH`; `ContextBarrier` takes flags; the per-instruction sync masks (`PinSyncState`) |
| `cpu/ppc/ppc_hir_builder.cc` | marks the barriers emitted for branch instructions |
| `cpu/compiler/passes/guest_register_pinning_pass.h`, `.cc` | `RunWide` and the three new settings |
| `cpu/backend/a64/a64_emitter.h`, `.cc` | write-back and reload take a mask; the masks of the current instruction; 8-bit pins; the block counters |
| `cpu/backend/a64/a64_sequences.cc` | `LOAD_CONTEXT` and `STORE_CONTEXT` for I8 ask for a pin |
| `cpu/compiler/translation_stats.h`, `compiler.cc` | the new counters, `FormatTranslationStats`, `SelfCheckMismatches` |
| `cpu/compiler/passes/dead_context_store_pass.h`, `.cc` | new: section 3.1 |
| `cpu/compiler/compiler_passes.h`, `cpu/ppc/ppc_translator.cc` | the new pass in the pipeline; the translate-ahead queueing after a function is assembled |
| `cpu/backend/a64/a64_emitter.h`, `.cc` (further) | `EmitCompareResult`/`TakeFusedCompare`/`FindFusableBranch`; `UseShortBranch` and the two-attempt loop in `Emit`; the locals laid out once; linked call literals; `EmitDeadStoreVerify`, `EmitStackFixupVerify` |
| `cpu/backend/a64/a64_sequences.cc`, `a64_seq_control.cc` | integer compares end in `EmitCompareResult`; `BRANCH_TRUE`/`BRANCH_FALSE` on I8 ask for a fused compare; the dead-store check after I8 and I64 context loads |
| `cpu/backend/a64/a64_seq_util.h` | `IsGuestStackPointerValue`; the fix-up left out or counted in `ComputeMemoryAddress` |
| `cpu/backend/a64/a64_code_cache.h`, `.cc`, `a64_function.h` | `LinkCallSites` and the literal list a function carries to it |
| `cpu/processor.h`, `.cc`, `cpu/entry_table.cc` | the translate-ahead helper, its stop in `~Processor`, the priority boost from the entry table's wait |

# jit-speed: faster code from the ARM64 translator, and a benchmark for it

Branch `claude/jit-speed`, cut from `claude/merge-1123` at `bd5d108`. Written 2026-11-23 and 24 by a cloud session.
Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless they start with another directory.

**Nothing here has run on a phone.** Every figure is a GitHub runner's or a count made under qemu-user in the
session's container, and says which. By `boosted/docs/PLAN.md` rule 2 none of it is a speed-up of a game until the
owner has measured it: "What to measure on the phone" below is the list.

## Done / next

Done:
- A translator benchmark that runs in GitHub Actions on `ubuntu-24.04-arm` (`.github/workflows/jit-bench.yml`),
  comparing every push with the branch's base. Section 1.
- The study of XenonRecomp and rexglue-sdk against our output for the same instructions. Section 2.
- Ten changes, one a commit, each with its numbers in the commit message. Section 3. Four are new code generation
  with no setting (they compute the same values); two are scheduler-check changes with no setting; four turn on, by
  default, exact settings that earlier sessions built and tested and left off.
- On the benchmark's twelve workloads the translated code runs 35% fewer host instructions than the base
  (geometric mean; 20% to 51% by workload) and is 15% smaller. Section 4.

Next, largest first: section 5.

## 1. The benchmark

`.github/workflows/jit-bench.yml`, on every push to `claude/jit-**` and on request (`workflow_dispatch`, with an
optional base). It builds the existing rig (`boosted/bench`, the core's CPU sources headless for ARM64 Linux) twice,
against the pushed commit and against the merge base with `claude/merge-1123`, runs both turn about for three
rounds and prints one table (also the job's summary, and `results/report.md` in the artifact).

What is measured (`boosted/bench/core/bench_jit.cc`, `bench_core --suite=jit`):

| Number | What it is |
|---|---|
| host bytes per PowerPC instruction | every function of an image translated once; machine code bytes over the guest instructions of those functions. At 4 KiB pages (the runner's) and at 16 KiB (the phone's: the same program under `qemu-aarch64 -p 16384`, where the translator emits the address fix-up for the `0xE0000000` view) |
| translation time | thread CPU time for that, per PowerPC instruction |
| run time | each workload's timed call, lowest of seven samples, three rounds |
| host instructions | the CPU's counter over the same call: exact, no noise, where the runner exposes it |

The images:
- **Workloads** (`boosted/bench/jit/workloads.c`): twelve C kernels written for the rig, compiled by
  `build-workloads.py` with `powerpc-linux-gnu-gcc -mcpu=cell -mpowerpc64 -maltivec` into a flat image at
  `0x82000000`, and with the host's compiler, whose results the translated code must return (a wrong result fails
  the job). Integer and table code, 64-bit integer, branchy code (sort, search), a bytecode interpreter (a switch
  through CTR), memory copies at mixed alignments, strings, calls, virtual calls and pointer chasing, double
  precision (an n-body step), single precision (matrices, transforms, clamps, conversions), a particle update, and
  AltiVec (written with intrinsics for PowerPC and element by element for the host).
- **The xsdk titles** (`xex2img.py`): eight `.xex` files from the newest green run of `xsdk-probes.yml` on
  `xsdk/probes` (xsdk pinned there at `9e6de98`): the SDK's whole test title, `m55_check` and the CPU probes. They
  are **translated whole, not run**: they need the kernel, files and threads, which the headless rig does not have,
  and the CPU probes are checks of single instructions, not workloads. They are what makes the code size figure one
  of real compiled programs (GCC 15, newlib, libstdc++; 2,293 functions in the test title). Their function list is
  the entry point and every `bl` target. If the artifact has expired the job says so and measures the workloads
  only; run "xsdk probes" on `xsdk/probes` again to bring it back.

What it cannot say: anything about a game's frame rate. The runner's cores are not the phone's, a title's code is
not these kernels, and translated code is one of several things a frame waits for.

In a cloud session's container (x86, no instruction counter): the rig cross-compiles and runs under qemu-user as
`boosted/notes/bench-harness.md` describes; `boosted/bench/jit/jit-count-qemu.sh <dir with bench_core>
<workloads.manifest> [name=value ...]` counts the host instructions translated code runs for each workload's short
call (qemu's execution log limited to the code cache), in about fifteen seconds. Those are the counts quoted below.
`BENCH_JIT_DUMP=<dir>` writes each function's machine code for a disassembler.

Faults of the benchmark found by its first run, fixed: one workload was not repeatable (state left by the call
before), and the translator ends on a fault for a few "functions" of the two largest xsdk images that are data (a
fault records the address in `<manifest>.skip`, the image is run again without it, both sides share the file).
The rig itself no longer linked at `claude/merge-1123` (the stall watchdog's hooks): fixed in `kernel_shim.cc`.

## 2. How the static recompilers lower PowerPC, and where ours differed

Read from the sources (XenonRecomp `XenonRecomp/recompiler.cpp`, `XenonUtils/ppc_context.h`; rexglue-sdk
`src/codegen/builder/*.cpp`, `resources/templates/codegen/pch_h.inja`). Both emit C++, one function per guest
function, and leave the rest to the C compiler. What they emit:

- **Registers.** Fields of a context, or C++ locals with the config switches (`NON_VOLATILE_AS_LOCAL`,
  `NON_ARGUMENT_AS_LOCAL`, CR, CTR and XER as locals): then the compiler keeps them in host registers across the
  whole function and nothing is stored on a path that does not need it.
- **Flags.** One byte per CR bit; a compare is `cr.compare<T>(a, b, xer)` and a branch `if (cr6.eq) goto`. Neither
  eliminates dead flags itself: with CR as locals the compiler does, and a local CR field does not survive a call or
  a return at all. XER's SO and OV are never computed.
- **Memory.** `bswap(*(base + (u32)address))`. No check on the usual path. MMIO is decided when the code is
  generated (XenonRecomp: only a store followed by `eieio`; rexglue: a per-instruction classification).
- **Calls.** A direct C call for `bl`, `return` for `blr`, a table of function pointers indexed by guest address
  for `bctrl`, a C `switch` for jump tables. No link register compare, no host/guest stack synchronisation, no
  indirection for a return.
- **Float.** `lfs` is `double(float)`, `stfs` is `float(double)`: one instruction each, a NaN's signalling bit is
  not kept. `fcmpu` is one compare. The flush-to-zero mode is switched only between scalar and vector code, tracked
  per function. `fmadd` is `a*b+c` in XenonRecomp (two roundings), `fma` in rexglue. No NaN fix-up after vector
  operations by default.

Ours, for the same instructions, at the base (`bd5d108`, defaults), from the benchmark's dumps:

| Instruction or shape | A recompiler | Ours at the base | Ours now |
|---|---|---|---|
| a register used in several blocks | in a host register throughout | loaded from and stored to the context in each block | pinned in a host register (default now) |
| `cmpwi` + `bne` | compare and branch | compare, three `cset`, three byte stores, a test, two branches | compare, `cset`s for the flags still read later, one branch |
| `lbzu; cmpwi; bne` (a strlen loop), per pass | 4 instructions | about 20, with two tests of the preemption flag | about 11, one test |
| `lfs` | 2 (load, convert) | 22: load, swap, convert and thirteen for the NaN bit | 7, the NaN case out of line |
| `stfs` | 2 | 18 | 7 |
| `fcmpu` + branch | compare and branch | 3 or 4 `fcmp`, masks, four byte stores | 1 or 2 `fcmp`, the flags still read later |
| `bl` to a known function | a call | a call, with a stackpoint record pushed in every prologue (17 instructions) and the return address handed on | the leaner record and linked call sites (default now) |
| `blr` | `ret` | compare with the caller's address, epilog; twenty more instructions inline for the other case | the same compare; the other case once a function |
| a loop head | nothing | load and test of the preemption flag, a dozen instructions out of line | load and test, a call out of line |
| conditional forward branch | one branch | `b.cond` over a `b` | one branch |

The gaps that are left are in section 5.

## 3. What was done, commit by commit

Counts are host instructions run by translated code for the twelve workloads' short calls, as a fraction of the
base's count (geometric mean), then host bytes per PowerPC instruction of the workloads image at 4 and 16 KiB pages.
Counted under qemu-user in the container; the runner's own table is in section 4.

| Commit | What | Setting | Count | Bytes |
|---|---|---|---|---|
| base `bd5d108` | | | 1.000 | 30.82, 35.64 |
| `9841f85` | `lfs`/`stfs` as one conversion, the NaN case out of line (`CONVERT_KEEP_NAN`) | none: same bits | 0.946 | 30.11, 34.93 |
| `8e98969` | no wait-backoff site in a loop that walks memory (`memory_poll_park_pass.cc`) | none | 0.945 | 30.09, 34.91 |
| `68fe051` | `fcmpu`/`fcmpo` without masks, one `fcmp` for the three results | none: same bits | 0.941 | 29.98, 34.82 |
| `9152763` | default on: `a64_tight_sequences`, `a64_fuse_compare_branch`, `a64_short_backward_branches`, `a64_short_forward_branches`, `ppc_rlw_narrow_rotate`, `register_allocation_elide_copies` | existing | 0.805 | 27.82, 32.66 |
| `5646f13` | default on: `a64_lean_calls`, `a64_linked_calls`, `a64_compact_stackpoint_push` | existing | 0.776 | 27.07, 31.92 |
| `23844ba` | default on: `a64_extended_gpr_pool`, `pin_guest_registers` with `_wide`, `_flags`, `_max_int = 10`, `_max_vec = 16`, `pin_guest_registers_coalesce` | existing | 0.675 | 29.10, 33.94 |
| `34a19e9` | default: `eliminate_dead_context_stores = 2`, `dead_context_stores_over_preempt_checks` | existing | 0.649 | 28.46, 33.30 |
| `8827125` | the out-of-line half of a preemption check as one thunk for all code | none | 0.649 | 27.07, 31.92 |
| `f695bca` | no preemption check at the entry of a function that calls nothing | none | 0.648 | 26.71, 31.56 |
| `c34e02f` | one copy a function of the return that does not go back to the caller | none | 0.648 | 26.21, 31.05 |

By workload at the head, against the base: integer 0.745, int64 0.800, branchy 0.592, interp 0.798, memory 0.703,
strings 0.649, calls 0.762, virtual 0.554, float 0.613, single 0.530, particles 0.486, vector 0.639.

On the four defaults commits: the settings are the work of the `translator-wide` and `translator-overhaul` sessions
(their notes say what each does and why it is exact). They were off because nothing had measured them. This
branch's rule for a setting that can change behaviour was "default on only when the benchmark and the existing
translator tests both pass", and they do: the whole corpus (141,978 tests) passes with the new defaults and leaves
every test in the state the old defaults left it in. `matrix.txt` has a new line, "before jit-speed defaults",
that runs everything with each of them put back, so the old code stays tested.

Left off on purpose, each measured on the benchmark with everything above on:
- `eliminate_dead_context_stores = 3` (the ABI level): 0.606 against 0.649. It is the nearest thing to a
  recompiler's local CR fields, and it assumes the ABI (no code reads cr0, cr1, cr5 to cr7 after a call or return
  without setting them). The guard of translator-overhaul.md section 1.14 is on with it. The next thing to try on
  the phone once this build is clean.
- `a64_scratch_gpr_pool`, `a64_shared_guest_registers = 4`, `a64_frame_chain` (start-up settings): 0.656 against
  0.705 when measured on the earlier settings alone. With shared registers the context copy of r1, r3, r30 and r31
  is stale outside host calls, and that has never run beside the park rework's poll sites, which hash registers.
- `a64_fpcr_known_block_entry`: no count to speak of (0.648); it removes `msr fpcr` writes, which is a time
  question the runner's table may show and the phone decides.
- `a64_lean_extern_thunk`, `a64_address_fixup_bit_tests`, `a64_stack_access_no_fixup`,
  `propagate_context_constants`, `simplification_skip_unchanged_blocks`, `a64_thunk_fpcr_compare`: not looked at.

### The phone's config file

The app's config store writes every setting into `xenia-canary.config.toml`, so the phone's file has
`a64_tight_sequences = false` and the rest as explicit values, and a stored value wins over a new default. For this
build to run as measured, take these lines out of the config (or set them as below) before the first launch:

```toml
[a64]
a64_tight_sequences = true
a64_fuse_compare_branch = true
a64_short_backward_branches = true
a64_short_forward_branches = true
a64_lean_calls = true
a64_linked_calls = true
a64_compact_stackpoint_push = true
a64_extended_gpr_pool = true

[CPU]
ppc_rlw_narrow_rotate = true
register_allocation_elide_copies = true
pin_guest_registers = true
pin_guest_registers_wide = true
pin_guest_registers_flags = true
pin_guest_registers_max_int = 10
pin_guest_registers_max_vec = 16
pin_guest_registers_coalesce = true
eliminate_dead_context_stores = 2
dead_context_stores_over_preempt_checks = true
```

(The sections are the ones the settings' `DEFINE_` lines name.) I did
not add `UPDATE_from_bool` entries to force the new defaults over a stored `false`: with the bundled file's old
`defaults_date` that would also stop the owner switching one off to bisect (`defaults-config.md`, "Flagged, not
changed").

## 4. The runner's table

jit-bench run 2 (id 45297587708, green), https://github.com/ctyldsley/hakuX-Boosted/actions/runs/45297587708:
`c34e02f` against the base `bd5d108`, built and run in the same job on `ubuntu-24.04-arm`, turn about, lowest of
three rounds. All eight xsdk titles from xsdk-probes run 45183497015.

**Code size and translation time, whole images**

Host bytes emitted per PowerPC instruction, over every function of the image; 16 KiB is the phone's page size (run under qemu, size only). Translation time is thread CPU time per PowerPC instruction.

| image | functions | PowerPC insns | bytes/insn, 4 KiB: base | head | change | bytes/insn, 16 KiB: base | head | change | translate us/insn: base | head | change |
|---|---|---|---|---|---|---|---|---|---|---|---|
| workloads | 29 | 2,148 | 30.8 | 26.2 | **-14.9%** | 35.7 | 31.1 | **-12.9%** | 2.89 | 3.75 | **+29.9%** |
| xsdk_m55_check | 1,344 | 246,857 | 23.9 | 17.3 | **-27.8%** | 29.3 | 22.6 | **-23.0%** | 1.97 | 2.61 | **+32.4%** |
| xsdk_probe_atomics | 363.0 | 57,875 | 23.7 | 17.3 | **-27.1%** | 28.9 | 22.3 | **-22.6%** | 2.11 | 2.86 | **+35.9%** |
| xsdk_probe_byterev | 369.0 | 58,723 | 23.7 | 17.3 | **-27.0%** | 28.8 | 22.3 | **-22.5%** | 2.08 | 2.85 | **+36.8%** |
| xsdk_probe_ctr | 359.0 | 57,598 | 23.7 | 17.2 | **-27.1%** | 28.8 | 22.3 | **-22.6%** | 2.11 | 2.85 | **+34.8%** |
| xsdk_probe_exec | 364.0 | 57,917 | 23.7 | 17.3 | **-27.1%** | 28.8 | 22.3 | **-22.6%** | 2.1 | 2.88 | **+37.4%** |
| xsdk_probe_spin | 1 | 13 | 57.5 | 38.5 | **-33.2%** | 58.2 | 38.8 | **-33.3%** | 13.3 | 15.2 | **+14.3%** |
| xsdk_probe_unroll | 359.0 | 57,991 | 23.7 | 17.3 | **-27.1%** | 28.8 | 22.3 | **-22.6%** | 2.1 | 2.9 | **+38.1%** |
| xsdk_test | 2,292 | 456,985 | 24.2 | 17.6 | **-27.2%** | 29.6 | 22.9 | **-22.6%** | 2.06 | 2.72 | **+32.3%** |
| **geometric mean** |  |  |  |  | **-26.6%** |  |  | **-22.9%** |  |  | **+32.2%** |

Functions the translator refused (data taken for code by the function list): xsdk_m55_check 5, xsdk_test 1.

**Run time, workloads**

Each workload's timed call: milliseconds (lowest of the samples) and host instructions retired (the CPU's counter; exact, where the runner has one). The result of every call is checked against the host C compiler's.

| workload | run ms: base | head | change | host insns, millions: base | head | change |
|---|---|---|---|---|---|---|
| integer | 74.63 | 72.07 | **-3.4%** | 1,131 | 860.3 | **-23.9%** |
| int64 | 59.44 | 55.3 | **-7.0%** | 660.5 | 528.4 | **-20.0%** |
| branchy | 39.05 | 27.68 | **-29.1%** | 449.0 | 265.9 | **-40.8%** |
| interp | 42.06 | 35.56 | **-15.5%** | 473.6 | 378.8 | **-20.0%** |
| memory | 29.24 | 14.29 | **-51.1%** | 361.3 | 206.0 | **-43.0%** |
| strings | 50.05 | 28.83 | **-42.4%** | 551.0 | 343.5 | **-37.6%** |
| calls | 33.66 | 31.37 | **-6.8%** | 460.6 | 348.8 | **-24.3%** |
| virtual | 57.83 | 40.16 | **-30.6%** | 487.3 | 268.1 | **-45.0%** |
| float | 42.61 | 36.56 | **-14.2%** | 452.6 | 278.6 | **-38.4%** |
| single | 9.645 | 6.557 | **-32.0%** | 93.86 | 49.63 | **-47.1%** |
| particles | 30.62 | 17.07 | **-44.3%** | 377.1 | 165.9 | **-56.0%** |
| vector | 7.171 | 5.313 | **-25.9%** | 65.62 | 37.87 | **-42.3%** |
| **geometric mean** |  |  | **-26.8%** |  |  | **-37.5%** |

How to read it:
- **Code size on real compiled programs is down 27% at 4 KiB pages and 23% at the phone's 16 KiB** (17.3 and 22.3
  bytes a PowerPC instruction on the xsdk titles, from 23.7 and 28.8). The base's 23.7 is close to what the phone
  measured for Gears of War (22.7), which says these images are a fair stand-in for a title's code where the
  workloads image (float and vector kernels, few functions) is denser.
- **Host instructions retired are down 37.5%, run time 26.8%** (geometric means; every workload's result checked).
  Time falls less than instructions where a loop's time is its memory accesses or its multiplies and divides
  (integer, int64, calls).
- **Translation takes 32% longer** (2.1 to 2.8 microseconds a PowerPC instruction): the pinning and dead-store
  passes. That is first-run cost, and a title translates its code as it first reaches it, so it is the one number
  here that got worse and it belongs with the phone's first-run stutter. Section 5, item 0.
- Six "functions" of the two largest images are data the function list took for code; the translator faults on
  them on both sides and they are left out of both.

## 5. What is next, largest first

From the dumps of the head's code (14,073 host instructions for the workloads' 2,140 PowerPC instructions at 4 KiB
pages, 6.6 each, where a recompiler's output is between 1.5 and 3):

0. **Translation time, up 32%.** `simplification_skip_unchanged_blocks` (cpu-translator.md section 2, exact, with
   a verify mode) was written for the translator's largest single cost and is still off; `translate_ahead` moves
   translation off the guest's threads. Measure both with this benchmark's translation column before the phone's
   first-run stutter is judged with this build.
1. **Write-backs and reloads of pinned registers round out-of-line paths: a quarter of the code.** 1,753 loads and
   1,681 stores of context slots, most of them in function tails: each preemption check's slow path writes the
   pinned registers back, calls and reads them again. The handler is the scheduler switching fibres; if nothing
   reads another thread's registers from its context while it is switched out (the poll sites read their own
   thread's; a suspended thread's context for `NtGetContextThread` is the case to check), the write-back can go,
   and each check's tail becomes one call. It needs that reading done first, and a setting.
2. **Flags computed in loops and used at the exit.** `mov w14, #0; cmp; cset; cset` on every pass of a loop whose
   cr0 is read only after it. The ABI level of the dead-store pass removes most of what is left (0.606); sinking a
   flag's computation to the exits that read it would remove it exactly.
3. **`lbz`/`lhz`/`lwz` into a pinned register take a load and a move or extension**, and `stwu`/`lbzu` compute the
   address twice (`add w0, w15, #1` for the access, `add x15, x15, #1` for the register). The overhaul's notes
   have the first (section 5); the second is a peephole in the update forms' front end.
4. **Scalar float still tests every result for a NaN** (`fcmp; b.vc`, 394 of them in the workloads' code: two
   instructions an operation) to give PowerPC's choice of NaN. A recompiler does not. As a per-title or global
   setting it is the largest thing left for float code; it changes results only when a NaN is produced.
5. **`msr fpcr` at block starts.** `a64_fpcr_known_block_entry` removes some; "no FPCR write in a function without
   vector float code" (translator-overhaul.md section 5) removes the rest. Costly on a real core, invisible in
   instruction counts.
6. **Calls.** Every prologue still pushes a stackpoint (for `longjmp`) and every return compares the link register.
   `a64_frame_chain` replaces the first with two stores; a recompiler has neither. The return's other case could
   be one thunk for all code rather than one copy a function (about ten instructions a function).
7. **Constants of the address space.** `mov x14, #imm48` (three instructions) before each indirection table
   look-up, thunk call and literal: 739 `movk` in the workloads' code. A register that holds the code cache's base,
   or a literal pool, takes two of the three away.
8. **Float constants loaded from read-only data** (`lis; lfs` from `.rdata`) could be folded when the page cannot
   be written; with `xex_code_writable` it can, so it needs the write watch that `icbi` handling has.
9. **The benchmark** does not run the xsdk titles (no kernel in the rig) and has no title's code. A whole
   executable translated on the phone (`aot_translate_measure`) remains the measure of code size that counts: at
   the base it said 22.7 bytes an instruction for Gears of War, against 30.8 for these workloads.

## What to measure on the phone

One build, the APK of this branch's head, with the config lines of section 3 set. Each run with
`log_translation_stats = true`.

1. Gears of War, the same spot as build 159 (three guest threads at 85 to 95% of a core, 18 to 24 fps):
   `perf.txt` (fps per GHz), the busy share of the three guest threads, and `aot_translate_measure` once for the
   code cache's size against 107 MB.
2. If anything misbehaves, in this order, one at a time: `pin_guest_registers = false`;
   `eliminate_dead_context_stores = 0`; `a64_lean_calls = false` with `a64_linked_calls = false`; then the six of
   the first defaults commit. The four changes without a setting are the float load and store, the float compare,
   the preemption thunk and the return's shared path; a fault that survives every setting is in one of those or in
   the two scheduler-check changes, and `git revert` of the single commit is the test.
3. The verify modes, where a run is to be trusted before play: `a64_pin_verify`,
   `pin_guest_registers_coalesce_verify`, `dead_context_store_verify` (no `PinVerify:` or `TranslatorVerify:`
   line in the log).
4. Then `eliminate_dead_context_stores = 3`, per title, with `dead_context_store_verify` first.

## Upstream files changed, and why

For `boosted/docs/ARCHITECTURE.md` (copied in with this branch):

| File | Change |
|---|---|
| `cpu/hir/opcodes.h`, `cpu/ppc/ppc_emit_memory.cc`, `cpu/backend/a64/a64_sequences.cc`, `cpu/compiler/passes/constant_propagation_pass.cc` | `CONVERT_KEEP_NAN`: `lfs`/`stfs` as one flagged conversion on ARM64, the NaN case out of line; not folded as a constant |
| `cpu/ppc/ppc_emit_fpu.cc`, `cpu/backend/a64/a64_emitter.cc`, `.h` | `fcmpu`/`fcmpo` store the three ordered compares unmasked on ARM64; the flags cache knows float compares (`MakeFlagsKey`, `A64Emitter::fcmp`) |
| `cpu/compiler/passes/memory_poll_park_pass.cc` | `TryInstrumentLoop` leaves a loop whose load address moves on every pass (a scan) without a backoff site |
| `cpu/compiler/passes/preempt_check_injection_pass.cc` | no check at the entry of a function without a call |
| `cpu/backend/a64/a64_backend.cc`, `.h`, `a64_emitter.cc` | `EmitPreemptYieldThunk`: the slow path of every preemption check in one thunk |
| `cpu/backend/a64/a64_emitter.cc`, `.h` | `CallIndirect`: one copy a function of a return that misses the caller's address (`return_miss_label_`) |
| `cpu/backend/a64/a64_emitter.cc`, `a64_backend.cc`, `a64_code_cache.cc`, `cpu/ppc/ppc_emit_alu.cc`, `cpu/compiler/passes/register_allocation_pass.cc`, `guest_register_pinning_pass.cc`, `dead_context_store_pass.cc` | defaults of eighteen existing settings (section 3) |
| `cpu/ppc/testing/seq_float_nan_moves.s`, `cpu/ppc/testing/ci/matrix.txt` | tests for the NaN moves; the "before jit-speed defaults" line |

## Build state

At `eb4014a`, the last commit that changes code (the commit after it changes documents and a test label):

- jit-bench run 6 (id 45298413350): code 26.6% smaller at 4 KiB pages and 22.9% at 16 KiB, translation 33.0% slower, run time 26.5% lower, host instructions 37.5% fewer.
- `boosted.yml` run 172 (id 45298413158): Xbox core, APK and "Translator tests" green. APK `app-debug.apk` 46,372,424 bytes, signed with the fork's key; artifact `hakuX-Boosted-debug-apk`, 36,194,278 bytes.
- Translator tests: 84 configurations, every one passed (the reference 141,978 of 141,978; the new line with the old defaults 141,978 passed and 141,957 states compared with the reference, 0 differ), 2 h 59 min against 2 h 56 min on `claude/merge-1123`.
- `boosted-bench.yml` run 51 shows cancelled though each step it ran passed; not looked into.
- Not installed or run on a device.
- Translator tests locally (cross-compiled, qemu-user, 4 KiB pages) gave the same: the whole corpus passes at the head and every test ends in the state the base's code and the old defaults leave it in.
- jit-bench run 1, at the first commit, failed on the benchmark's own two faults (section 1); run 2 at `c34e02f` is the table of section 4.

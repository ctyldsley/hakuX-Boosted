# The translator, overhauled: registers, calls, flags, memory

Branch `claude/translator-overhaul`, cut from `claude/integration-1115` at
`5aea2e2`. Written from 2026-11-14 by a cloud session. It follows
`boosted/notes/translator-wide.md` (the tests, wide pinning, the cheaper-code
settings) and `boosted/notes/cpu-translation.md` (the study).

**Nothing here has run on a device.** Every figure is either a count of host
instructions run per guest instruction on the bench loops (exact; the same
count on the workflow's ARM64 runner and under qemu in the session's
container, see "How it is measured") or says what else it is. No time is
quoted as a speed-up: runner times are runner times.

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless
they start with another directory.

## Done / in progress / next

Done (every setting off by default; the defaults' code is unchanged):
- Step 0: why the "Translator tests" job failed on `claude/integration-1115`
  (Actions run 135), and the fix (section 0). Green on this branch.
- Counting host instructions per guest instruction without a performance
  counter (`boosted/bench/tools/xlat-count-qemu.sh`, "How it is measured").
- Fourteen integer host registers (`a64_extended_gpr_pool`), up to ten
  integer pins (section 1.1).
- Values that live in their pins (`pin_guest_registers_coalesce`, with
  `pin_guest_registers_coalesce_verify`), section 1.2; copies that need no
  instruction (`register_allocation_elide_copies`), section 1.3; a fault in
  pinning's cost model that kept pins out of loops (section 1.4).
- Compare and branch on live flags, `cbz`/`cbnz` for tests against zero
  (in `a64_fuse_compare_branch`), section 1.5; flag stores in loops no
  longer kept alive by the scheduler's check
  (`dead_context_stores_over_preempt_checks`), section 1.6.
- Float and VMX float operations with one test on the usual path
  (`a64_tight_sequences`), section 1.7.
- Calls and returns (`a64_lean_calls`), section 1.8; the lean thunk for
  kernel calls now applies to import thunks (`a64_lean_extern_thunk`, which
  did nothing before), section 1.9.
- `rlwinm` and friends as a 32-bit rotate (`ppc_rlw_narrow_rotate`),
  section 1.10; the 16 KiB address fix-up as bit tests
  (`a64_address_fixup_bit_tests`), section 1.11.
- Tests for all of it (section 3), matrix lines, bench rows.

In progress: nothing half-written is on the branch.

Next, in order (section 5 has each in full):
1. The device runs of section 4.
2. FPCR written once per function region instead of once per block.
3. The stackpoint record and the check after every call (the two largest
   fixed costs left on a call), which need a `longjmp` test first.
4. A guard that makes the ABI level of the dead-flag pass safe by default.

## 0. The failed "Translator tests" job of run 135

It was neither an interaction between the merged branches nor a break of the
small test build. It was the allocator abort the earlier notes describe, and
it has a cause that can be named and removed.

What failed: one line of the matrix, the last, "everything, 16k". Two of its
127 processes aborted with `Fatal glibc error: malloc.c:2599 (sysmalloc):
assertion failed: ... ((unsigned long) old_end & (pagesize - 1)) == 0`; every
test that did run passed (138,919 of 141,899) and no state differed from the
reference. Every other line passed, "everything" at 4 KiB pages among them.
The same line passes in the session's container, 141,899 of 141,899, and the
failing process's exact list of suites passed 24 times of 24 there.

The cause is in how qemu-user starts a program when it is asked for a page
size that is not the host's (`-p 16384` on a 4 KiB host):
- The program break starts at the end of the executable's data, which is
  4 KiB aligned, not 16 KiB aligned (seen with `qemu -strace`: the test
  runner's first `brk(NULL)` returns an address ending `...ee000`).
- glibc's allocator copes with that only if its first `brk()` succeeds (it
  then pads the break to a page boundary itself). Under qemu the first
  `brk()` usually fails, because one of qemu's own mappings sits a few pages
  above the executable (seen from the host's `/proc/<pid>/maps`). The
  allocator then switches to `mmap` and from there on assumes that any later
  `brk()` returns page-aligned memory, as it would on a real kernel.
- When qemu frees that mapping of its own while the tests run, the next
  `brk()` succeeds from the unaligned break, the allocator's top chunk ends
  off a page boundary, and the next time the heap has to grow the assertion
  fires.

That explains everything the earlier session saw and could not: it happens
with every translator setting off, only at 16 KiB, at a point that depends on
the machine (where the host placed qemu's mappings and when qemu frees one),
shorter processes make it rarer but not impossible, glibc's heap checker
finds nothing (there is no overrun), and telling glibc to keep large blocks
off the `brk` heap did not help (the top chunk is still grown with `brk`).
The merge moved nothing but the timing.

What was seen and what was not. All of it was then seen in the session's
container, in the bench (which did not have the fix) run at 16 KiB pages
with qemu's execution log on: `qemu -strace` shows `brk(NULL)` returning an
address ending `...d1000`, the first `brk()` to grow failing, a later
`brk()` of the same size succeeding from the unaligned break, and the abort
at the next growth. With the fix below in the bench the same run shows the
break moved to a 16 KiB boundary, every later `brk()` refused, and the run
finishing. What was not seen is the abort on GitHub's runner with the fix in
place: Actions run 138 (the fix alone) is green, all lines.

The fix (`cpu/ppc/testing/ci/stubs.cc`, test build only): a constructor that
runs before anything else and, only when the break is not page aligned,
moves it up to a page boundary and maps one inaccessible page there. `brk()`
can then never grow, so the allocator stays on `mmap` for the whole run,
which is the state every passing run in the container was in. On a real
kernel (and at 4 KiB under qemu) the break is aligned and nothing is done.
The runner prints one line when it applies.

The matrix still runs the `#16k` lines in processes of at most 1,500 tests.
With the cause removed that should no longer be needed, and it costs time
(a process takes 14 s to start under qemu): the next thing to try is one
`#16k` line unsplit. Not done in the same push as the fix, so that a green
job says the fix did no harm before anything else changes.

## How it is measured

The figure of merit is host instructions run per guest instruction on the
bench's translator loops (`boosted/bench/core/bench_xlat.cc`) and the count
of host instructions the compiled-C test suites run. Both are exact counts,
not times.

- On the workflow's runner (`.github/workflows/boosted-bench.yml`) the bench
  reads the CPU's instruction counter. The rows `run-core.sh` adds for this
  branch are named "overhaul 1" to "overhaul 10": each adds to the one
  before it, in the order of section 4.
- In a cloud session's container there is no counter, so
  `boosted/bench/tools/xlat-count-qemu.sh <dir with bench_core> [name=value
  ...]` runs each loop under qemu-user with its execution log limited to the
  code cache's addresses, at two lengths, and divides the difference. With
  the defaults it gives the runner's figures to the digit (integer 5.523,
  float 7.778, vector 10.22, calls 3.963, copy 6.271). It counts translated
  code and the thunks only, so `host_calls` reads lower than on the runner
  (18.5 against 19.33): what the host function does is outside the range.
  `DUMP=<dir>` also writes each function's machine code and its
  disassembly. `QEMU="qemu-aarch64 -p 16384 -L /usr/aarch64-linux-gnu"`
  counts at the phone's page size, which is the only way to see the address
  fix-up of section 1.11.
- The test corpus: the same log over a run of the test runner on the three
  compiled-C suites (`seq_cc_int`, `seq_cc_64`, `seq_cc_vec`: loops, calls
  inside loops, recursion, jump tables, stack arrays, float and AltiVec
  kernels). The guest instructions run are the same whatever the settings,
  so the ratio of two counts is the ratio of host instructions per guest
  instruction. (The script for this is in section 2.)

What none of this says: time. Fewer instructions is the brief's measure and
the one that can be counted exactly without the phone; an instruction saved
is not always time saved (section 1.11 trades four instructions that never
branch for one to three that do), and only the phone can say.

## 1. What was built

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/`.

### 1.1 Fourteen integer host registers (`a64_extended_gpr_pool`)

The allocator had seven integer registers (x22 to x28) and pinned guest
registers came out of the same seven, at most three. The study's R1 asked
for the allocator's values to move to caller-saved registers, with "about
175 uses" of x9 to x15 as scratch to audit. The audit turned out small: the
sequences (`a64_sequences.cc`, `a64_seq_*.cc`, `a64_seq_util.h`) use x0 to
x8, x16 and x17 as scratch and name x9 to x15 in four places, all of them
the two host functions called with a bare `blr`. Everything else that names
them is in the emitter's call and prologue code and in the thunks, at
points where no value is live.

So x9 to x15 simply join the pool: fourteen registers that hold either a
HIR value or a pinned guest register, by function. What had to change
(all conditional on the setting, which is read once at start-up):
- `EmitGuestToHostThunk` saves and restores x9 to x15 (a C function does
  not keep them); call sites reach the thunk through x16, not x9
  (`A64Emitter::HostCallReg`).
- The two bare host calls (`RSQRT_F64`, `RSQRT_V128`) save x9 to x15 on the
  stack round the call (`EmitSaveExtendedGprsForBareCall`).
- `EmitGuestAndHostSynchronizeStackHelper` works in x1 to x5, and its call
  site uses x17: it returns into code that runs with pins loaded.
- `EmitPreemptCheck`'s diagnostic store uses w17.
- Guest calls need nothing: no HIR value is live across one (translated
  functions have always used the allocator's registers without saving
  them), and pins are written back before a call and read back after it.

`pin_guest_registers_max_int` may then be up to 10 (it stays 0 to 3
without the setting): the allocator always keeps four registers.

Checked by: the whole corpus with the setting on; with
`register_allocation_from_top` (new, tests only: the allocator hands out
the highest free register first, so that x9 to x15 hold values in every
test and not only in crowded blocks); and with ten stress pins.

### 1.2 Values that live in their pins (`pin_guest_registers_coalesce`)

Wide pinning turned each load of a pinned register into a move from the
pin and each store into a move to it, so it removed memory operations and
no instructions (bench run 13: 48 of 108 functions pinned, instruction
counts the same). `RegisterAllocationPass::PlanPinHomes`
(`compiler/passes/register_allocation_pass.cc`; the comment above it has
the rules and the argument) removes the moves, block by block:
- a value loaded from a pinned register *is* the pin's host register, when
  every use of it comes before that register is next written;
- a value about to be stored to a pinned register is computed straight
  into the pin's register by the instruction that defines it, when nothing
  between the two could notice that the pin changed early (only pure
  instructions between, none of them touching this pin), every value
  still living in the pin has been used for the last time, and the new
  value itself is not used past the next write of the pin.

`add r4, r4, r5` with all three pinned is then one `add`. Values planned
this way take none of the allocator's registers. The backend's side is
small: the context load and store sequences emit nothing when the value's
register is the pin (`a64_sequences.cc`), and an instruction computing
into a pin takes that pin out of the write-back and reload masks of any
host call made from inside its own sequence (`a64_emitter.cc`, in `Emit`'s
loop).

It needs `pin_guest_registers_wide` (it relies on the per-instruction
masks) and works with either pool.

`pin_guest_registers_coalesce_verify` is its verify mode: every value the
setting would keep in a pin gets a register of its own as well, the pin is
written exactly where the setting would write it, and each use first
compares the two (`A64Emitter::EmitPinHomeVerify`; `TranslatorVerify:` in
the log, counted with the other self-check mismatches). It checks the
allocator's reasoning about *when a pin still holds a value*, at run time,
on the title's own code.

Checked by: the whole corpus with stress pinning (every function's most
used registers pinned whether it pays or not) at ten pins, with and without
allocation from the top; the verify mode over the integer, float and
vector instruction tests and the sequence tests with stress pinning: no
mismatch; `seq_pin_coalesce.s` (section 3).

### 1.3 Copies that need no instruction (`register_allocation_elide_copies`)

The front end's HIR is full of copies that the backend emitted as moves:
- the truncation in front of every 32-bit compare, store and rotate, and
  the zero-extension behind every 32-bit load, rotate and byte swap
  (`mov w, w`);
- assignments left by context promotion;
- `VECTOR_DENORMFLUSH` in front of every VMX float operation, which is a
  plain copy on a core whose flush-to-zero mode covers inputs.

The allocator now gives the result the operand's register where that is
sound, and the sequences then emit nothing
(`RegisterAllocationPass::IsElidableCopy`):
- an operand used for the last time by the copy hands its register over;
- a copy of a value living in a pin lives in the pin on the same terms.

A truncation is only treated as a copy when every use of its result reads
32 bits and nothing else (the integer compares, the extensions, the 32-bit
rotate and byte swap, the value operand of a store): the register's upper
half is then not cleared, and those sequences were read to confirm they use
the 32-bit register. A zero-extension is only a copy when its operand was
made by an instruction whose sequence ends in a write of the 32-bit
register (a 32-bit load, byte swap, or rotate by other than a constant 0).
The backend is told which truncations and extensions were elided
(`HIRBuilder::elided_truncates`); for any other it emits the move as
before.

No verify mode of its own: with pins the verify mode of 1.2 covers the
copies of values in pins (comparing 32 bits for a truncation); without
pins a copy only ever takes a register its operand has finished with.

### 1.4 The cost model weighed a return as loop code

`GuestRegisterPinningPass::RunWide` weighs each load and store by loop
depth, per HIR block. A PowerPC conditional branch does not end a HIR
block, so the code after a loop's back-edge (its exit, often the
function's return) sat in the loop's last block and its write-backs were
costed as if they ran every time round. In the bench's float loop that
made seven of eight float registers look unprofitable. A loop now ends at
its back-edge instruction, not at the end of that instruction's block.
This changes which registers `pin_guest_registers_wide` picks (more of
them); nothing else.

### 1.5 Compare and branch on live flags (in `a64_fuse_compare_branch`)

The setting fused a compare with its branch only when the branch was the
compare's sole use, which needs the condition register stores gone first.
Three more forms, same setting:
- **A flags cache.** `cmpw` is three HIR compares of the same operands;
  each emitted its own `cmp`. The emitter now remembers which compare the
  host flags hold (`A64Emitter::FlagsKey`) and leaves out a `cmp` that
  would repeat it. The flags are forgotten at every instruction that is
  not on a short list of ones that write no flag in any mode, and at every
  block start.
- **A branch on the flags** of a compare whose result has other uses (the
  store of the condition register bit): `b.cond` instead of testing the
  result's register, while the cache says the flags are still that
  compare's.
- **A test against zero** for equality whose only use is the branch,
  with nothing between the two that defines a value or writes a pin,
  emits nothing; the branch is `cbz` or `cbnz` on the register. `bdnz`
  becomes `sub; cbnz`.

A `cmpw` and its branch with all three bits stored: 11 instructions
before, 8 now; with the stores dead (1.6), 2.

### 1.6 The scheduler's check kept every flag store alive (`dead_context_stores_over_preempt_checks`)

`eliminate_dead_context_stores` treated the cooperative scheduler's check
(`CHECK_PREEMPT`, at every loop head when `guest_scheduler` is on, which it
is in the app) as reading the whole context. So in the app no store inside
a loop was ever dead, at any level, and the bench's loops showed the pass
doing nothing. With the new setting the check reads nothing. That is right
for the guest's own execution: when the check yields, no guest code runs on
this thread's context before it resumes, and a store taken for dead is one
that every path overwrites before reading. What can tell the difference is
something that inspects a parked thread's registers (a debugger, a crash
report, a title reading another thread's context): it may see an older
value in a flag or register the thread was about to overwrite.
`dead_context_store_verify` covers it like the rest of the pass.

### 1.7 Float and VMX float with one test (`a64_tight_sequences`)

Scalar `fadd`, `fsub`, `fmul`, `fdiv`, `fmadd` and friends tested their
inputs for NaN, did the operation, and tested the result: seven
instructions and two taken branches round one. For all of these a NaN
input gives a NaN result, so one test of the result finds both cases. The
operation is now done first (into a register no input is in), `fcmp;
b.vs`, and the code that picks PowerPC's NaN (the first NaN input by
position, quieted, or the default NaN) is out of line, reading the inputs
where they are (`EmitFpOpWithPpcNanTight`, `a64_sequences.cc`). Three
instructions on the usual path, four when the destination is an input.

VMX `vaddfp`, `vsubfp`, `vmaddfp`, `vnmsubfp`, `vmaxfp`, `vminfp` copied
both operands to scratch registers, spilled them to the stack for the
lane-by-lane NaN choice, and ran the choice's test in line. On a core whose
flush-to-zero mode covers inputs (the feature test the sequences already
use; the software flushing path is unchanged) the operation now reads the
operands where they are, writes the destination directly when it is not an
operand, and the lane-by-lane choice is out of line reading the operands'
registers (`EmitVmxNanFixupTight`, `EmitVmxMaxMinTight`, `a64_seq_util.h`).
The choice itself is the same code. A VMX add: 9 instructions before, 5 now.

The setting also makes 64-bit AND, OR and XOR with a constant use ARM64's
immediate form when the constant has one (`IsLogicalImmediate64`).

Same results by construction; checked by the generated instruction tests
(3,456 `fmadd` cases, 15,600 `vmaddfp` cases and so on, every NaN
combination with expected values) and by `seq_float_overlap.s`, which adds
what those lack: destinations that are operands.

What is left per float operation is the test itself. `a64_vmx_nan_fixup =
false` (an older setting, not this branch's) removes it for VMX at the
price of ARM's choice of NaN; row "overhaul 9" shows what that is worth.

### 1.8 Calls and returns (`a64_lean_calls`)

A call and return cost about 45 host instructions beyond the guest's own
(read off the disassembly of the bench's `calls` loop: 14 at the call
site, 21 in the prologue, 10 at the return). The setting takes that to
about 33 without changing what is recorded for `longjmp`:
- The return address went call site to frame slot to register (`mov; str`,
  later `ldr`). It is a constant known at the call: it is now materialised
  there once (`A64Emitter::EmitCallReturnAddress`), and not at all when the
  store of the link register just before has left it in x0. The frame's
  slot is unused in such a function and its zeroing in the prologue goes.
  (Applies to a function all of whose calls follow their
  `SET_RETURN_ADDRESS` in the same block, which is how the front end emits
  them; any other function keeps the old form whole.)
- A callee that already has code is reached through an 8-byte literal in
  the caller's tail (`ldr x9, literal`) instead of three instructions of
  immediate. (`a64_linked_calls`, the earlier setting, does the same for
  callees translated later and patches the literal.)
- The prologue's stackpoint record is written with a shifted add and a
  paired store, and its overflow test is one bit test when
  `a64_max_stackpoints` is a power of two (it is, 65,536): 11 instructions
  for 17. The record's contents are the same three fields.
- The tests after a call (pending stack synchronisation) and at a return
  (is the target my caller?) branch in one instruction on the usual path
  (the second with `a64_short_forward_branches`).

Not touched: what the record holds, `ResolveFunction`, the stack
synchronisation helper. The tests cannot run a `longjmp`, so nothing that
only a `longjmp` exercises was changed.

### 1.9 The lean thunk for kernel calls did nothing (`a64_lean_extern_thunk`)

Bench finding 3. The setting's condition was "the `CALL_EXTERN` is the last
instruction of its block". An import thunk is `sc 2; blr`: the `blr`'s
instructions follow the call in the same block, so the condition never
held for exactly the calls it was written for. The condition is now "no
value defined before the call is used after it", checked by position
(`A64Emitter::CallExtern`). The bench's `host_calls` loop goes from 18.5
to 10.7 with it. `inline_extern_calls` (also older) then removes the guest
call round the kernel call: 4.7.

Neither can be run by the instruction tests (no kernel calls there); the
bench runs both and checks the call count.

### 1.10 `rlwinm`, `rlwimi`, `rlwnm` as a 32-bit rotate (`ppc_rlw_narrow_rotate`)

PowerPC defines these on the low word doubled into both halves of a 64-bit
value, and the front end built exactly that: mask, shift left 32, OR,
64-bit rotate, mask (five instructions). When the mask does not wrap round
(MB <= ME, the usual case) only the low half survives, which is a 32-bit
rotate: `ror w` and, if the mask is not the whole word, one `and`
(`NarrowRotate32`, `ppc/ppc_emit_alu.cc`). Masks that wrap keep the old
form. The common shift forms (`slwi`, `srwi`) were already handled by the
front end and are unchanged.

### 1.11 The 16 KiB address fix-up as bit tests (`a64_address_fixup_bit_tests`)

On the phone every load and store with a computed address carries `mov
w17, #0xE0000000; cmp; cset; add`. An address is in 0xE0000000+ exactly
when its top three bits are set, so: `tbz w0, #31, done; tbz w0, #30,
done; tbz w0, #29, done; add w0, w0, #0x1000`. One instruction for
anything below 0x80000000 (stacks, the virtual heaps), two for
0x80000000 to 0xBFFFFFFF (the executable's data, the 64 KiB physical
range), three for 0xC0000000 to 0xDFFFFFFF, four where the page is added.
No flag and no scratch register is used.

This is the one change here that trades instruction count against
something else: the four instructions it replaces never branch (the
session before chose them over a compare and branch for that reason), and
this form takes one short forward branch per access. Counted, it is the
largest single saving on the memory loops at 16 KiB; whether it is time
saved is for the phone. `a64_stack_access_no_fixup` (older, with its
verify mode) still removes the fix-up altogether for stack accesses and
combines with it.

The instruction tests reach the first two tests (their code and data are
at 0x80000000+ and their stack below); nothing in them touches 0xC0000000
or above, so the third test and the add are checked by reading only.

## 2. What it reached

**The aim was a third to a half fewer host instructions per guest
instruction. On the bench loops that is reached and passed for integer,
float, vector and memory code with settings that are exact (row 7 below):
between 51% and 59% fewer at 4 KiB pages. Calls are 22% down, kernel calls
42%. On the compiled-C test corpus it is 27% to 31% with the exact
settings and 29% to 38% with the ABI level of the dead-flag pass: the low
edge of the aim, not past it.** None of it has run on a phone, and these
are instruction counts, not times.

Host instructions run per guest instruction on the bench loops, counted in
the session's container under qemu ("How it is measured"). Each numbered
row adds to the one before it. 4 KiB pages (what the workflow's runner has):

| Settings | integer | float | vector | calls | copy | host_calls |
|---|---|---|---|---|---|---|
| defaults | 5.523 | 7.778 | 10.222 | 3.963 | 6.271 | 18.500 |
| `claude/translator-wide`'s settings together (bench run 13's last row) | 4.904 | 7.444 | 9.778 | 3.852 | 5.467 | 18.000 |
| 1: `a64_tight_sequences` | 5.142 | 5.556 | 8.000 | 3.963 | 6.271 | 18.500 |
| 2: + `a64_fuse_compare_branch`, short branches both ways | 4.523 | 5.222 | 7.556 | 3.852 | 5.472 | 18.000 |
| 3: + `ppc_rlw_narrow_rotate`, `register_allocation_elide_copies` | 4.047 | 5.111 | 7.333 | 3.630 | 5.028 | 17.833 |
| 4: + `a64_lean_calls`, `a64_linked_calls`, `a64_lean_extern_thunk`, `a64_address_fixup_bit_tests` | 4.047 | 5.111 | 7.333 | 3.074 | 5.028 | 10.667 |
| 5: + `a64_extended_gpr_pool`, wide pinning with flags, 10 integer and 16 vector pins | 3.952 | 5.111 | 7.222 | 3.074 | 5.017 | 10.667 |
| 6: + `pin_guest_registers_coalesce` | 2.523 | 3.222 | 5.000 | 3.074 | 3.406 | 10.667 |
| 7: + `eliminate_dead_context_stores = 2`, `dead_context_stores_over_preempt_checks` | 2.523 | 3.222 | 5.000 | 3.074 | 3.050 | 10.667 |
| 8: as 7 with `eliminate_dead_context_stores = 3` (ABI) | 1.571 | 3.222 | 4.556 | 2.852 | 3.036 | 9.667 |
| 9: 8 + `a64_vmx_nan_fixup = false` | 1.571 | 3.222 | 3.222 | 2.852 | 3.036 | 9.667 |
| 10: 8 + `inline_extern_calls` | 1.571 | 3.222 | 4.556 | 2.852 | 3.036 | 4.667 |

The same at 16 KiB pages, the phone's (only countable under qemu; the
runner cannot show it). The fix-up of section 1.11 is in row 4:

| Settings | integer | float | vector | calls | copy | host_calls |
|---|---|---|---|---|---|---|
| defaults | 5.523 | 7.778 | 11.111 | 4.852 | 8.481 | 18.500 |
| `claude/translator-wide`'s settings together (bench run 13's last row) | 4.904 | 7.444 | 10.667 | 4.741 | 7.677 | 18.000 |
| 1: `a64_tight_sequences` | 5.142 | 5.556 | 8.889 | 4.852 | 8.481 | 18.500 |
| 2: + `a64_fuse_compare_branch`, short branches both ways | 4.523 | 5.222 | 8.444 | 4.741 | 7.682 | 18.000 |
| 3: + `ppc_rlw_narrow_rotate`, `register_allocation_elide_copies` | 4.047 | 5.111 | 8.222 | 4.519 | 7.238 | 17.833 |
| 4: + `a64_lean_calls`, `a64_linked_calls`, `a64_lean_extern_thunk`, `a64_address_fixup_bit_tests` | 4.047 | 5.111 | 7.556 | 3.407 | 5.646 | 10.667 |
| 5: + `a64_extended_gpr_pool`, wide pinning with flags, 10 integer and 16 vector pins | 3.952 | 5.111 | 7.444 | 3.407 | 5.635 | 10.667 |
| 6: + `pin_guest_registers_coalesce` | 2.523 | 3.222 | 5.222 | 3.407 | 4.025 | 10.667 |
| 7: + `eliminate_dead_context_stores = 2`, `dead_context_stores_over_preempt_checks` | 2.523 | 3.222 | 5.222 | 3.407 | 3.669 | 10.667 |
| 8: as 7 with `eliminate_dead_context_stores = 3` (ABI) | 1.571 | 3.222 | 4.778 | 3.185 | 3.655 | 9.667 |
| 9: 8 + `a64_vmx_nan_fixup = false` | 1.571 | 3.222 | 3.444 | 3.185 | 3.655 | 9.667 |
| 10: 8 + `inline_extern_calls` | 1.571 | 3.222 | 4.778 | 3.185 | 3.655 | 4.667 |

The reduction against the defaults:

| | integer | float | vector | calls | copy | host_calls |
|---|---|---|---|---|---|---|
| 4 KiB, row 7 against defaults | -54% | -59% | -51% | -22% | -51% | -42% |
| 4 KiB, row 8 (ABI) | -72% | -59% | -55% | -28% | -52% | -48% |
| 16 KiB, row 7 | -54% | -59% | -53% | -30% | -57% | -42% |
| 16 KiB, row 8 (ABI) | -72% | -59% | -57% | -34% | -57% | -48% |

How to read it:
- Rows 1 to 4 are the exact changes that need no pinning. Row 5 is the
  pool and ten pins without coalescing: almost nothing, as bench run 13
  found for wide pinning (moves in place of memory operations). Row 6 is
  where a loaded value lives in its pin, and it is the largest single
  step for every kind of code.
- Row 7 is the whole set at the level I judge exact. Row 8 is the ABI
  level of the dead-flag pass on top; the integer loop's 2.52 to 1.57 is
  six flag bits per pass that nothing reads.
- `host_calls` here is lower than the runner's figure for the same code
  (the runner counts the host function too).
- The integer loop at row 8 is 16 host instructions for 10 or 11 guest
  ones: there is little left to take there. What is left elsewhere:
  section 5.

The compiled-C test suites (host instructions run inside the code cache
over each whole suite; the guest work is the same in every column):

| Suite | defaults | everything, level 2 | everything, ABI level |
|---|---|---|---|
| `seq_cc_int` | 626,383 | 429,656 (-31.4%) | 388,091 (-38.0%) |
| `seq_cc_64` | 21,934 | 16,094 (-26.6%) | 13,984 (-36.2%) |
| `seq_cc_vec` | 75,058 | 53,489 (-28.7%) | 53,301 (-29.0%) |

"Everything" is the matrix's "overhaul, everything" line: this branch's
settings and the earlier sessions'. These suites are short functions that
call each other and work on stack arrays, so calls and memory accesses
weigh more than in the bench loops, and those are the two things this
branch moved least. Counted with:

```bash
T=$PWD/x360/emulator-core/src/main/cpp/xenia/src/xenia/cpu/ppc/testing
qemu-aarch64 -L /usr/aarch64-linux-gnu -one-insn-per-tb -d exec,nochain \
  -dfilter 0xaa0000000..0xaafffffff -D /tmp/log \
  /tmp/ppc-tests/xenia-cpu-ppc-tests --test_path=$T --test_bin_path=/tmp/ppc-test-bin \
  --test_skip_file=<(grep -v '^#' $T/skip.txt | grep .) <settings> seq_cc_int
grep -c '^Trace' /tmp/log
```

**The runner's own table for this branch**: see section 7.

## 3. Tests

New files in `cpu/ppc/testing/` (run by every line of the matrix):

| File | Cases | What it is |
|---|---|---|
| `seq_pin_coalesce.s` | 12 | by hand, expected values worked out by hand: a register's old value used after the register is rewritten, destinations that are their own operands, swaps, one value in two registers, a value used after the next store to its register, compares whose operands change before the branch that uses them (each of the three bits; a compare with zero in a loop; two fields from the same operands), `bdnz` with the count mirrored in a register that is rewritten, loads into registers that are then operands and destinations, and float operations, float compares, loads and stores between an integer compare and its branch |
| `seq_float_overlap.s` | 31 | from `ci/gen/gen-float-overlap-tests.py`: scalar and VMX float operations whose destination is one of their operands, with NaNs among the operands and NaNs the operation makes, each operand position, and chains in one block. Expected values from a run with every setting off (which passes the generated tests of the same instructions) |

The generated instruction tests already hold every NaN combination of the
float and VMX operations with three distinct registers (3,456 `fmadd`
cases, 15,600 `vmaddfp`); the second file adds the overlap they lack.

New lines in `ci/matrix.txt` (after the earlier session's): the pool alone
and allocating from the top; coalescing plain, with stress pinning over
everything, in its verify mode, and with the old seven-register pool;
copy elision alone and with pins; the compare forms with pins and dead
stores; `a64_tight_sequences` over everything; lean calls with short
branches, with long ones, and with every function emitted twice; the
narrow rotate alone and with copies and pins; dead stores with the
scheduler's checks emitted; the bit-test fix-up at 16 KiB, alone and with
pins; and four lines over everything with the whole set: plain, with
stress pinning and the scheduler's checks, with the ABI level of the
dead-flag pass, and at 16 KiB.

`register_allocation_from_top` is a test setting, not for play.

What the tests still do not cover (as before): kernel calls (so
`a64_lean_extern_thunk` and `inline_extern_calls` are only run by the
bench's `host_calls` loop, which checks the number of calls made),
`longjmp`, the scheduler's yield, anything on the phone. New on that
list: guest addresses from 0xC0000000 (section 1.11).

Deliberate faults, to see that the tests notice (each put in, seen, and
taken out):

| Fault put in | Plain run (stress pins, 2,116 tests) | Verify mode |
|---|---|---|
| a loaded value stays in its pin after the pin is rewritten (`PlanPinHomes`, the test of the last use) | 12 tests fail, 43 states differ | 2,199 mismatches reported |
| a scalar float result written over an operand before the NaN test (`EmitFpOpWithPpcNanTight`) | 7 fail (all in `seq_float_overlap.s`) | |
| a VMX float result written over an operand (`EmitVmxFpBinOp_V128`) | 3 fail (same file) | |
| the flags cache never forgets what the flags hold | 1 fails (`cmp_then_flag_users_then_branch`, added for it: before that test existed, none) | |
| a value computed into a pin that an earlier computed value still lives in (rule 3 of `PlanPinHomes`) | **not noticed** | not noticed |
| every truncation treated as a copy, whatever uses its result | **not noticed** | not noticed |
| the zero test used although the register is rewritten before the branch | **not noticed**, at either flag level | |
| the return address assumed still in x0 after any instruction | **not noticed** | |

The four that went unnoticed are to be read as limits of the tests, not as
proof the checks are unneeded. What I can say about each: rule 3 only
matters for a value that was itself computed into the pin and is used
between the next result's definition and its store, which one PowerPC
instruction's HIR seldom does (for loaded values the same case is caught by
the last-use test, the first row); the truncation list is conservative, and
no sequence the tests reach reads more than 32 bits of a 32-bit operand;
the front end emits nothing between the link register's store and its
call, so the x0 rule guards against a front end change. The zero test is
the one I would look at first: `seq_pin_coalesce.s` has two cases written
for it (`cmp_zero_register_reused`, `bdnz_with_register_reuse`) and neither
makes the fault show, so either the fused form is not chosen there or the
register happens to survive; I did not find out which. The form itself is
exercised (the bench's integer loop ends in `sub; cbnz`).

## 4. The settings: what I judge safe to default on, and the order on the phone

All of this branch's settings are off by default, like the earlier
sessions'. "Exact" below means the emitted code computes the same values
as before in every case, by construction, and the whole test corpus says
the same; "assumes" means there is something a title could do that makes
it wrong, with the verify mode that would show it.

| Setting | Kind | My judgement |
|---|---|---|
| `a64_tight_sequences` | exact | default on. It is the float and vector work, pure instruction selection, and the most heavily tested (every NaN case of every operation) |
| `a64_fuse_compare_branch` (as extended), `a64_short_backward_branches`, `a64_short_forward_branches` | exact | default on |
| `ppc_rlw_narrow_rotate` | exact | default on |
| `register_allocation_elide_copies` | exact, but rests on which sequences read only 32 bits of a register | default on after one device run with `pin_guest_registers_coalesce_verify` (which checks the copies of pinned values) |
| `a64_lean_calls`, `a64_linked_calls`, `a64_compact_stackpoint_push` | exact for calls and returns; `longjmp` cannot be tested here | default on after a device run in a title that uses `setjmp` (the record's contents are unchanged, so I expect nothing) |
| `a64_lean_extern_thunk` | assumes no value is held in a vector register across a kernel call, which it now checks per call site | default on after a device run: it has never applied to a real kernel call before this branch |
| `a64_address_fixup_bit_tests` | exact; fewer instructions, more branches | the phone decides (it is a time question): try it with `log_translation_stats` and a profile |
| `a64_extended_gpr_pool`, `pin_guest_registers` + `_wide` + `_flags`, `pin_guest_registers_max_int = 10`, `_max_vec = 16`, `pin_guest_registers_coalesce` | exact in what they compute; they change which host register holds what in every function | the largest change. Default on only after device runs with `a64_pin_verify` and then `pin_guest_registers_coalesce_verify` show no line, in more than one title |
| `eliminate_dead_context_stores = 2` + `dead_context_stores_over_preempt_checks` | exact for the guest; a debugger or crash report may see an older flag | default on after a device run with `dead_context_store_verify` |
| `eliminate_dead_context_stores = 3` | assumes the ABI: no code reads cr0, cr1, cr5 to cr7 after a call or return without setting them | per title, after `dead_context_store_verify` at level 3 shows no line. Worth it: it is the difference between "level 2" and "ABI" in the tables. Section 5 has what would make it safe to default |
| `inline_extern_calls` | older; as its notes | after `a64_lean_extern_thunk` |
| `a64_vmx_nan_fixup = false` | changes results (ARM's choice of NaN in VMX operations) | not a default; a per-title option worth knowing about for vector-heavy titles |

Order on the phone, one step at a time against the run before it, each
with `log_translation_stats = true`, config built whole on the PC as
before, saves backed up:

| # | Add | Verify run first | What to look for |
|---|---|---|---|
| 0 | nothing | | the baseline of this build |
| 1 | `a64_tight_sequences`, `a64_fuse_compare_branch`, `a64_short_backward_branches`, `a64_short_forward_branches`, `ppc_rlw_narrow_rotate` | none needed | float or vector garbage would mean a NaN path is wrong: none expected |
| 2 | `a64_lean_calls`, `a64_linked_calls`, `a64_compact_stackpoint_push`, `a64_lean_extern_thunk` | | a title that uses `setjmp`; any corruption after a kernel call |
| 3 | `a64_address_fixup_bit_tests` | | a profile: translated code's share against step 2 |
| 4 | `a64_extended_gpr_pool` (start-up setting), `pin_guest_registers`, `pin_guest_registers_wide`, `pin_guest_registers_flags`, `pin_guest_registers_max_int = 10`, `pin_guest_registers_max_vec = 16` | `a64_pin_verify`: no `PinVerify:` line | the third `TranslationStats:` line: functions pinned, blocks executed in pinned functions |
| 5 | `pin_guest_registers_coalesce`, `register_allocation_elide_copies` | `pin_guest_registers_coalesce_verify`: no `TranslatorVerify:` line, "self-check mismatches" 0 | this is the step the instruction counts fall at |
| 6 | `eliminate_dead_context_stores = 2`, `dead_context_stores_over_preempt_checks` | `dead_context_store_verify`: no `TranslatorVerify:` line | |
| 7 | `eliminate_dead_context_stores = 3` | `dead_context_store_verify` at 3, per title | |
| 8 | `inline_extern_calls` | | |

Steps 1 to 3 can be one run if time is short: they are the exact ones.
The earlier sessions' settings not named here (`a64_stack_access_no_fixup`,
`propagate_context_constants`, `translate_ahead`,
`simplification_skip_unchanged_blocks`, `a64_thunk_fpcr_compare`,
`ppc_mcrf_copies_field`) are unchanged and combine with these;
`translator-wide.md` section 5 has their order and verify modes. The whole
set together is the matrix's "overhaul, everything" line.

## 5. Not done, and how to go on

Largest first.

- **FPCR is written at the first float operation of every block** (`ldr;
  msr`, and `msr` is not a cheap instruction on a real core). The emitter
  forgets the mode at each block start because it cannot know which
  predecessor ran. To go on: give each block an assumed entry mode (that of
  the first edge emitted into it), and make every other edge conform before
  it branches, with the switch on the edge; a call leaves the mode unknown.
  It is a change to `A64Emitter::Emit`'s block loop and to the branch
  sequences (a branch whose target assumes another mode goes through a
  stub in the tail). Float results depend on it, so it wants the whole
  corpus at both modes. Two instructions and a system register write per
  float-using block.
- **The stackpoint record and the check after every call**: 11 + 3
  instructions in every prologue and epilogue and 2 after every call, all
  for `longjmp`. Two ways on, both needing a `longjmp` test first (the
  runner crashes on one today because return-site records live in
  `XexModule`; a stub of that for `RawModule` would be the first step):
  (a) link guest frames through the host stack instead of a side array
  (four instructions in the prologue, two in the epilogue, no overflow
  test), with `ResolveFunction` walking the list; (b) have
  `ResolveFunction` return a stub that repairs the stack before the return
  site runs, which removes the check after every call.
- **Making the ABI level of the dead-flag pass safe by default.** Level 3
  is wrong for a function that returns a result in a condition register
  field to a caller that reads it. A caller that does that is visible when
  it is translated (a load of cr0, cr1, cr5 to cr7 reached from a call with
  no store between: the pass has the data flow). Recording the callee's
  address then, before the callee is translated (callers are usually
  translated first), and translating recorded callees at level 2, would
  make level 3 right in every case where the caller is seen first, and
  detectable (a counter) in the rest.
- **Scalar float still tests every result** (two instructions per
  operation). The study's R8 (PowerPC's choice of NaN as a setting) is
  untouched.
- **A pinned register is written back before every call that could read
  it.** A static mapping shared by all functions (the study's R1 in its
  FEX form) would remove that for the registers both sides agree on. The
  pool and the coalescing here are what it would be built on: the
  allocator already treats a pin's host register as a value's home.
- **`lbz` and `lhz` still zero-extend with an instruction**, and the load
  that precedes them already did; the 8-bit and 16-bit load sequences were
  not audited for it as the 32-bit ones were.
- **The matrix's `#16k` lines still run in processes of 1,500 tests.**
  Section 0's fix should make that unnecessary; one `#16k` line without the
  split would show it and save the job a quarter of an hour.
- **`log_translation_stats` does not print the new counters** yet
  (`pin_coalesced_loads`, `pin_coalesced_defs`, `copies_elided`,
  `compares_reused_flags`, `branches_on_flags` are counted, in
  `compiler/translation_stats.h`, and not in `FormatTranslationStats`).

## 6. For `HANDOFF.md` and `ARCHITECTURE.md`

Not edited by this session. To copy in at the merge.

For `HANDOFF.md`, "Right now": the translator tests' failure on
`claude/integration-1115` was qemu's (section 0), fixed in the test build;
the settings of section 4 and their order; the figures of section 2 with
the run link; that nothing has run on a device.

For `ARCHITECTURE.md`, upstream files changed (all marked `hakuX-Boosted:`):

| File | Change |
|---|---|
| `cpu/backend/a64/a64_emitter.h`, `.cc` | the 14-register map and `SetExtendedGprPool`; `HostCallReg`; the saves round bare host calls; the pin masks of an instruction computing into a pin; `EmitPinHomeVerify`; the flags cache (`FlagsKey`, the `cmp` wrapper, `FlagsHoldCompare`), the fused zero test; `a64_lean_calls` (`EmitCallReturnAddress`, `EmitLoadCallTarget`, the lean `PushStackpoint`, fixed call literals); the lean extern thunk's condition; settings `a64_tight_sequences`, `a64_lean_calls`, `a64_address_fixup_bit_tests` |
| `cpu/backend/a64/a64_backend.cc` | setting `a64_extended_gpr_pool`; the guest-to-host thunk saves x9 to x15 with it; the stack synchronisation helper's registers; `denormflush_is_copy` |
| `cpu/backend/a64/a64_sequences.cc` | context loads and stores emit nothing when the value's register is the pin; elided truncations and extensions; immediate forms of AND/OR/XOR; the tight scalar float helpers; the tight VMX multiply-add and max/min paths; the bare host calls |
| `cpu/backend/a64/a64_seq_util.h` | `IsLogicalImmediate64`; `EmitVmxNanFixupTight`, `EmitVmxMaxMinTight`, the tight VMX binary operation; the bit-test fix-up |
| `cpu/backend/a64/a64_seq_control.cc` | conditional branches take a fused zero test or the live flags |
| `cpu/backend/machine_info.h` | `denormflush_is_copy` |
| `cpu/compiler/passes/register_allocation_pass.h`, `.cc` | `PlanPinHomes`, `IsElidableCopy`, allocation from the top; settings `pin_guest_registers_coalesce`, `_verify`, `register_allocation_elide_copies`, `register_allocation_from_top` |
| `cpu/compiler/passes/guest_register_pinning_pass.cc` | up to ten integer pins with the pool; loop weights per instruction |
| `cpu/compiler/passes/dead_context_store_pass.cc` | setting `dead_context_stores_over_preempt_checks` |
| `cpu/compiler/translation_stats.h` | the new counters |
| `cpu/hir/hir_builder.h`, `.cc` | `pin_home_verify`, `elided_truncates` |
| `cpu/ppc/ppc_emit_alu.cc` | setting `ppc_rlw_narrow_rotate`, `NarrowRotate32` |
| `cpu/ppc/testing/ci/stubs.cc`, `matrix.txt`, `gen/gen-float-overlap-tests.py` | section 0's fix; the new lines; the generator |
| `cpu/ppc/testing/seq_pin_coalesce.s`, `seq_float_overlap.s` | new tests |

Outside the core: `boosted/bench/core/bench_xlat.cc` (the counting mode),
`bench_env.cc` (section 0's fix, for runs under qemu at 16 KiB),
`run-core.sh` (the rows), `boosted/bench/tools/xlat-count-qemu.sh` (new),
`boosted/bench/CMakeLists.txt` (a definition CMake dropped).

## 7. Build state

(Filled in after the push: see the commit that follows this one.)

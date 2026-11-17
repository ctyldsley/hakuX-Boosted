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

Done in the first round (every setting off by default; the defaults' code
unchanged by it):
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

Done after the first round (2026-11-16, second part; sections 1.12 and
1.13):
- `log_translation_stats` prints the overhaul's counters.
- A `longjmp` test (`seq_longjmp.s`, eight cases), which the tests could
  not make before, and the four faults it found on the way: two in the test
  set-up, one in the leaf inliner (a mistranslation, fixed by default) and
  one in the search for the frame a `longjmp` lands in (fixed by default
  where the records confirm the frame; section 1.12).
- The call records kept in the host frames (`a64_frame_chain`, with
  `a64_frame_chain_verify`): the bench's calls 3.07 to 2.59 on top of row
  7, kernel calls 10.67 to 8.50 (section 1.13). Off by default.

- A guard for the ABI level of the dead-flag pass
  (`dead_context_stores_abi_guard`, on by default, section 1.14): the two
  kinds of hand-written code that level was wrong for are found and
  translated round, and what cannot be repaired is counted and logged.

- FPCR taken as known at the start of a block whose predecessors agree
  (`a64_fpcr_known_block_entry`, with a verify mode; section 1.15). Off by
  default. And a fault in upstream's `mtfsfi` found on the way, recorded
  there, not changed.

- `register_allocation_elide_copies` also takes the zero extension after
  a byte or halfword load (`lbz`, `lhz`) for a copy, as it did after a
  32-bit one. The question left open in the first round (a load that
  faults into the MMIO handler) has an answer: on ARM64 the handler
  emulates 32-bit loads only (`cpu/mmio_handler.cc`), so no byte or
  halfword load has its register written by it. Small: `seq_cc_int`
  429,656 to 429,374 with everything on.

Done in the third round (the night of 16 to 17 November; section 1.16):
- Eighteen host registers for values and pins (`a64_scratch_gpr_pool`):
  the bench's integer loop 2.52 to 2.24.
- Guest registers kept in the same host register by all translated code
  (`a64_shared_guest_registers`; 5a D, in the form the measurement
  pointed to): with four of them and eighteen registers the compiled-C
  corpus runs 5.9% fewer host instructions than the second round's best
  (380,803 against 404,546; 39% below the defaults) and the bench's
  calls go from 2.59 to 2.26. A kernel call gets eight instructions
  dearer. Both off by default, both start-up settings.
- A bench program whose host function reads and writes guest registers
  (`host_args`), which is what tests the thunks' side of it, and a count
  of non-local returns that found no landing frame, with a setting that
  makes one fail a test line.

In progress: nothing half-written is on the branch.

Next, in order (section 5 lists everything left; section 5a is the
handover):
1. The device runs of section 4.
2. No FPCR write at all in functions without VMX float code (section
   1.15's last paragraph before the tests).
3. More shared registers, or other ones, once a device profile says
   which (section 1.16 chose four from the test corpus).

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

### 1.12 Non-local returns: a test, and what it found

`cpu/ppc/testing/seq_longjmp.s`: `setjmp` and `longjmp` written out in
PowerPC, and eight cases: out of three frames, out of one, three times to
one buffer from a loop, with float and vector registers live in the frame
landed in, through `bctrl`, into a function that made no stack frame, and
into the outermost and the middle activation of a function that calls
itself. Every case changes registers in the frames it leaves and reads
them after landing. The suite runs with every line of the matrix, so the
resume stubs of pinned functions (never run anywhere before) now run under
plain, wide and stress pinning with their verify modes.

To get the first case to pass took four changes, each a fault of its own:

1. **Raw modules kept no return sites.** `ResolveFunction` recognises a
   non-local return only at an address the module has marked as a return
   site, and only a `XexModule` could mark one. `RawModule` (what the tests
   load) now keeps a set (`RawModule::MarkReturnSite`, `IsReturnSite`);
   `PPCHIRBuilder::SetReturnAddress` and `ResolveFunction` ask it. Test
   set-up only.
2. **The leaf inliner folded `longjmp` into its caller.** `inline_leaf_calls`
   inlines a callee of a few instructions that ends in `blr`, taking the
   `blr` for a return to the call site. A callee that writes the link
   register first (`mtlr`) does not return there: inlined, its jump became
   a fall-through. The test's nine-instruction `longjmp` was inlined and
   never jumped. This is a mistranslation of any such leaf in a title, not
   a test artefact, so the fix is on by default: bodies containing `mtlr`
   are not inlined (`inline_leaf_keeps_lr_writers_out`, false gives the old
   behaviour for comparison).
3. **The check after a call sat in front of the resume address** when the
   front end writes a HIR comment ahead of each source offset, which it
   does with debug information on (the tests; not the app's default). The
   emitter now steps over comments as it does over source offsets. Test
   set-up only, unless a title is run with that debug setting.
4. **The wrong frame was restored.** A record is made on entry to a
   function with the guest stack pointer it was entered with.
   `FindStackpointSyncDepth` (taken from the x64 backend, where a record is
   made before the frame is allocated) stops at the first record whose
   pointer is not below the current one. That is the record of the landing
   function's *callee*: it was entered with the landing function's stack
   pointer. Its host stack pointer was restored, the landing function ran
   on its callee's host frame, read that frame's saved return address as
   its own, and its return was resolved as a jump to an unknown address.
   In the test that address was the runner's sentinel and the runner
   crashed; in a title the return is resolved through the general path
   (a function translated from the middle of another, a host frame
   leaked), which may well be why `setjmp` titles work at all today, but
   it is not what was meant. `FindLandingStackpointDepth`
   (`a64_longjmp_resume_live_frame`, **on by default**) picks the frame
   whose callee's record holds a return address inside the landing
   function; where the records do not confirm one it falls back to the old
   search, so behaviour changes only where the frame is positively
   identified. A function with no guest frame of its own and recursion are
   handled and tested. I made this default-on, against the rule for this
   branch, because the reference line of the matrix runs the suite at the
   defaults and the old search crashes on a correct program; turning it
   off gives the earlier behaviour exactly. It deserves one run of a title
   known to use `setjmp` with `log_translation_stats`: the counter
   "non-local returns resumed on the landing function's frame" says how
   often it applied.

### 1.13 The call record in the frame (`a64_frame_chain`)

What a call paid for `longjmp` before: eleven instructions on entry to
write a record into a per-thread array (`PushStackpoint`), three on exit
(`PopStackpoint`), two after every call to see whether the resolver had
asked for a stack repair (`EnsureSynchronizedGuestAndHostStack`).

With `a64_frame_chain` the record is the host frame itself:
- on entry `stp x30, x29` takes the place of `str x30` (the caller's x29
  goes in the frame's reserved slot), the guest stack pointer of entry goes
  into the unused upper half of the guest return address's slot, and
  `mov x29, sp`: three instructions more than no record at all;
- on exit and at tail calls `ldp x30, x29` takes the place of `ldr x30`:
  nothing more;
- after a call: nothing. The resolve thunk stores x29 (the frame the
  return was made from) in the backend context; `ResolveFunction` walks
  the frames back from it with the same rule as 1.12 (`FindLandingFrame`)
  and, when it finds the landing frame, answers with a five-instruction
  helper that makes that frame the stack pointer and x29 again and jumps
  to the return site (or its resume stub). The return site needs no check
  because the stack is already right when it runs.
- the host-to-guest thunk zeroes x29, which ends the chain of that entry
  into guest code. A jump out of a guest callback into the guest code that
  called the kernel is therefore not resumed in place (the array could
  name such a frame, across host frames it then abandoned; I do not
  believe a title depends on it, but it is the one behavioural
  difference, and the verify mode would report it as a mismatch).
- no array is allocated (a megabyte a thread at the default size), and
  there is no stackpoint overflow report: a runaway recursion ends at the
  host stack's guard page instead.
- `PopulatePseudoStacktrace` (the guest return addresses a crash report
  lists) has nothing to read and returns false.
- x29 no longer points at a host frame record while guest code runs, so a
  native unwinder (a crash dump) stops at the first guest frame. It could
  not get through guest frames before either.

`a64_frame_chain_verify` keeps both: the array decides, the chain is
walked as well, and a difference between the two frames is logged
("a64_frame_chain_verify: ...") and counted with the self-check
mismatches (so a test line with it fails on one).

The mode is read once at start-up (the thunks and every function must
agree); it needs `a64_enable_host_guest_stack_synchronization`, which is
on.

Counts, in the container under qemu (the runner's figures will be in the
bench run of this commit, rows "overhaul 11" to "13"): host instructions
per guest instruction, 4 KiB pages unless said.

| | calls | host calls |
|---|---|---|
| defaults | 3.963 | 18.500 |
| defaults + frame chain | 3.222 | 15.167 |
| row 7 (exact settings) | 3.074 | 10.667 |
| row 7 + frame chain | 2.593 | 8.500 |
| row 8 (ABI flag level) | 2.852 | 9.667 |
| row 8 + frame chain | 2.370 | 7.500 |
| row 8 + frame chain + `inline_extern_calls` | 2.370 | 4.667 |
| row 7, 16 KiB pages | 3.407 | |
| row 7 + frame chain, 16 KiB pages | 2.926 | |

(The host calls column is this script's count, which leaves out what the
host function itself does; it is lower than the runner's for that reason,
see "How it is measured".) So calls are now 35% below the defaults with
exact settings (they were 22%), and the loops that make no calls are
unchanged, as they should be.

On the compiled-C test corpus (host instructions run in the code cache by
the whole suite, everything on at the ABI level): `seq_cc_int` 388,091
without the chain, 363,263 with it (6% fewer; 42% below the defaults'
626,383).

### 1.14 The ABI flag level, guarded (`dead_context_stores_abi_guard`)

`eliminate_dead_context_stores = 3` takes cr0, cr1 and cr5 to cr7 for dead
at calls and returns, which is what the ABI says and what makes the
integer loop 1.57 instead of 2.52. It was wrong for two kinds of
hand-written code, and nothing but a verify run per title stood between
it and them. Both kinds can be seen in the code from the side that reads,
and the guard (on by default; it does nothing below level 3) looks:

- **A caller that reads a flag its callee returned.** A forward analysis
  of every function (`FindFlagReadsAfterCalls`, run before the pass can
  decide to leave the function alone): which volatile flag bytes may hold
  what a call left there, and which call. A load of such a byte names the
  callee, and that callee is translated with its flags kept at its returns
  (as at level 2). A callee is normally translated after its caller (on
  its first call), so the name is there in time. What the callee jumps to
  instead of returning is named too when the callee is translated.
- **A callee that takes a flag as an argument.** When a caller is
  translated, the first instructions of each function it calls directly
  are read from guest memory (`GuestCodeReadsFlagsAtEntry`: straight line
  from the entry, through unconditional jumps, to the first call, return
  or conditional branch): a read of a volatile field before a write keeps
  the flags alive at that call. A function found reading a flag at entry
  when it is itself translated is recorded as well, for callers
  translated after it.

What it cannot repair, it counts and logs once each:
- a callee already translated (with dead flags at its returns) when the
  caller that reads its flag is first seen: "found too late". It needs a
  callee shared with an earlier caller that ignores the flag. The log line
  says to use level 2 for the title;
- a read after a call through a pointer: the callee cannot be named, so
  every function translated from then on keeps its flags at returns (the
  level degrades to 2 at returns, stays 3 at calls), and the line says how
  many were translated before;
- a callee that reads a flag at entry only after a branch, or is called
  through a pointer: not seen by the scan. `dead_context_store_verify`
  remains the check for those.

`log_translation_stats` prints all five counts ("ABI flag guard: ...").
A title whose "found too late" and "through a pointer" counts are both 0
after a play session has shown no code the level is wrong for that the
guard did not handle.

On the bench and the test corpus the guard costs nothing: every figure of
row 8 is the same with it on and off (compiled code never does either
thing). The tests it is for: `seq_flags_across_blocks.s` has a callee
returning its answer in cr0 (which level 3 failed by design and skipped),
the same through a jump to another function, a callee branching on cr6
set by its caller, the same through a jump stub, and the pointer form.
With the guard off all five fail at level 3; with it on all pass. The
pointer form runs only in the matrix line made for it, because it turns
return-side elimination off for every test after it in the process.

### 1.15 FPCR known at the start of a block (`a64_fpcr_known_block_entry`)

Scalar float code runs with FPCR holding the guest's rounding mode, VMX
float code with a fixed value, and the emitter tracks which is loaded so
that consecutive operations in a block do not write it again. At the
start of every block it forgot: the first float operation of each block
loads the value from the backend context and writes FPCR (`ldr; msr`),
whatever the block before had done.

With the setting, a block starts in the mode its predecessors left when
all of them have been emitted before it and they agree. A block is
entered by the branches that name its label and by falling out of the
block before it, and by nothing else (a return site after a call is in
the middle of a block, where the call has already made the mode unknown),
so the emitter counts the branches to each block up front, records the
mode at each branch as it emits it, and at a block all of whose branches
it has seen compares them with the block above. Straight-line float code
over several blocks and the arms of a branch are covered, scalar and VMX
alike. A loop's first block is not (its back edge comes later), nor the
code after a call, nor the function's first block. Nothing about calls or
other functions changes, so it is exact by construction and can be turned
on and off freely.

`a64_fpcr_known_block_entry_verify` reads FPCR at every block entered
with a known mode and compares it with that mode's value; a difference
is counted with the self-check mismatches. It works on its own as well
(checking the prediction while emitting what was emitted before), which
is how to try it on a device first.

What it is worth is a question of time, not of instruction count, and so
not one this session can answer. The bench loops are single blocks and do
not change. The compiled-C corpus has little float code spread over
blocks: `seq_cc_int` 626,383 to 625,703 host instructions at the
defaults, `seq_cc_vec` 75,058 to 74,802. A write to FPCR costs far more
than an ordinary instruction on most cores, though (how much on the
phone's I do not know), and game code is float code in small blocks, so
the device may show more than these counts suggest:
`log_translation_stats` prints "FPCR writes emitted" and "blocks starting
in a known FPCR mode" to say how much of a title it reaches. The larger
step (no FPCR write at all in functions without VMX float code, by making
"scalar mode at every call and return" a rule for all translated code)
needs a second emission of each function or a prediction of what each
sequence does to the mode; section 5.

Tests: `seq_fpcr_blocks.s`, six cases with the guest rounding up, each
dividing 1.0 by 3.0 in a block reached in more than one way (scalar arm
and VMX arm, both scalar, a loop whose body ends in VMX, both VMX, after
a call): the quotient's last bit shows which mode it was computed in.
Two deliberate faults (a disagreement between predecessors ignored;
branches still to come ignored) each fail one of them and are counted by
the verify mode. The second showed the verify check had been placed in
front of the block's label, where only a fall-through reached it; it is
behind the label now.

**Found on the way, not changed:** `mtfsfi` sets the wrong bits.
`InstrEmit_mtfsfix` (`cpu/ppc/ppc_emit_fpu.cc`) takes the immediate from
the five-bit field at bits 16 to 20, of which the instruction's
immediate is the upper four, so the value written to the FPSCR field is
twice what the instruction says: `mtfsfi 7, 2` (round up) sets the
non-IEEE bit and round-to-nearest. It is upstream's, it changes the
rounding mode of any title that uses the instruction, and it is outside
this branch's brief, so it is only recorded here; the tests set the mode
with `mtfsf` so that a fix does not break them. Worth a look by the main
thread: the fix is `i.X.RB >> 1`, and `instr_mtfsfix.s` has cases to
extend.

### 1.16 Guest registers in the same host register everywhere (`a64_shared_guest_registers`) and eighteen host registers (`a64_scratch_gpr_pool`)

This is 5a D, built in the third round (the night of 16 to 17 November)
after the measurement below said it was worth it.

**What was left to take.** With everything on, 13% of the host
instructions the compiled-C corpus runs were still loads and stores of
guest general registers in the context: `seq_cc_int`, 404,546 in all,
26,720 loads and 27,058 stores (counted with a script that classifies
every instruction executed in the code cache; it is not in the tree, the
method is "How it is measured" plus qemu's `in_asm`). By register: r1
7,523 loads (every stack access reads the stack pointer), r3, and r26 to
r31, which a function's prologue stores to the stack and its epilogue
loads back. Pinning cannot reach these: a pin belongs to one function, is
written back before every call and read again after, and the cost model
therefore keeps registers that are busy round calls out of pins.

**The mapping.** `a64_shared_guest_registers = n` keeps the first n of
r1, r3, r31, r30, r29, r28 in x22 upward in *all* translated code. They
are pinned slots of every function (`GuestRegisterPinningPass::AddShared`
appends them after the function's own, marked `shared`), so every context
load and store of one is a register move or, with
`pin_guest_registers_coalesce`, nothing. What is different from a pin:
- the backend never loads or stores them (a shared slot has no bit in
  the masks): not at entry, not at calls between translated functions,
  not at returns, not at `longjmp` resume stubs;
- the context holds them only while host code runs. The host-to-guest
  thunk loads them before it calls guest code and stores them after; the
  two guest-to-host thunks (full and lean) store them before the host
  function and load them after it, since a kernel export reads its
  arguments from the context and writes its result there; the resolve
  thunk stores them (`ResolveFunction` reads r1 for `longjmp`). x22 to
  x27 are registers a host function preserves, so nothing is saved
  round a host call;
- the three places the emitter read r1 from the context (the two forms of
  the stackpoint push and the frame chain's prologue) read the register.

**Why it needs more host registers.** Each shared register is one fewer
for a function's own pins and values, and a loop wants those. With the
fourteen of `a64_extended_gpr_pool` the bench's integer loop went from
2.52 to 3.10 with six shared registers while calls went from 2.59 to
2.22: about break-even on the corpus. So `a64_scratch_gpr_pool` adds x4
to x7 to the pool, eighteen in all. No sequence used x7; x4, x5 and x6
were temporaries in five (the two atomic compare-and-exchange sequences
and three VMX ones), which now use x8, x16 and x17 whatever
the setting (the same code with other register numbers: the one change
to the defaults' code in this round). The saves round host calls made
from inside a sequence (the guest-to-host thunk and the two bare calls)
cover x4 to x7 as they cover x9 to x15, and the stack synchronisation
helper keeps out of them. With eighteen, up to fourteen pins
(`pin_guest_registers_max_int = 14`), less the shared ones.

**What it gives** (host instructions per guest instruction on the bench,
everything exact on plus the frame chain; container counts):

| | integer | calls | copy | host calls | host args |
|---|---|---|---|---|---|
| 14 registers (row 12) | 2.523 | 2.593 | 3.050 | 8.500 | |
| 18 registers | 2.238 | 2.593 | 3.044 | 8.500 | 6.556 |
| 18, 2 shared | 2.428 | 2.333 | 3.044 | 9.000 | |
| 18, 4 shared | 2.523 | 2.259 | 3.047 | 9.333 | 6.667 |
| 18, 6 shared | 2.714 | 2.222 | 3.052 | 10.000 | 7.111 |

and on the corpus (`seq_cc_int`, the whole suite's host instructions):
404,546 with 14 registers; 396,292 with 18; **380,803 with 18 and 4
shared** (5.9% fewer; 39% below the defaults' 626,383); 380,826 with 6.
So four is the number I would use: r1, r3, r31, r30. Float and vector
loops do not change.

What it costs: a kernel call stores and loads each shared register (the
"host calls" column: 8.50 to 9.33 with four). A kernel call runs hundreds
of host instructions of its own, so I expect that to be noise, but it is
the direction to watch on a title that makes very many small kernel
calls. Which registers a host function really needs is narrower than
"all" (it does not read r28 to r31 unless it captures or unwinds a
context), but I did not narrow it: wrong there would be silent.

What it changes that a title could notice:
- the context's copy of a shared register is stale while guest code
  runs. Anything that reads another thread's context, or this thread's
  from a signal handler, sees the value from the last host call: a crash
  report's registers, a debugger (the setting turns itself off with
  `debug`), and the pseudo stack trace's r1. The existing pins have the
  same property for their registers; this adds r1, which was never
  pinned for exactly this reason (the list in `ClassifySlot`'s comment).
  The readers that matter (`ResolveFunction`, kernel exports) are behind
  the thunks' stores;
- a guest access to part of a shared register's slot would read stale
  memory. The front end reads and writes a general register only as one
  64-bit value; the pass checks every function all the same, logs
  ("a64_shared_guest_registers: ...") and counts it with the self-check
  mismatches.

There is no verify mode in the usual sense (nothing to compare a shared
register with while guest code runs). What stands in for it: the tests,
the bench's new `host_args` program (below), and the counter above.

Tests: every suite runs with it in the matrix lines below; `seq_longjmp`
covers non-local returns with r1 shared (the landing function's r1 is in
a register and `ResolveFunction` reads the thunk's copy). The thunks'
synchronisation cannot be reached from the instruction tests (they call
no host function that reads a register), so the bench has a seventh
program, `host_args`: a host function that reads r3, r4, r31 and r1 from
the context and writes r3, in a loop that keeps a sum in r30. Its check
fails, and the counting script and the bench job with it, if the thunk
does not store before the call or does not load after it; both faults
were planted, in both thunks, and both were caught.

Both settings are read once at start. `a64_shared_guest_registers` needs
`a64_extended_gpr_pool`; with fourteen registers it works and is not
worth having.

## 2. What it reached

**The aim was a third to a half fewer host instructions per guest
instruction. On the bench loops that is reached and passed for integer,
float, vector and memory code with settings that are exact (row 7 below):
between 51% and 59% fewer at 4 KiB pages. Calls are 22% down, kernel calls
42%. On the compiled-C test corpus it is 27% to 31% with the exact
settings and 29% to 38% with the ABI level of the dead-flag pass: the low
edge of the aim, not past it.** None of it has run on a phone, and these
are instruction counts, not times.

Second round: with the call records in the host frames (section 1.13)
calls are 35% down with exact settings (2.59 against 3.96) and kernel
calls 54% by this script's count; the loops without calls are unchanged.
The table below is the first round's and does not have that row; section
1.13 has its own.

Third round: with eighteen host registers and four guest registers shared
by all code (section 1.16) the compiled-C corpus is at 380,803 host
instructions, 39% below the defaults with exact settings (it was 31%
after the first round), and the bench's calls at 2.26, 43% below.

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

**The runner's own table for this branch**: bench run 21, green,
https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43903306476 (commit
`0293269`; rows "overhaul 1" to "overhaul 10" of the translator section,
each against this branch's own "defaults" row). Its instruction counts are
the ones above to the digit, from the CPU's counter this time: integer
5.52, 4.05 at row 4, 3.95 at row 5, 2.52 at rows 6 and 7, 1.57 at row 8;
float 3.22 from row 6; vector 5.00 at row 7, 4.56 at row 8, 3.22 at row 9;
calls 3.07 from row 4 and 2.85 at row 8; copy 3.41, 3.05, 3.04 at rows 6,
7, 8. `host_calls` there counts the host function as well: 19.3 at the
defaults (run 13), 11.5 from row 4, 10.5 at row 8, 5.50 at row 10.

The same run's times on that runner (a GitHub ARM64 machine, lowest of
several samples, not the phone): nanoseconds per guest instruction at row 7
are 0.392 integer, 0.671 float, 0.394 vector, 0.195 copy, 0.761 host calls;
run 13 on the same kind of runner had 0.560 integer and 0.904 vector at the
defaults. In the scheduler section a kernel call that does nothing takes
10.0 ns with the lean calls and the lean thunk; run 12 had 18.6 ns. I
quote these as what the runner said, not as what a phone will.

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
| (second round) the frame chain's resume helper leaves x29 alone | `seq_longjmp` crashes with `a64_frame_chain` | |
| the chain names the callee's frame instead of the landing function's | `seq_longjmp` crashes with `a64_frame_chain`; 9 mismatches counted with `a64_frame_chain_verify` | |
| a frame does not record the guest stack pointer of entry | crash; 10 mismatches with verify | |
| the old frame search (`a64_longjmp_resume_live_frame = false`) | `longjmp_three_frames` crashes | this is the fault of section 1.12 |
| the guard off at level 3 | the five hand-written flag conventions of `seq_flags_across_blocks.s` fail | by design: it is what the guard is for |
| (third round) a guest-to-host thunk does not load the shared registers after the host function | the bench's `host_args` check fails, with the full thunk and the lean one | |
| a guest-to-host thunk does not store them before it | the same | |
| the resolve thunk does not store them (`ResolveFunction` reads a stale r1) | **not noticed** by `seq_longjmp` as it was: the tests passed through the general path. With `a64_longjmp_unconfirmed_is_mismatch` (added for this): 2 to 5 mismatches, and the line fails | the matrix's new lines set it |
| the guest-to-host thunk does not keep x4 to x7 | **not noticed** by 1,952 tests; `values_across_host_helper` (added for this) fails with allocation from the top | |
| FPCR: a disagreement between a block's predecessors ignored | `fpcr_join_vector_arm` fails; 1 mismatch counted by the verify mode alone | |
| FPCR: branches to the block still to come ignored | `fpcr_loop_header` fails; 2 mismatches counted | the verify check had to be moved behind the block's label to see this one |

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
makes the fault show. Counted with a temporary print over three suites, an
equality compare that could be fused has something written between it and
its branch once at level 2 and three times at the ABI level (against 27
and 50 where nothing is), so the guard does make decisions in the tests;
but in none of those does ignoring it give a wrong answer, and I did not
construct a case that does. The form itself is
exercised (the bench's integer loop ends in `sub; cbnz`).

## 4. The settings: what I judge safe to default on, and the order on the phone

All of this branch's settings are off by default, like the earlier
sessions', except three that correct or guard (marked in the table). "Exact" below means the emitted code computes the same values
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
| `eliminate_dead_context_stores = 3` | assumes the ABI: no code reads cr0, cr1, cr5 to cr7 after a call or return without setting them; with the guard of section 1.14 the code that does is found and handled, except the cases it counts | per title, after `dead_context_store_verify` at level 3 shows no line. Worth it: it is the difference between "level 2" and "ABI" in the tables. Section 5 has what would make it safe to default |
| `inline_extern_calls` | older; as its notes | after `a64_lean_extern_thunk` |
| `a64_longjmp_resume_live_frame` | a correction (section 1.12); acts only where the call records confirm the frame | **on by default already**; false is the earlier behaviour |
| `inline_leaf_keeps_lr_writers_out` | a correction (section 1.12) | **on by default already** |
| `a64_frame_chain` (start-up setting) | exact for calls and returns; changes how a `longjmp` finds its frame, and x29 | default on after a device run with `a64_frame_chain_verify` in a title that uses `setjmp` shows no mismatch |
| `dead_context_stores_abi_guard` | makes level 3 more careful, never less | **on by default already** (it only acts at level 3) |
| `a64_scratch_gpr_pool` (start-up setting) | exact; four more host registers for values and pins | with `a64_extended_gpr_pool`, after the same device runs as it |
| `a64_shared_guest_registers = 4` (start-up setting) | exact for guest code; the context copy of r1, r3, r30, r31 is stale except during host calls (section 1.16) | the last thing to turn on: after everything else has run clean on the device, in more than one title, watching for anything that reads a guest register from outside (crash reports aside) |
| `a64_fpcr_known_block_entry` | exact | default on after one device run with `a64_fpcr_known_block_entry_verify` alone shows "FPCR mismatches 0" |
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
| 7 | `eliminate_dead_context_stores = 3` (the guard is on with it) | `dead_context_store_verify` at 3, per title | the "ABI flag guard" counts in the stats line: "found too late" and "through a pointer" should be 0; if not, that title stays at 2 |
| 8 | `inline_extern_calls` | | |
| 9 | `a64_frame_chain` (start-up setting) | `a64_frame_chain_verify`: no `a64_frame_chain_verify:` line, "frame chain mismatches" 0, in a title that uses `setjmp` (the counter "non-local returns resumed on the landing function's frame" above 0 says it does) | call-heavy code is where it shows |
| 10 | `a64_fpcr_known_block_entry` | `a64_fpcr_known_block_entry_verify` on its own first: "FPCR mismatches" 0 | a profile: this one is about time, not instruction count |
| 11 | `a64_scratch_gpr_pool` (start-up setting), `pin_guest_registers_max_int = 14` | as step 4 (`a64_pin_verify`) | loops; "blocks executed in pinned functions" |
| 12 | `a64_shared_guest_registers = 4` (start-up setting) | none exists; the stats line's "self-check mismatches" must stay 0 (it counts an access the mapping cannot serve) | call-heavy code. If a title misbehaves only with this on, try 2, then 0, and say which |

Steps 1 to 3 can be one run if time is short: they are the exact ones.
The earlier sessions' settings not named here (`a64_stack_access_no_fixup`,
`propagate_context_constants`, `translate_ahead`,
`simplification_skip_unchanged_blocks`, `a64_thunk_fpcr_compare`,
`ppc_mcrf_copies_field`) are unchanged and combine with these;
`translator-wide.md` section 5 has their order and verify modes. The whole
set together is the matrix's "overhaul, everything" line.

## 5. Not done, and how to go on

Largest first. (The stackpoint record, the ABI flag guard and the
counters, which stood here after the first round, are done: sections 1.13,
1.14 and 1.12's opening.)

- **Which registers are shared, and how many**, is chosen from the test
  corpus (section 1.16). r4 to r10 (arguments) and r26 to r29 are the
  candidates a title's profile might prefer; the order is one array in
  `a64_emitter.cc` (`shared_gprs_`) and the count a setting. Float
  registers are not shared at all: f1 (argument and result) would be the
  first, in a register a host function preserves the low half of (v8 to
  v15), which the vector pool would have to give up.
- **A kernel call stores and loads every shared register.** Narrowing
  that to what a host function can read (not r28 to r31, unless it
  captures or unwinds a context) would take most of the cost back; it
  needs each such host function found first.
- **No FPCR write at all in functions without VMX float code.** Section
  1.15 removes the writes a block can inherit from the blocks before it.
  What is left: the first float block after a function's entry, after
  each call, and at the top of each loop. All three go if "FPCR holds the
  scalar value at every call, return and function entry" is made a rule
  for all translated code (the thunks already establish it on the way in
  and after a host call): a function with no VMX float operation then
  never writes FPCR, and one with them restores the scalar value before
  it calls or returns. It has to know, before it emits a function's first
  block, whether the function uses VMX mode anywhere, and at each exit
  whether the mode may be VMX. Two ways: emit such functions twice (the
  first pass records what each block does to the mode, as section 1.15's
  records do; costs translation time), or have each sequence declare the
  mode it needs so the HIR can be scanned (fifteen places call
  `ChangeFpcrMode(Fpu)`, one helper `EmitWithVmxFpcr`). It must be a
  start-up setting (all code agrees or none), and the verify mode of
  section 1.15 is the check for it. A loop's first block can also be
  given a known mode without the rule, by setting the mode on the way
  into the loop (a stub on the fall-through edge) when its back edges
  agree; that is local and smaller.
- **Scalar float still tests every result** (two instructions per
  operation). The study's R8 (PowerPC's choice of NaN as a setting) is
  untouched.
- **A callee found to return a flag after it was translated** is reported
  by the ABI flag guard, not repaired (section 1.14, 5a C).
- **`lwz`, `lbz` and `lhz` into a pinned register take two instructions**
  (the load into a temporary, the extension into the pin): the extension
  is planned as the value computed into the pin, so the load does not
  get the pin's register. `PlanPinHomes` would have to take the load for
  the definition when the stored value is an elidable copy of it. Tried
  in the second round (a dozen lines there) and taken out again: it
  passed the tests, and moved `seq_cc_int` from 429,374 to 429,079 and
  the bench's copy loop not at all (its loads go to registers that are
  not pinned). Not worth a change to settings that have been through the
  whole matrix; worth another look once a device profile shows where
  loads into pinned registers are hot.
- **The matrix's `#16k` lines still run in processes of 1,500 tests.**
  In the container the whole corpus now runs at 16 KiB pages in single
  processes without the abort (everything on, with the frame chain:
  141,959 of 141,959), so section 0's fix does make the split
  unnecessary here; I left the job as it is rather than change it on the
  container's evidence alone. Dropping `line_max` for `#16k` lines in
  `ci/run-matrix.sh` would save the job about a quarter of an hour, and
  the retry in `run-tests.sh` is still there if the runner's qemu
  differs.
- **Upstream's `mtfsfi`** reads its immediate shifted by one bit (section
  1.15, last paragraph). Not this branch's to change.

## 5a. Handover: the pieces that would finish the job

Written for whoever picks this up (the main thread or another cloud
session). Each can be done in a cloud session's container; none needs the
phone to write or to test, only to judge at the end. A to D are all done. Cut a branch from
`claude/translator-overhaul`.

### A. A `longjmp` test: done (section 1.12)

### B. Calls without the `longjmp` tax: done (section 1.13)

Done differently from what was sketched here: one step instead of two, and
x29 instead of a head pointer in the backend context, which makes the
record cost three instructions on entry and none on exit. What is left of
a call's fixed cost after it: the indirection-table load and compare of a
return, the return-address constant for the callee, and the write-back and
reload of pinned registers round the call, which is D.

### C. The ABI flag level, guarded: done (section 1.14)

Left of it: retranslating a callee found too late instead of only
reporting it (there is no way to retranslate a function today; the
indirection table entry could be pointed back at the resolve thunk, but
linked call sites and callers that hold the old address would have to be
found), and the entry scan following conditional branches.

### D. One register mapping for all translated code: done (section 1.16)

Smaller than sketched here: the pinning machinery already had everything
a shared register needs except "never synchronise this one", so it is a
flag on a pinned slot, three thunks and a wider pool, not a second
allocator. A, B, C and D are all done; what is left is in section 5.

## 6. For `HANDOFF.md` and `ARCHITECTURE.md`

Not edited by this session. To copy in at the merge.

For `HANDOFF.md`, "Right now": the translator tests' failure on
`claude/integration-1115` was qemu's (section 0), fixed in the test build;
the settings of section 4 and their order; the figures of section 2 with
the run link; that nothing has run on a device. Also: three behaviours
changed at the defaults in the second round, all corrections (section
1.12): a small function that writes the link register is no longer
inlined, a `longjmp` resumes on the landing function's own host frame
where the call records confirm it, and (with debug information only) the
stack check after a call sits where a `longjmp` resumes. If a title that
worked stops working after this merge, `a64_longjmp_resume_live_frame =
false` and `inline_leaf_keeps_lr_writers_out = false` give the earlier
behaviour. The third round changed the defaults' code in one way that
changes no behaviour: five sequences (two atomic compare-and-exchange,
three VMX) use other temporary registers.

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
| (second round) `cpu/raw_module.h` | return sites for raw modules (`MarkReturnSite`, `IsReturnSite`) |
| `cpu/ppc/ppc_hir_builder.cc` | raw modules' return sites marked; `inline_leaf_keeps_lr_writers_out`; the function's address handed to the HIR builder |
| `cpu/hir/hir_builder.h` | `guest_function_address` |
| `cpu/backend/a64/a64_backend.cc`, `.h` | `FindLandingStackpointDepth` and `a64_longjmp_resume_live_frame`; `a64_frame_chain`, `a64_frame_chain_verify`: `FindLandingFrame`, `EmitFrameChainResumeHelper`, x29 zeroed in the host-to-guest thunk and stored by the resolve thunk, three fields in `A64BackendContext`, no stackpoint array with the chain |
| `cpu/backend/a64/a64_emitter.h`, `.cc` | the frame chain's prologue, `EmitRestoreHostReturn`, `EmitLoadGuestReturnAddress`; no stackpoint push, pop or check with it; the check after a call steps over HIR comments |
| `cpu/compiler/passes/dead_context_store_pass.cc` | `dead_context_stores_abi_guard`: `AbiGuard`, `GuestCodeReadsFlagsAtEntry`, `FindFlagReadsAfterCalls` |
| `cpu/compiler/compiler.cc` | the overhaul's counters in the `TranslationStats` line |
| `cpu/backend/a64/a64_emitter.cc` (again) | `a64_fpcr_known_block_entry`, `_verify`: the block loop's edge records |
| `cpu/ppc/testing/seq_fpcr_blocks.s` | new tests |
| (third round) `cpu/backend/a64/a64_emitter.h`, `.cc` | `a64_scratch_gpr_pool` (the map of eighteen, the bare-call saves); `a64_shared_guest_registers` (`SetSharedGprs`, `SharedHostReg`, `EmitSharedGprsToContext`, `EmitSharedGprsFromContext`, shared pins without a mask bit, r1 read from its register) |
| `cpu/backend/a64/a64_backend.cc` | the two settings; the thunks' stores and loads of the shared registers; x4 to x7 saved by the guest-to-host thunk; the stack helper's registers; `NoteUnconfirmedLanding` and `a64_longjmp_unconfirmed_is_mismatch` |
| `cpu/backend/a64/a64_seq_memory.cc`, `a64_seq_vector.cc` | five sequences use x8, x16, x17 where they used x4 to x6 |
| `cpu/backend/machine_info.h`, `cpu/hir/hir_builder.h` | `shared_gprs`; `PinnedContextSlot::shared` |
| `cpu/compiler/passes/guest_register_pinning_pass.h`, `.cc` | `AddShared`; shared registers kept out of a function's own pins; up to fourteen pins of eighteen |
| `boosted/bench/core/bench_xlat.cc`, `tools/xlat-count-qemu.sh`, `run-core.sh` | the `host_args` program; rows 14 to 17 |
| `cpu/ppc/testing/seq_longjmp.s`, `seq_flags_across_blocks.s`, `ci/known-failures.txt`, `ci/matrix.txt` | new tests and lines |

Outside the core: `boosted/bench/core/bench_xlat.cc` (the counting mode),
`bench_env.cc` (section 0's fix, for runs under qemu at 16 KiB),
`run-core.sh` (the rows), `boosted/bench/tools/xlat-count-qemu.sh` (new),
`boosted/bench/CMakeLists.txt` (a definition CMake dropped).

## 7. Build state

- `b9b2c46` (section 0's fix alone): Actions run 138, **green, all three
  jobs**, the translator tests among them
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43649271608).
  Bench run 16 green.
- `cc22121` (the pool, coalescing, the compare forms, tight float): not
  pushed on its own. In the container: the whole corpus with the set on,
  with stress pinning and allocation from the top, with tight sequences
  and fused compares alone; the verify mode over the integer, float and
  vector tests.
- `0293269` (everything): bench run 21 green
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43903306476).
  Actions run 142
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43903307406):
  **green, all three jobs**: "Xbox core", "APK (both cores)" and the
  translator tests with every line of the matrix, the new ones included
  (2 hours 25 minutes for that job).

- The second round (the `longjmp` test and its corrections, the frame
  chain, the ABI flag guard, the counters): pushed after this text was
  written; the run links and results are in the last paragraph of this
  section once known. In the container before the push, on that tree:
  - defaults, all 141,948 tests then: pass, and every state equal to the
    first round's reference apart from the new tests (so the three
    default-on corrections change no test's result);
  - `seq_longjmp` under each of the 57 lines the matrix had: 5 of 5 (it had
    five cases then; eight now, run under the lines below);
  - `a64_frame_chain` over everything: 141,951 of 141,951;
    `a64_frame_chain_verify` over the seq_ suites and five instruction
    groups: no mismatch; the whole set of settings with the chain over
    everything: 141,950 of 141,950, and with verify over a subset: no
    mismatch; the seq_longjmp and flag suites with everything and the
    chain at 16 KiB pages: pass;
  - "overhaul, ABI flags" with the guard on (its default) over everything
    that line runs: 124,824 of 124,824; the pointer line and the ABI level
    with the chain: pass;
  - the deliberate faults of section 3's table.
- FPCR known at block entry (section 1.15), committed after that with
  `[skip ci]` so as not to cancel the run above; it is tested by the next
  run that is not skipped. In the container: all 141,960 tests with the
  setting, and again with its verify mode: pass, no mismatch; the verify
  mode alone and with the whole set and the frame chain over subsets: no
  mismatch.
- The byte and halfword extension (`ac2b0d3`): in the container, all
  tests with the whole set, the frame chain and the FPCR setting, and all
  with `register_allocation_elide_copies` alone: pass.
- Actions run 143 (on `e681c3a`: the `longjmp` corrections, the frame
  chain, the guard) had built the core and the APK and was halfway
  through the translator tests when the push after it replaced it with a
  run on the whole second round, so that one run covers everything.
- **Actions run 144 on `e4e160e` (the whole second round): green, all
  three jobs**: "Xbox core", "APK (both cores)" and the translator tests
  with every line of the matrix, the second round's among them (2 hours
  40 minutes for that job)
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43960085196).
  Bench run 23 on the same commit: green.
- Bench run 22 (the second round's commit `e681c3a`): green
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43947299227).
  Its row "overhaul 13: 8 + frame chain" has calls at 2.37 host
  instructions per guest instruction, the figure counted here.

- The third round (section 1.16), pushed after this text was written; the
  run's result is in the last lines of this section once known. In the
  container before the push: the new lines of `ci/matrix.txt` through
  `ci/run-matrix.sh`, all passing ("overhaul 18, shared 4" over
  everything: 141,961 of 141,961; six shared registers alone and
  eighteen registers alone over everything as well); the shared set with
  stress pinning, with each verify mode, with the stackpoint array
  instead of the frame chain, at the ABI flag level and at 16 KiB pages:
  pass; the bench's seven programs with their checks at the defaults and
  with the set; the four deliberate faults of section 3's table.

In the container before the push (x86-64, tests cross-compiled for ARM64
and run under qemu-aarch64 8.2), on the tree of `0293269`: every new line
of `ci/matrix.txt` at least once; the four lines over all the tests
("overhaul, everything" 141,942 of 141,942, "overhaul, stress, preempt"
141,942, "overhaul, ABI flags" 124,812 of the 124,812 that level runs,
"overhaul, everything, 16k" 141,942, with no process needing a second go), each with every test's state
equal to the reference's; the deliberate faults of section 3; and every
changed source file compiled with the compiler's default warnings as
errors (clean, apart from warnings in upstream's `a64_op.h` that were
there before).

That the defaults translate the same code as before was checked two ways:
every test's state with nothing set equals the state the tests left on
`claude/integration-1115`'s tree (141,882 tests in common; the two that
read the time base differ, as `ci/nondeterministic.txt` says they do), and
the bench's
integer loop with nothing set disassembles to the same instructions as
before this branch (addresses apart).

# The PowerPC to ARM64 translator: study, ranked rewrites, prototypes

Branch `claude/cpu-translation`, from `claude/xbox360-emulator-architecture-eac8l7`
at `5f8a0e9` (code as `7648e9c`). Written overnight 2026-11-12 to 13 by a cloud
session. Nothing here has run on a device. No speed-up is claimed anywhere in
this file: every "saves" below is a count of instructions or memory operations
read off the emitter's source, not a measurement.

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless they
start with another directory.

## Done / in progress / next

Done:
- The study (sections 1 to 3).
- The ranked list of rewrites, with a design for each of the large ones
  (section 4).
- Five settings, all off by default, in code (section 5): four change what is
  emitted, one measures.
- Local syntax check of every changed file (clang, arm64 target, host
  headers). The CI build is the real check: see "Build state" at the end.

In progress: nothing half-written is on the branch. Every commit is a
complete step.

Next, for whoever picks this up (in order):
1. A device run with `log_translation_stats = true` alone, to get the two
   tables section 6 asks for. They decide which of the rewrites to start.
2. The PowerPC instruction tests in CI (section 7, first item). Without them
   no rewrite of the register model can be checked short of a device.
3. Rewrite R1 (guest registers pinned in host registers), in the steps of
   section 4.

## 1. How a guest function becomes ARM64

One guest function at a time, on the guest thread that first calls it.

1. **Miss.** Every call site that did not know its callee's host address when
   it was emitted loads a 32-bit entry from the indirection table
   (`backend/code_cache_base.h`, `AddIndirection64`, one slot per guest
   instruction address from 0x80000000). An untranslated address holds the
   resolve thunk (`A64HelperEmitter::EmitResolveFunctionThunk`,
   `backend/a64/a64_backend.cc`), which calls `ResolveFunction` (same file),
   then `Processor::ResolveFunction` (`cpu/processor.cc`), which takes the
   entry in `entry_table_` and calls `DemandFunction`, then
   `PPCFrontend::DefineFunction` (`cpu/ppc/ppc_frontend.cc`). That takes a
   `PPCTranslator` from a pool, so several guest threads translate at once.
2. **Scan.** `PPCScanner::Scan` (`cpu/ppc/ppc_scanner.cc`) walks from the
   entry to find the function's end (the last `blr` not jumped over) and its
   blocks.
3. **Front end.** `PPCHIRBuilder::Emit` (`cpu/ppc/ppc_hir_builder.cc`) walks
   every instruction from start to end address, looks up its emitter
   (`ppc_emit_alu.cc`, `_fpu.cc`, `_altivec.cc`, `_memory.cc`,
   `_control.cc`) and appends HIR. Every guest register read is a
   `LoadContext` at a fixed offset in `PPCContext` (`ppc_context.h`), every
   write a `StoreContext` (`LoadGPR`, `StoreGPR`, `LoadCRField`, `StoreCA`
   and so on, same file). The front end knows nothing about what an earlier
   instruction left in a value: each PowerPC instruction starts from memory.
   Already here: calls to `__savegprlr_N`/`__restgprlr_N` and to straight-line
   leaves of up to 16 instructions are expanded inline
   (`TryInlineGprlrSaverest` in `ppc_emit_control.cc`, `TryInlineLeafCall`).
4. **Passes**, in the order `PPCTranslator::PPCTranslator`
   (`cpu/ppc/ppc_translator.cc`) adds them:
   - `ControlFlowAnalysisPass`, `ControlFlowSimplificationPass`: edges, dead
     blocks removed, a block merged into its only predecessor when that
     predecessor does not end in a volatile instruction.
   - `PreemptCheckInjectionPass`: scheduler safepoints.
   - `ContextPromotionPass`: **per block**, forwards a stored or loaded
     context value to later loads of the same slot and removes a store that a
     later store in the same block overwrites. Any volatile instruction
     (every call, trap, context barrier) forgets everything.
   - A group that repeats until neither member changes anything
     (`ConditionalGroupPass::Run`): `SimplificationPass` (five separate walks
     of the whole function: `SimplifyBitArith`, `EliminateConversions`,
     `SimplifyAssignments`, `SimplifyBasicArith`, `SimplifyVectorOps`) and
     `ConstantPropagationPass` (one walk).
   - `SpinLoopBackoffPass`, `DelayCountdownCollapsePass`,
     `MemoryPollParkPass`: guest spin and poll loops.
   - `MemorySequenceCombinationPass`: folds a byte swap into the load or
     store beside it (`LOAD_STORE_BYTE_SWAP`).
   - `SimplificationPass` once more, `DeadCodeEliminationPass`.
   - `RegisterAllocationPass` (`compiler/passes/register_allocation_pass.cc`):
     **per block**. Its own comment: "Registers do not move across blocks".
     It resets at each block (`PrepareBlockState`), assigns the first free
     register of the set, and when none is free spills the value whose next
     use is furthest (`SpillOneRegister`) to a stack local.
   - `FinalizationPass`.
   `DataFlowAnalysisPass` and `ValueReductionPass` exist in
   `compiler/passes/` and are not in the list.
5. **Back end.** `A64Assembler::Assemble` then `A64Emitter::Emit`
   (`backend/a64/a64_emitter.cc`): prologue, then for each HIR instruction
   `SelectSequence` picks an emitter from the tables in `a64_sequences.cc`,
   `a64_seq_memory.cc`, `a64_seq_vector.cc`, `a64_seq_control.cc`; then the
   epilogue and the out-of-line tail code. `Emplace` copies the code into the
   cache (`PlaceGuestCode`), which writes its DWARF unwind entry
   (`PosixA64CodeCache::InitializeUnwindEntry`) and points the function's
   indirection slot at it.

The register model the back end works to (`a64_emitter.cc`, `gpr_reg_map_`,
`vec_reg_map_`; `a64_backend.cc`, `Initialize`):

| Host | Use |
|---|---|
| x19 | `A64BackendContext*` (sits just below the `PPCContext`) |
| x20 | `PPCContext*` |
| x21 | guest memory base |
| x22 to x28 | the 7 registers the allocator hands to integer HIR values |
| x0 to x17 | scratch inside a sequence, free between sequences |
| v4 to v31 | the 28 registers for float and vector HIR values |
| v0 to v3 | scratch |

No guest register lives in a host register from one block to the next.

## 2. What it emits for the commonest things

Counts are instructions executed on the usual path, read from the emitters.
"Far form" means `A64Emitter::b`, `cbz`, `cbnz`, `tbz`, `tbnz`
(`a64_emitter.cc`): each emits the inverted branch over an unconditional `b`,
two instructions, so that a target more than 1 MiB (or 32 KiB for `tbz`) away
still encodes. Every conditional branch in translated code is in this form,
so a guest branch that falls through costs a taken host branch.

**Context traffic.** A block that uses guest register rN loads it from
`[x20 + offset]` at first use and stores it back if it changed
(`LOAD_CONTEXT_*`, `STORE_CONTEXT_*` in `a64_sequences.cc`: one `ldr` or
`str`). A store is removed only when the same block overwrites the slot
later. So a loop of one block that increments a counter and tests it does a
load, the add, a store, and the compare's stores, every time round, however
many of the 7 registers are free.

**Integer arithmetic.** `add`, `sub`, logic and shifts are one host
instruction each once their operands are in registers. `rlwinm` and friends
arrive as rotate, and, or chains that `SimplificationPass`
(`TryHandleANDROLORSHLSeq`) reduces. Carry is a separate compare
(`AddDidCarry` in `ppc_emit_alu.cc`: truncate both, `not`, unsigned compare)
and a byte store to `xer_ca`: the host's own carry flag is never used.
`addc`/`adde` chains therefore cost about five instructions and a byte store
per link instead of `adds`/`adcs`.

**Condition register.** One byte per bit, 32 bytes (`ppc_context.h`, `cr0`
to `cr7`). `cmpw` is `PPCHIRBuilder::UpdateCR`: three HIR compares (lt, gt,
eq), each emitted as its own `cmp` + `cset` (`DEFINE_COMPARE_XX` in
`a64_sequences.cc`), each stored with `strb`: 9 instructions. The `beq` that
follows reads the byte back through context promotion as a value and branches
with `cbnz` in far form: 2 more. A native `cmp` + `b.eq` is 2. The two
compares whose result the branch does not use are still computed and stored,
because the block ends at the branch and the stores are only dead if a later
block overwrites them, which nothing looks for.
`mfcr`/`mtcr` build or scatter all 32 bytes (`LoadCR`, `StoreCR`).

**XER.** `xer_ca`, `xer_ov`, `xer_so` are bytes. Overflow (`OE` forms) is not
implemented (`XEINSTRNOTIMPLEMENTED` in `InstrEmit_addx` and the rest).

**Memory access.** The guest's 4 GiB is mapped at x21, so an access is
`[x21 + zero-extended address]`. `ComputeMemoryAddress` (`a64_seq_util.h`):
- `mov w0, wADDR` to clear the upper half;
- on a host whose pages are larger than 4 KiB (this phone: 16 KiB), four
  more: `mov w17, #0xE0000000; cmp; cset; add w0, w0, w17, lsl #12`, because
  guest addresses from 0xE0000000 sit 0x1000 further on in the host mapping
  and that cannot be done with page tables at 16 KiB.
`LOAD_I32` then `ldr w, [x21, x0]` and `rev` when the byte swap was folded
in. So `lwz r, d(rA)` is 8 instructions here (the five above, the offset add
in `EmitGuestOffsetAdd`, the load, the swap) against 3 on a 4 KiB host. A
constant address is `mov; ldr; rev`.

**Faults and MMIO.** No bounds check: a wild access faults. Accesses to the
GPU's register range are caught by the signal handler
(`MMIOHandler`, `cpu/mmio_handler.cc`), which handles the access and records
the guest instruction in the per-title info cache
(`A64Backend::RecordMMIOExceptionForGuestInstruction`, flag `accessed_mmio`
in `InfoCacheFlags`, `cpu/xex_module.h`, a file per executable under the
cache directory). The next translation of that instruction calls a C helper
through the thunk instead (`IsPossibleMMIOInstruction`, `MMIOAwareLoad`,
`a64_seq_memory.cc`). `emit_inline_mmio_checks` (off) would add a range check
to every 32-bit access.

**Byte swapping.** `rev`/`rev16` after loads and before stores, folded by
`MemorySequenceCombinationPass`; a swap of a swap is cancelled
(`SimplificationPass::CheckByteSwap`). A value loaded, tested against zero
and discarded is still swapped. Vectors are swapped on load and store with a
table permute.

**Scalar float.** `EmitFpBinOpWithPpcNan_F64` (`a64_sequences.cc`) for each
add, subtract, multiply, divide: `fcmp; fccmp; branch (far form); op; fcmp;
branch (far form)`, 7 instructions executed and two taken branches round one
`fadd`, to give PowerPC's choice of NaN. Before the first float operation of every
block the emitter writes FPCR (`ChangeFpcrMode`: `ldr; msr`), because
`ForgetFpcrMode` runs at each block start; a block that mixes scalar and
vector float writes it at each change of kind.

**VMX.** Almost everything is inline NEON (`a64_seq_vector.cc`, and the
`_V128` sequences in `a64_sequences.cc`). Each float operation
(`EmitVmxFpBinOp_V128`, `a64_seq_util.h`) is: FPCR to the vector mode if not
known, flush denormal inputs by hand unless the core's FZ does it
(`kA64FZFlushesInputs`), the operation into v2, the NaN fix-up
(`a64_vmx_nan_fixup`, on), flush denormal outputs, move to the destination.
`vexptefp`, `vlogefp` go to C helpers through the thunk (`POW2_V128`,
`LOG2_V128`). `vrsqrtefp` (`RSQRT_V128`) calls a C function four times with a
bare `blr`, see section 8.

**Guest function prologue and epilogue** (`A64Emitter::Emit`,
`PushStackpoint`, `PopStackpoint`): `sub sp`, three stores (host return
address, guest return address, zero), then the stackpoint push for `longjmp`
recovery: 2 loads, a multiply, 4 stores, 2 loads from the context, an
increment, an overflow compare and branch: 22 instructions in, 6 out.

**Guest to guest call** (`A64Emitter::Call`): if the callee was already
translated when the caller was emitted, `mov x9, #host; ldr x0; blr x9`.
Otherwise, and a caller is usually translated before its callees, the call
site keeps the table lookup for good (nothing patches it later): on Android
the table is "encoded" (`encoded_indirection`), so `mov w16, #guest; mov x14,
#bias; add; ldr w9; tbnz (far form); mov x14, #code base; add x9; b; ldr x0;
blr x9`, about 14 instructions with two taken branches before the call
itself. Before it, `SET_RETURN_ADDRESS` (`mov; str`) and the store of LR.
After it, `EnsureSynchronizedGuestAndHostStack`: a load and a far-form
`cbz`, 3 instructions.

**Return** (`blr` is a `CALL_INDIRECT` with `CALL_POSSIBLE_RETURN`,
`A64Emitter::CallIndirect`): load LR from the context, compare it with the
return address the caller passed in x0 and the prologue saved, far-form
branch to the epilogue. If it does not match, a table lookup and a tail
jump.

A call and return between two guest functions is therefore about 55 host
instructions and about 20 memory operations more than the guest's own `bl`
and `blr`.

**Indirect calls** (`bctrl`, virtual calls): the table lookup above with the
address in a register.

**Kernel calls.** An import is a 16-byte guest stub the loader rewrites to
`sc 2; blr` (`XexModule::SetupLibraryImports`, `cpu/xex_module.cc`) and marks
`kExtern`. Calling it is a full guest call to the stub (lookup, prologue,
stackpoint), then `CALL_EXTERN` (`A64Emitter::CallExtern`): `mov x0,
#handler; ldr x1, kernel_state; mov x9, #thunk; blr x9`, then the stub's
`blr` and epilogue.

**The guest to host thunk** (`EmitGuestToHostThunk`): saves q4 to q31 (14
`stp` of 32 bytes) and x29/x30, calls the host function with the context in
x0, reloads FPCR and writes it, restores the 28 vector registers, returns.
About 37 instructions, 464 bytes written and read back, an FPCR write. It
saves the vector registers because a sequence in the middle of a block may
call a helper while values are live in them. It does not save x19 to x28: the
host function preserves those itself (callee-saved under AAPCS64).
**Which registers truly need saving** is therefore a property of the call
site, not of the thunk:
- `CALL_EXTERN`: none. It is always the last instruction of its block
  (`HIRBuilder::CallExtern` ends the block, and
  `ControlFlowSimplificationPass` never merges across a volatile tail), HIR
  values never outlive their block (the allocator asserts it), and the guest
  caller of the stub has nothing live across a guest call, since translated
  functions use v4 to v31 and x22 to x28 without saving them.
- The preempt check (`EmitPreemptCheck`): none either, by its own comment
  (block head, nothing live), but it shares the full thunk.
- A helper called from inside a sequence (MMIO access, `POW2`, `LOG2`,
  reserved load and store, tracing): exactly the vector registers holding
  values whose last use is after this instruction. The allocator knows that
  set; the emitter is not told. Passing it (a 28-bit mask on the instruction)
  would let the call site save only those.

## 3. Where it leaves performance on the table

Against three translators that solve the same problem.

**Dolphin, JitArm64** (GameCube/Wii, the same guest architecture family).
- Guest registers are cached in host registers for the length of a block and
  Dolphin's blocks are long: it follows unconditional branches and continues
  past conditional ones, flushing registers only on the exit path, in
  out-of-line code. Here the unit is the basic block and the flush is in
  line and unconditional.
- The condition register is kept as one 64-bit value per field, computed by
  a subtract, from which each bit is a cheap test; and its x86-64 JIT merges
  a compare with the conditional branch that follows into the host's
  compare and branch (I did not check how far the ARM64 one goes). Here:
  three compares, three byte stores, a reload.
- Carry is kept in the host's carry flag between an `addc` and the `adde`
  that follows. Here: a compare and a byte in memory each time.
- Blocks are linked: a branch to another block is patched to a direct host
  branch once the target exists. Here a call site that missed at emission
  goes through the table for ever.
- Accurate NaN handling is a setting that is off by default. Here it is
  always on for scalar float.
- Memory: fastmem with the fault handler patching the access to a slow path
  (as here), on a 4 KiB layout with no fix-up.
- Dolphin does not keep translated code between runs.

**RPCS3, the PPU recompiler** (PS3: 64-bit PowerPC with VMX, the closest
guest to this one).
- Ahead of time: the whole executable is analysed into functions and
  compiled by LLVM on all cores before the title starts, and the object
  code is cached on disk keyed by the executable's hash and the settings, so
  a second boot loads it. Here: translation on first call, on the guest
  thread, repeated every boot. The profile shows it: about 6% of GameThread
  in the passes.
- LLVM optimises a whole guest function at once: redundant loads and stores
  of guest registers and condition bits nothing reads are removed across
  blocks by a real compiler's passes, and the register allocator is global.
  (I did not re-read its source for how much guest state stays in the
  thread structure round calls.)
- Guest to guest calls are direct host calls with the thread pointer and
  the memory base in fixed registers.
- NaN fix-ups, FPCR-exact behaviour and the like are per-title settings,
  off by default.

**FEX** (x86-64 on ARM64, and the reason hakuX's own core got fast).
- Static register allocation: the guest's general registers and vector
  registers are each pinned to one host register everywhere, so nothing is
  loaded or stored at block boundaries or at guest calls; they are spilled
  only round calls into FEX's own C++.
- Flags are computed lazily: a deferred record of the last flag-setting
  operation, materialised only if something reads it, and dead flag writes
  are eliminated across blocks.
- Blocks are linked by patching direct branches; indirect branches go
  through a small per-thread cache before the full lookup.
- Multi-block compilation: a group of blocks reached from an entry is
  compiled together.
- It has work on caching translated code between runs; I did not check its
  current state and do not rely on it here.

Put against those, this translator's losses, largest first by my reading
(the device has to rank them properly, section 6):

1. **No guest register survives a block boundary.** Every block reloads and
   re-stores. On the audio mixer the profile names (`sub_82C62548`, tight
   float and vector loops) this is the whole loop overhead apart from the
   arithmetic.
2. **Condition register and carry in memory, eagerly.** About 11
   instructions per compare-and-branch against 2.
3. **Calls.** About 55 instructions and 20 memory operations per call and
   return; the table lookup at most call sites; a full guest frame round a
   kernel call; 56 vector saves round every host call.
4. **The 16 KiB address fix-up**: four instructions on every computed load
   and store.
5. **Float**: NaN handling on every scalar operation, FPCR writes at every
   block that touches float, denormal flushes in software on cores whose FZ
   does not cover inputs.
6. **Far-form branches** everywhere.
7. **Translation at run time, every run**, and its cost (section 6 has what
   the passes cost and why `SimplificationPass` leads).
8. **Code size**: all of the above is also why the cache reached 128 MiB in
   two minutes. Each of 1 to 6 shrinks it.

## 4. Ranked rewrites

Ranked by expected gain over risk, with what each needs first. "Measure" is
always: the same scene of Fable II (and one other title), `device-watch.py
fps`, and a simpleperf run of the six dispatch threads as in the 200 s
profile, comparing the share of guest code and the named symbols.

### R0. A way to test the translator without the phone (do first)

Not a speed-up; the condition for every rewrite below.
`cpu/ppc/testing/` holds 576 PowerPC instruction tests with expected
register values and a runner (`xenia-cpu-ppc-tests`) that links the a64 back
end; `cpu/testing/` holds HIR-level back end tests. Both are behind
`XENIA_BUILD_TESTS` (off). GitHub's `ubuntu-24.04-arm` runners are arm64
Linux: a job that configures the core with `-DXENIA_BUILD_TESTS=ON`, builds
those two targets and runs them would check every change to the passes and
the emitter in minutes, with each new setting both off and on.
Risk: none to the app. Cost: the workflow and whatever the Linux arm64
build of the core needs (it is the Android code path minus the JNI layer;
`XE_PLATFORM_xendroid` is assumed in `a64_code_cache_posix.cc`, so some
conditionals will need care). The workflow is not this branch's to edit.

### R1. Guest registers in host registers across blocks and calls

Expected gain: the largest. It removes most `LOAD_CONTEXT`/`STORE_CONTEXT`
pairs (item 1 above) and is what makes R2 worth having. I expect it to cut
the instruction count of integer-heavy guest code by a third or more; that
is an estimate from reading emitted sequences, not a measurement.

Design: FEX's static mapping, for a subset. All translated code agrees that
certain guest registers live in certain host registers, always. Nothing is
loaded or stored at block ends or at guest calls. The `PPCContext` copy of a
pinned register is stale while guest code runs and is brought up to date
only where host code can look.

Exact structures:

```cpp
// backend/a64/a64_pinned_regs.h (new)
struct PinnedGuestReg {
  uint16_t context_offset;  // offsetof(PPCContext, r[n])
  uint8_t  host_reg;        // x22..x28
};
// Chosen once, at build time, from the table section 6 produces.
// First cut: r1 (stack), r3, r4, r5 (arguments, results), r11, r31, r30.
inline constexpr PinnedGuestReg kPinnedGprs[] = { ... 7 entries ... };
// Bitmap of PPCContext GPR indices that are pinned, for the front end.
inline constexpr uint32_t kPinnedGprMask = ...;
```

- Temporaries move off x22 to x28 onto caller-saved registers: the
  allocator's integer set becomes x9 to x15 (`gpr_reg_map_`). That is sound
  for guest calls already (nothing is live across them). It is not sound
  today for helper calls inside a sequence, so the full thunk gains
  `stp`/`ldp` of x9 to x15 (4 pairs), and every sequence that uses x9 to x15
  as scratch must move to x0 to x8, x16, x17. That audit is the bulk of the
  work: about 175 uses across `a64_sequences.cc`, `a64_seq_memory.cc`,
  `a64_seq_vector.cc`, `a64_seq_util.h`, plus `Call`, `CallIndirect`,
  `PushStackpoint` and the thunks in `a64_emitter.cc`/`a64_backend.cc`.
- HIR: two new opcodes, `OPCODE_LOAD_PINNED` and `OPCODE_STORE_PINNED`
  (`hir/opcodes.inl`), operand = index into `kPinnedGprs`. `PPCHIRBuilder::
  LoadGPR`/`StoreGPR` emit them for pinned registers when the setting is on.
  `ContextPromotionPass` treats them like context slots, except that a
  volatile instruction does not invalidate them: guest calls preserve the
  mapping (the callee may change the register, so a *value* known before a
  call is forgotten, but no store is needed before it).
- Emitter: `LOAD_PINNED` is `mov dest, xPIN` (or nothing, once the allocator
  is taught to treat the pinned register as the value's home while no
  `STORE_PINNED` to it intervenes); `STORE_PINNED` is `mov xPIN, src`.
- Host boundary: `EmitHostToGuestThunk` loads the pinned set from the
  context after setting x20 and stores it back before returning;
  `EmitGuestToHostThunk` stores it before the host call and reloads it
  after (7 stores and 7 loads, against the 56 vector operations it does
  today). The resolve thunk likewise, since `ResolveFunction` reads
  `guest_context->r[1]`. Any host code that reads or writes a guest GPR from
  a signal handler while guest code is interrupted (the debugger, the stack
  walker, `XThread` suspension and the cooperative scheduler's fiber switch
  if it switches from a signal) must go through the saved host register
  state instead. The fiber switch at a safepoint goes through the thunk, so
  it is covered.
- `PushStackpoint` reads `r[1]` and `lr` from the context: it must read the
  pinned register instead.
- Vector and float registers: the same scheme with v8 to v15 later; they are
  callee-saved only in their low halves, so the thunk must still save them.
  Start with GPRs only.

Steps, each buildable and testable alone: (a) move the temporaries to x9 to
x15 with nothing pinned, behind a setting, and prove it with R0's tests;
(b) pin r1 only; (c) the rest.
Risk: high. A missed scratch use or a missed host boundary corrupts a guest
register silently. This is not to be attempted without R0.
Measure: instruction count of a dumped function (`dump_functions_at`) before
and after; the guest share of the profile; fps.

A lesser version, if R1 is judged too large: keep the per-block model but
flush only on exit paths and extend blocks along fall-through edges
(Dolphin's shape). Less gain, since guest functions are short and calls
remain boundaries, and nearly the same allocator work.

### R2. Compare and branch without the condition register; carry in the flag

Expected gain: large on branchy integer code (item 2). About 9 of every 11
instructions of a compare-and-branch.
Design, in two independent parts:
- In `SelectSequence` (`a64_sequences.cc`), a pattern over HIR: a
  `COMPARE_*` whose result's uses are a `STORE_CONTEXT` to a CR byte and a
  `BRANCH_TRUE`/`BRANCH_FALSE` later in the same block, with no instruction
  between that changes the operands' registers or the host flags, emits `cmp`
  once, the `cset` + `strb` for each CR bit that is not dead, and `b.cond`
  for the branch. `SelectSequence` already returns a `new_tail`, so a
  sequence may consume more than one instruction; nothing uses that yet.
- A function-wide dead CR store pass: a backward liveness over the CFG for
  the 32 CR bytes and `xer_ca` (`DataFlowAnalysisPass` already computes
  liveness for locals and is unused; this is its smaller cousin). A CR byte
  is live out of the function at every call and return unless the callee is
  known. PowerPC's ABI says cr0, cr1, cr5 to cr7 are volatile across calls,
  but a hand-written leaf may break that, so a title-level setting.
- Carry: `OPCODE_ADD_CARRY` already exists for `adde`; pair a preceding
  `addc` in the same block to `adds`/`adcs` in the emitter.
Risk: medium. The matcher is local and testable with R0; the liveness pass
is the part that can be wrong.
Measure: as R1.

### R3. Cheaper guest to guest calls

Expected gain: medium to large on call-heavy code; the profile's host-call
and call overhead on GameThread.
Parts, smallest first:
1. (prototype, in this branch) `a64_compact_stackpoint_push`.
2. **Link call sites.** When `Call` is emitted for a callee that has no code
   yet, emit `mov w16, #guest; ldr x9, <literal>; ldr x0, [sp, #56]; blr x9`
   with an 8-byte literal in the function's tail code that starts as the
   resolve thunk's address (the thunk reads the guest address from w16, as
   now). When the callee is placed
   (`OnCodePlaced`, `a64_code_cache.cc`), store the callee's address into
   every literal recorded for that guest address. A literal is data, so the
   patch is one aligned 8-byte store with no instruction cache flush and no
   need to stop other threads.
   ```cpp
   // backend/a64/a64_code_cache.h
   struct CallSiteLiteral { uint32_t guest_target; uint64_t* literal; };
   // per guest address, guarded by the cache's allocation mutex
   std::unordered_multimap<uint32_t, uint64_t*> pending_call_literals_;
   ```
   The code cache maps the same pages twice (execute and write views), so
   the store goes through the write view.
3. **Stackpoints only where a `longjmp` could land.** The push exists so
   that a `blr` to a `setjmp` return site can find the host frame. Only
   functions that contain a return site that some `longjmp` targets need a
   record; in practice, functions that call `setjmp`. The info cache already
   records `is_return_site` per address; it would need a separate "was the
   target of a non-returning jump" bit, which `ResolveFunction` (a64) sees
   when it takes the `is_return_site` path. With that recorded from earlier
   runs, a function with no such site could omit push, pop and the
   post-call check (about 30 instructions a call). The first run of a title
   has no record, so the full form must stay the default until the record
   exists. Risk: a title whose `longjmp` path was never exercised in an
   earlier run.
4. **Return without the compare**: a shadow return stack, as FEX and
   Dolphin do not need but QEMU-style translators use: push the host return
   address at call, `ret` on `blr` after one compare of the guest LR. The
   current scheme already is this with the compare in the callee; no change
   proposed.
Risk: 2 is low to medium (data patch, but a lifetime question: literals in
code that is never freed, which is the case today). 3 is medium.
Measure: `perf` share of the prologue symbols is not separable; use a
call-heavy function's instruction count and fps.

### R4. Cheaper host calls

Expected gain: the profile's `a64_host_code` share: 3.7% of the six threads,
6.8% of GameThread, and (earlier run) up to 20% of the busiest thread.
Parts:
1. (prototype) `a64_lean_extern_thunk`: no vector saves for `CALL_EXTERN`.
2. (prototype) `inline_extern_calls`: no guest frame round a kernel call.
3. The preempt check's yield through the lean thunk too (it is at a block
   head with nothing live, by its own comment). Not done here: its safety
   rests on a pass in another session's area keeping that promise.
4. A live-vector mask on instructions that call helpers mid-block, from the
   allocator, so those sites save only what is live.
5. FPCR: the thunk writes it after every host call. Read it first (`mrs`)
   and write only if it differs: identical semantics, and a read is cheaper
   than a write on cores where the write serialises. Not done here because
   whether it is a gain is a property of this core that only a measurement
   settles.
Risk: 1 and 2 low (argued in the code comments), 4 medium.
Measure: `a64_host_code` in the profile; section 5 says how for each
setting.

### R5. Translate ahead of first use, on spare cores

Expected gain: removes translation from the guest threads during play (about
6% of GameThread in the first minutes) and the hitches when a new area's
code is first reached. No steady-state gain.
What is already there: the info cache
(`XexInfoCache`, `cpu/xex_module.cc`) keeps, per executable hash, a
`was_resolved` bit for every address ever resolved, across runs.
`XexModule::PrecompileKnownFunctions` translates all of them, but on the
loading thread, serially, and only under `enable_early_precompilation`
(off, described as a stress test).
Design:
```cpp
// cpu/xex_module.cc
void XexModule::PrecompileKnownFunctionsAsync();  // new
//   collects the was_resolved addresses into a vector, sorted by address,
//   and starts N worker threads (N = performance cores - 2, nice +10) that
//   each take the next address and call processor_->ResolveFunction(addr).
```
`Processor::ResolveFunction` is already safe to call from several threads
(the entry table hands exactly one caller `STATUS_NEW`; others wait) and the
translator pool gives each thread its own translator. A guest thread that
needs a function a worker is translating waits for it as it would for
another guest thread. The workers must stop at shutdown and when the module
unloads.
Caveats: `ResolveFunction` on a worker has no `ThreadState`; the a64
`ResolveFunction` wrapper is not involved (workers call the processor's).
Function extents come from the scanner at translation time, so a worker
translating an address that is not really a function start produces what a
guest call there would. Passes that consult guest memory (the spin and poll
passes read constants) see the memory as it is at that moment, which for
code is stable after load.
Risk: low to medium; the first-run case is unchanged.
Files: `cpu/xex_module.cc`, `cpu/processor.cc`: not this branch's.
Measure: count of `ResolveFunction` calls on guest threads after the first
second (a counter), the pass symbols' share of GameThread, and hitch counts
on entering a new area on the second run.

### R6. A translation cache kept between runs

Expected gain: the same as R5 for boot and first-visit hitches, without
spending the cores; nothing in steady state. After R5 the remaining gain is
only the workers' CPU time, so R5 first.
Why it is not a small job here: emitted code embeds absolute addresses that
differ from run to run:
- host function and thunk addresses (`mov x0, #handler`, `mov x9, #thunk`),
  which move with ASLR of `libe.so` and with code cache placement;
- the code cache base and indirection table bias (`mov x14, #...`);
- other functions' host addresses in direct calls;
- addresses of emulator data (`preempt_yield_handler`, vector constants,
  `Function*` for undefined externs).
Design:
```cpp
// backend/a64/a64_persistent_cache.h (new)
struct CacheFileHeader {
  uint32_t magic, version;
  uint8_t  executable_sha1[20];     // XexModule::image_sha_bytes_
  uint64_t settings_hash;           // every cvar that changes emitted code
  uint64_t emitter_build_id;        // changes with any backend change
  uint32_t function_count;
};
struct CachedFunction {
  uint32_t guest_address, guest_end_address;
  uint32_t code_size, stack_size, prolog_stack_alloc_offset;
  uint32_t reloc_count, source_map_count;
  // then: code bytes, CacheReloc[reloc_count], SourceMapEntry[...]
};
enum class RelocKind : uint8_t {
  kThunk,            // index: which thunk
  kHostFunction,     // index into a table of named helpers
  kKernelExport,     // library + ordinal
  kCodeCacheBase, kIndirectionBias, kExternalTableBase,
  kBackendData,      // named emulator data address
  kGuestFunction,    // guest address whose host code is called directly
};
struct CacheReloc { uint32_t code_offset; RelocKind kind; uint32_t index; };
```
Every `mov reg, #imm64` of such an address must go through one emitter
entry point that records the relocation and always emits the full four
instructions (`movz` + 3 `movk`) so it can be rewritten in place. Today
those are plain `mov(x9, reinterpret_cast<uint64_t>(...))` calls scattered
through the emitter and the sequences: about 60 sites. The table of named
helpers has to be complete; a missed one is a jump to a stale address.
Functions translated with per-run knowledge (the MMIO flags, anything a
pass read from guest data memory) must be keyed on that knowledge or left
out.
A cheaper cache that avoids relocations: cache the **HIR after the passes**
instead of the machine code. It holds no host addresses (symbols are guest
addresses), skips the scanner, the front end and all passes, and leaves only
the emitter to run. It needs a serialiser for `hir::Instr`/`Value`. By the
profile the passes are most of translation time, so this gets most of the
gain for a fraction of the risk.
Risk: machine-code cache high; HIR cache medium.
Measure: time from launch to first frame and to the menu, first and second
run; the pass symbols in the profile.

### R7. The address fix-up on 16 KiB hosts

Expected gain: four instructions off every computed load and store on this
phone; memory access is among the commonest things guest code does.
Options:
- Keep 0xE0000000 in a register for the whole function (one more reserved
  register, or x17 loaded once per block): fix-up becomes `cmp; cset; add`,
  three instead of five.
- Remove the need: the 0x1000 displacement exists because the 0xE0000000
  view of physical memory starts one 4 KiB page in. If guest physical
  memory were allocated so that view could be mapped at a 16 KiB-aligned
  host offset, no fix-up would be needed at all. That is `memory.cc`
  (nobody's overnight) and touches every `TranslateVirtual`.
- For `d(rA)` forms where rA is the stack pointer or was proven below
  0xE0000000 in this block already, skip it. Needs a range fact the HIR
  does not carry.
Risk: first option low, second high, third medium.
Measure: instruction count; fps.

### R8. Float

- Scalar NaN handling as a setting (like `a64_vmx_nan_fixup` for vectors),
  default on, so a title can turn it off: one `fadd` instead of eight
  instructions. Needs per-title testing; some titles compare float bits.
- Move the NaN path out of line (tail code) so the usual path is `fcmp;
  fccmp; b.vs tail; op` and the result check: same semantics, fewer taken
  branches.
- FPCR: carry the known mode across block boundaries when every
  predecessor ends in the same mode (the emitter forgets at each block
  start). Needs predecessor information in the emitter.
Risk: low to medium each. Measure: the audio mixer function's share.

### R9. Forward branches in short form

The prototype does backward branches only. For forward ones the distance is
unknown when the branch is emitted. Emit short and, if the assembler
reports the label too far when it is bound, emit the function again with
far forms. That needs emission to be repeatable (the emitter mutates no
HIR, as far as I read, but I did not verify every sequence).
Risk: low once repeatability is established. Gain: one instruction and one
taken branch per not-taken guest branch.

### R10. Translation cost itself

See section 6 for what to measure first. Candidates:
- `SimplificationPass::GetScalarNZM` (`simplification_pass.cc`) recomputes
  the known-zero mask of a value by walking its definition tree to depth 16
  with no memory, at both operands of every and, or, xor, add and compare,
  on every iteration of the group. A value used on both sides is walked
  twice at each level. A small table for the duration of one top-level call
  (value pointer and depth reached, reuse only if the stored answer was
  computed with at least as much depth left) returns the same or a tighter
  mask and makes the walk linear.
- The group reruns all six walks whenever anything changed; the last
  iteration always changes nothing. Folding the five `SimplificationPass`
  walks into one visit per instruction would cut memory traffic over the
  instruction list by a factor of about three per iteration. Each rule is
  local, so the order does not affect correctness, but the output can
  differ.
- `RegisterAllocationPass::Run` computes `preferred_reg` and never uses it:
  `has_preferred_reg` is set only if `preferred_reg.set` is non-null, and it
  is tested before it is assigned (see section 8).
Risk: low. Gain: only translation time.

## 5. The settings in this branch

All default to off, so merging changes nothing that runs. All take effect
for functions translated after they are set (set them in the config before
launch). Set one at a time when measuring.

| Setting | Section | What changes in emitted code | What to look for |
|---|---|---|---|
| `a64_lean_extern_thunk` | a64 | `CALL_EXTERN` calls a second thunk (`EmitGuestToHostLeanThunk`, `a64_backend.cc`) that saves x29/x30 only | simpleperf: the `a64_host_code` share should fall on the threads that make kernel calls (GameThread 6.8% in the 200 s profile). Any vector or float corruption after a kernel call means the argument in the thunk's comment is wrong somewhere: turn it off and report the title and scene |
| `inline_extern_calls` | CPU | `bl` to an import stub that still holds `sc 2; blr` becomes `StoreLR` + `CALL_EXTERN` in the caller (`PPCHIRBuilder::TryInlineExternCall`) | the same symbol, and guest code share; with `dump_functions_at` on a function that calls an import, the HIR shows `inlined import stub` and no `call` |
| `a64_short_backward_branches` | a64 | conditional branch to an already placed block label within 1 MiB (32 KiB for bit tests) is one instruction (`A64Emitter::IsNearBoundLabel`) | code size per function in a dump; no visible effect expected alone. A function that fails to translate with "assembler rejected" would mean the range test is wrong |
| `a64_compact_stackpoint_push` | a64 | stackpoint index by `add x8, x8, x9, lsl #4` instead of `mov; umull; add` | nothing visible alone; a `longjmp` title (any that uses `setjmp`) must still work |
| `log_translation_stats` | CPU | nothing | `xe.log`, a line starting `TranslationStats:` every 2048 functions |

`log_translation_stats` prints, cumulatively: functions compiled, total
milliseconds in the HIR passes, how many times the simplify/constprop group
ran and how many iterations that took in all, and milliseconds per pass in
pipeline order. The pass names are the compiler's mangled type names (for
example `N2xe3cpu8compiler6passes20ContextPromotionPassE`); the index is the
position in the list in section 1, where the group is one entry. It adds two
clock reads per pass per function while on.

Suggested order on the device: `log_translation_stats` alone (one run, for
the numbers); then `a64_lean_extern_thunk`; then that plus
`inline_extern_calls`; then all four.

## 6. What a device run must check, and the two tables to bring back

1. **With everything off**, the build behaves as `7648e9c`. The only code
   that runs differently with all settings off: the lean thunk is generated
   at start-up (a few dozen bytes of the code cache, never called), and each
   block label's offset is recorded in a map during emission.
2. **`log_translation_stats`**: bring back the last `TranslationStats:` line
   after five minutes of play. It answers: what fraction of translation is
   `SimplificationPass` (inside the group, entry 3 or so), how many
   iterations the group takes per function on average (iterations divided
   by runs), and whether register allocation or context promotion matters.
   If the average is near 3 and the group dominates, R10's first two items
   are worth doing.
3. **Each emitting setting, alone, then together**: boot to play in Fable II
   and one title that uses `setjmp`/`longjmp` (the stackpoint setting), one
   that is float heavy. Watch for a crash at the first kernel call
   (`a64_lean_extern_thunk`, `inline_extern_calls`), wrong float or vector
   results after a kernel call (lean thunk), a hang in a loop (short
   branches).
4. **simpleperf, 200 s, the six dispatch threads**, for off and for all on:
   compare `a64_host_code`, the guest code share, and fps.
5. A table I could not produce without a device and that R1 needs: which
   guest registers the hot code uses. From the same profile, take the ten
   hottest guest functions, dump them (`dump_functions_at`), and count
   `load_context`/`store_context` by offset in the optimised HIR. The
   offsets map to registers by `PPCContext` (`ppc_context.h`): r0 to r31
   are 8 bytes each from `offsetof(PPCContext, r)`. That table picks the
   seven registers to pin.

## 7. Changes needed in files this branch may not edit

For the desktop session, at merge or after.

- `.github/workflows/boosted.yml` and the core's CMake options: the test
  job of R0.
- `cpu/xex_module.cc`, `cpu/processor.cc`: R5 as designed above.
- `cpu/backend/code_cache_base.h`: nothing needed for the prototypes. For
  R3 part 2, `OnCodePlaced` lives in `backend/a64/a64_code_cache.cc`, which
  is in this branch's area; the base header is not.
- `memory.cc`: R7's second option.
- `boosted/HANDOFF.md`, `boosted/docs/ARCHITECTURE.md`: the settings table
  and the upstream file list below.

## 8. Found in passing (not changed, for whoever owns the decision)

- `RSQRT_V128::Emit` (`a64_sequences.cc`) calls the C function
  `PpcVrsqrtefpLane` with a bare `blr x9`, four times, not through the
  thunk. Its comment says the function is pure integer math. Nothing stops
  the C compiler using v0 to v7 or v16 to v31 in it (for copying, zeroing or
  vectorising), and the allocator may have live guest values in v4 to v7
  and v16 to v31 at that point. Whether it does is decided by each build of
  the compiler. Either call it through the thunk or check the function's
  disassembly in the shipped library.
- `RegisterAllocationPass::Run`: the `preferred_reg` reuse is dead code (the
  test `if (preferred_reg.set)` is made on the zero-initialised struct
  before `preferred_reg = instr->src1.value->reg` could run). Harmless on a
  three-operand host; the allocator just never reuses a dying source's
  register for the result.
- `A64Emitter::PushStackpoint` builds a far-safe branch by hand (`b(LT,
  no_overflow); b(overflow_label)`) on top of `A64Emitter::b`, which is
  already far-safe: three branch instructions where two would do.
- `EmitFpBinOpWithPpcNan_F64` sets a generated NaN to 0xFFF8000000000000.
  PowerPC's default NaN is 0x7FF8000000000000 as far as I know; the tests in
  `cpu/ppc/testing` mention both values and I did not work out which
  applies where. Flagged as a question, not a finding.
- `DataFlowAnalysisPass` and `ValueReductionPass` are compiled and unused.

## 9. Upstream files changed, and why

All marked `hakuX-Boosted:` at each change.

| File | Change |
|---|---|
| `cpu/backend/a64/a64_backend.h` | `guest_to_host_lean_thunk()` and its member |
| `cpu/backend/a64/a64_backend.cc` | setting `a64_lean_extern_thunk`; `EmitGuestToHostLeanThunk`; generated in `Initialize` |
| `cpu/backend/a64/a64_emitter.h` | `bound_label_offsets_`, `IsNearBoundLabel`, the two range constants |
| `cpu/backend/a64/a64_emitter.cc` | settings `a64_short_backward_branches`, `a64_compact_stackpoint_push`; block label offsets recorded in `Emit` and cleared in `ResetPerFunctionState`; the nine branch helpers take the short form when `IsNearBoundLabel`; `CallExtern` picks the thunk; `PushStackpoint` shifted add |
| `cpu/compiler/compiler.cc` | setting `log_translation_stats`; the timed copy of the pass loop in `Compile`; the counters |
| `cpu/compiler/passes/conditional_group_pass.cc` | counts runs and iterations when `log_translation_stats` |
| `cpu/ppc/ppc_hir_builder.h`, `.cc` | setting `inline_extern_calls`; `TryInlineExternCall` |
| `cpu/ppc/ppc_emit_control.cc` | `InstrEmit_branch` tries `TryInlineExternCall` after the leaf test |

## 10. What I did not attempt, and why

- **R1, R2, R3 parts 2 to 4, R6, R9**: each changes the register model, the
  flag model or the lifetime of emitted code. None can be shown correct by a
  green build, and a wrong one corrupts guest state silently in ways a boot
  test may not show. Designs are above; R0 is the gate.
- **R5**: the design is small, but its files belong to nobody tonight.
- **The FPCR read-before-write and the preempt check through the lean
  thunk**: reasons given under R4.
- **Any change to float or NaN semantics**: needs per-title testing.
- **Memoising `GetScalarNZM` and folding the simplification walks**: both
  would change the optimiser's output (never to something unsound, by the
  argument in R10, but not bit-identical), and the measurement that says
  whether they are worth it does not exist yet. `log_translation_stats` is
  there to get it.
- **Reading emitted code for the hot functions**: there is no dump from the
  device in the repository (rightly: no game code is kept here), so the
  instruction counts in section 2 come from the emitter's source, not from
  disassembly.

## Build state

See the end of this file's history: the last commit message on the branch
says whether the Actions run for it was green. Local check: every changed
file passes `clang++ -fsyntax-only` for an arm64 target against the pinned
xbyak_aarch64, fmt and date sources (host C library headers, so pointer-size
noise from those was ignored).

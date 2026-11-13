# The translator, from study to code: pinned guest registers, cheaper translation, a cheaper thunk

Branch `claude/cpu-translator`, cut from `claude/integration-1113` at `d940365`.
Written 2026-11-13 by a cloud session. It follows `boosted/notes/cpu-translation.md`
(the study) and takes its recommendations in order of gain over risk.

**Nothing here has run on a device.** The only check was a green GitHub Actions
build and a local syntax check. No speed-up is claimed anywhere in this file:
every "saves" is a count of instructions or of translation work read off the
source, not a measurement. Every setting is off by default and the defaults
leave behaviour as it was.

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/` unless they
start with another directory.

## Done / in progress / next

Done (all behind settings, all off):
- `pin_guest_registers`: guest registers held in host registers across a whole
  function (study R1, in the per-function form; section 1).
- `a64_pin_verify`: its self-check mode.
- `simplification_skip_unchanged_blocks`: the repeated simplification walks
  skip blocks that cannot have changed, with the same output (section 2), and
  `simplification_skip_verify`, its self-check mode.
- `a64_thunk_fpcr_compare`: the host-call thunks write FPCR only when it
  differs (study R4 part 5; section 3).
- `log_translation_stats` prints a third line with the counts for all of the
  above (section 5).

In progress: nothing half-written is on the branch.

Considered and not built (section 4): translating a function's callees on a
helper thread. It needs one line in `cpu/processor.cc`, which is not this
branch's file, to be safe at shutdown.

Next, in order:
1. A device run of each setting alone, in the order of section 6.
2. If `pin_guest_registers` holds up: raise `pin_guest_registers_max_int` to
   3 and compare; then the dead condition-register store pass (study R2),
   which pinning makes more worthwhile.
3. The helper-thread translation, once the `processor.cc` line is in.
4. The PowerPC instruction tests in CI (study R0) remain the missing piece:
   with them, each setting could be checked on and off in minutes.

## The settings

| Setting | Group | Default | What it changes | Expected effect | Risk |
|---|---|---|---|---|---|
| `pin_guest_registers` | CPU | false | Emitted code of functions that contain a loop: the hottest guest registers live in host registers for the whole function | Fewer loads and stores in loops; shorter dependency chains through memory for loop counters, pointers and float accumulators. Nothing for straight-line or call-heavy code, which it leaves alone | The highest here: a missed sync point would hand host code a stale register or lose a change. Section 1 lists every sync point and why the list is complete |
| `pin_guest_registers_max_int` | CPU | 2 | Most integer registers one function may pin (0 to 3) | More pins save more loads but leave the allocator 4 instead of 5 registers | More spills in busy blocks at 3 |
| `pin_guest_registers_max_vec` | CPU | 8 | Most float and vector registers one function may pin (0 to 12 of 28) | As above for float and VMX code | Low: 16 or more stay for the allocator |
| `a64_pin_verify` | a64 | false | With pinning on: stores also go to the context, loads compare register and context | None (slower than not pinning). A debugging run | None beyond the slowdown |
| `simplification_skip_unchanged_blocks` | CPU | false | Translation only: repeated simplification walks skip unchanged blocks | Less time in `SimplificationPass` on the thread that translates; the same translated code | Low. If the locality argument of section 2 were wrong the translated code would differ; the verify mode detects exactly that |
| `simplification_skip_verify` | CPU | false | With the above: nothing is skipped, and a change in a block that would have been is logged | None. A checking run | None |
| `a64_thunk_fpcr_compare` | a64 | false | Both guest-to-host thunks: `mrs`, compare, conditional `msr` instead of an unconditional `msr` | One FPCR write fewer per host call when the host function did not touch FPCR (the usual case). Whether that is a gain depends on what the write costs on this core | Very low: FPCR holds the same value afterwards either way. Read once at start-up |

`pin_guest_registers`, the two simplification settings and `a64_pin_verify`
take effect for functions translated after they are set; set them in the
config before launch. The settings the study added (`a64_lean_extern_thunk`,
`inline_extern_calls`, `a64_short_backward_branches`,
`a64_compact_stackpoint_push`) are unchanged and combine with these.

## 1. Guest registers pinned across a function (`pin_guest_registers`)

### What it does

Today every block loads each guest register it uses from `PPCContext` at first
use and stores it back if it changed, because no value outlives its block. A
loop of one block that advances a pointer and a counter loads both, changes
both and stores both on every trip, and the next trip's load waits for the
previous trip's store.

With the setting on, a new analysis pass
(`compiler/passes/guest_register_pinning_pass.cc`, run directly before register
allocation) picks, per function, a few context slots to keep in host registers
from the end of the prologue to the last instruction:

- candidates are r0, r3 to r12, r14 to r31 and CTR (as 64-bit integers), f0 to
  f31 (as doubles) and v0 to v127 (as vectors);
- only in functions that contain a loop (a branch to a block at or before its
  own);
- a slot is weighed by its remaining context loads and stores, each counted 1
  outside loops, 8 in a loop, 64 and 512 in nested ones, against the cost it
  adds: one load at entry, and at every sync point (see below) a store if the
  function writes the register and a load if the function carries on;
- it must come out at least 8 ahead, which means at least one access inside a
  loop clear of all costs. A register used only round calls is never pinned;
- at most `pin_guest_registers_max_int` integer and
  `pin_guest_registers_max_vec` float or vector slots, best first.

The chosen host registers come out of the allocator's own sets, from the top
(x28, x27, x26; v31 downwards), and the allocator is told to leave them alone
for that function (`RegisterAllocationPass`, the `reserved` mask). In the
emitter a pinned slot's `LOAD_CONTEXT` becomes a register move from the pin
and its `STORE_CONTEXT` a move into it.

This is the study's R1 in a per-function form, not its global static mapping.
The global form needs the 175-use scratch-register audit the study describes
and cannot be checked without the instruction tests. The per-function form
needs no agreement between functions: each is correct on its own, at the
price of syncing round calls, which the cost model keeps out of the picture
by not pinning where calls dominate.

### Correctness argument

The context copy of a pinned register is stale while the function's own code
runs. The rule that makes this invisible:

> the context is made current (flush) before control leaves the function's own
> code, and the pinned registers are read back (reload) wherever control comes
> back in.

A flush is always safe: the pin holds the register's current value. A pin the
function never writes is not flushed, because it still equals the context
copy it was loaded from. A reload is safe exactly where the context is
current, so reloads sit only directly behind a flush-and-leave.

Ways out of a function's own code, and where each is handled (all in
`backend/a64/a64_emitter.cc`, see the block comment above `EmitPinnedFlush`):

| Way out | Handling |
|---|---|
| Guest call, direct or indirect, conditional or not (`CALL`, `CALL_TRUE`, `CALL_INDIRECT`, `CALL_INDIRECT_TRUE`) | Flush before the sequence, reload after, in `Emit`'s instruction loop (`ClassifyPinSync`). An unconditional tail call is flush only |
| `blr` (an indirect call with the possible-return flag, which may branch to the epilogue) | The same flush covers the branch to the epilogue |
| Kernel and builtin calls (`CALL_EXTERN`, through either thunk) | Flush, reload |
| `RETURN`, `RETURN_TRUE`, and falling off the last block into the epilogue | Flush |
| `TRAP`, `TRAP_TRUE`, `DEBUG_BREAK`, `DEBUG_BREAK_TRUE` | Flush, reload |
| `CONTEXT_BARRIER` (the front end's own "the context must be current") | Flush, reload |
| The scheduler's preemption check (`CHECK_PREEMPT`) | Its cold path only, round the yield (`EmitPreemptCheck`): the test on the usual path does not leave and costs nothing extra, so loops keep their benefit |
| Any host helper called from inside a sequence: MMIO access, clock, spin park, pow and log, reservations, tracing | Inside `CallNativeSafe` itself, so a helper added later is covered without anyone remembering |
| The two C functions called with a bare `blr` (`RSQRT_F64`, `RSQRT_V128`) | Bracketed by hand (`EmitPinnedSyncBeforeBareHostCall`): they never see the context, but nothing saves the vector registers a C function may use |
| The resolve thunk, `ResolveFunction`, the stack synchronisation helper | Reached only from inside a guest call's sequence, after its flush |

Ways in:
- the first instruction: the context is current (every caller is a guest call
  or the host-to-guest thunk) and the prologue loads the pins;
- the return from any of the above: reloaded;
- **a `longjmp` into the middle of the function.** `ResolveFunction`
  (`a64_backend.cc`) resumes an existing frame at the host address of a return
  site. That address lies behind the reload, so the pins would hold whatever
  the jumping code left there. Every return site of a function with pins
  therefore gets a stub in its tail code (reload, then branch to the ordinary
  return-site address, where the existing stack check follows), and
  `ResolveFunction` resumes at the stub (`A64Function::MapPinnedResumeAddress`).
  The stub table is stored on the function before its code is placed in the
  cache. A return site with no stub (none should exist) is not resumed in
  place: the general path translates from that address, as it already does
  when no function is found, and a warning is logged.

The host registers themselves: x22 to x28 and v4 to v31 are used by no
sequence as scratch (checked by search for literal uses). x22 to x28 are
callee-saved for host code, the full thunk saves v4 to v31, and after every
call that goes through neither the pins are reloaded anyway.

What may still see a stale context copy of a pinned register: something that
inspects `PPCContext` while the function's own code is interrupted between
sync points, which means a signal handler or a debugger. The MMIO fault
handler does not read guest registers from the context (it decodes the host
instruction). A crash report would show the last synced values. Translated
code already had this property inside a block (context promotion drops
overwritten stores); pinning stretches it to the function. The registers host
code reads at arbitrary points are never pinned: r1 (the prologue's
stackpoint push, `ResolveFunction`'s test, the kernel), r13 (the PCR pointer)
and LR; nor is anything that is not a register (CR and XER bytes, FPSCR,
scheduler and reservation fields). With the `debug` setting on, nothing is
pinned.

A slot is pinned only if every access to any of its bytes in the function is
the same access (same offset, same type), so a register that is also touched
in another shape stays in the context.

### What the green build does not show

- That the list of ways out is complete on a real title. It was built from
  reading every `CallNative`, `blr` and context access in the back end, not
  from running anything.
- That four or five allocator registers are enough in practice. The allocator
  spills when it runs out and should cope with three; it has only ever run
  with seven.
- That the cost model picks well. It only affects speed, not correctness.

### How to check it on a device

1. `pin_guest_registers = true`, `log_translation_stats = true`. In `xe.log`
   the third `TranslationStats:` line gives: functions considered, with loops,
   pinned; slots by kind; how many context loads and stores became moves; sync
   sites and resume stubs emitted; and which guest registers were pinned. If
   "pinned" is near zero the model is too strict for this title and the
   setting does nothing.
2. `a64_pin_verify = true` as well, for one run through the same scenes. In
   this mode the context is written at every store as without pinning and
   stays authoritative; every load of a pinned register compares the host
   register with it. A line starting `PinVerify:` names the guest function and
   context offset where they differed, the count is in the stats line, and the
   run carries on from the context value. A difference means a pinned register
   was clobbered or a reload is missing. What this mode cannot show is a
   missing flush (host code reading a stale context), since the context is
   never stale in it: that is the part the table above has to carry.
3. Then without verify: boot to play in Fable II, one title that uses
   `setjmp`/`longjmp` (the stats line's "resume entries" counts longjmps that
   went through a stub), one that is float heavy.
4. simpleperf of the six dispatch threads, 200 s, as the baseline profile:
   compare the guest code share, `sub_82C62548` (the audio mixer) and fps. With
   `dump_functions_at` on a hot function, the machine code dump shows the
   moves in place of `ldr`/`str [x20]` inside the loop.

A cheaper-to-trust check than any of these, not built: translate a function
both ways and run both. It needs a second copy of the context and of the
memory the function writes, so it only works for functions that make no
calls and no stores to memory, which are few; the verify mode above was
chosen instead because it covers every pinned function.

## 2. The cost of `SimplificationPass`

### Why it costs more than the other passes together

From the code:

1. `ConditionalGroupPass::Run` reruns `SimplificationPass` and
   `ConstantPropagationPass` until one whole round changes nothing. So every
   function pays for at least one round that by definition does nothing, and
   in practice two or three rounds in all.
2. Each round of `SimplificationPass` is five separate walks over every
   instruction of the function (`SimplifyBitArith`, `EliminateConversions`,
   `SimplifyAssignments`, `SimplifyBasicArith`, `SimplifyVectorOps`), where
   every other pass makes one.
3. The pass runs once more on its own later in the pipeline.

That is 15 to 20 walks of the function against one or two for each other
pass, which matches the profile (3.8% of GameThread in `SimplificationPass::Run`
itself: the walks are inlined into it). `GetScalarNZM`, which the study
suspected, is exponential in the worst case but does not appear in the
profile by name, so it was left alone.

### The fix (`simplification_skip_unchanged_blocks`)

A walk is a function of the block in front of it and nothing else. Every rule
starts from an instruction, follows its operands to their definitions and use
lists, and rewrites that instruction or inserts one before it. Values other
than constants are defined and used in one block (the register allocator
relies on this and asserts it), so all of that stays inside the block. A walk
that has just been through a block without changing it would do nothing if run
on it again, until something changes that block.

So each block carries a stamp of its last change and, for each of the five
walks, a stamp of its last visit that changed nothing
(`hir/block.h`). A walk skips a block whose clean stamp for that walk is newer
than its change stamp. Changes are stamped by the walks themselves and by
`ConstantPropagationPass`, per block. The stamps are trusted only within one
run of the group; the lone `SimplificationPass` later in the pipeline never
skips, because the passes before it do not stamp.

The translated code is the same because a skipped walk is one that would have
changed nothing; the number of rounds, and each walk's answer to "did anything
change", are untouched. Two things had to be right for that:

- every change must be stamped. All rules were read for this. One rule in
  `CheckAnd` can turn a rotate into a shift on a definition and still answer
  "no change"; its answer is left as it was (it decides whether the group runs
  again, so changing it would change the output) and the stamp is told
  separately (`unreported_change_`). `ConstantPropagationPass` was scanned
  mechanically: every mutation is followed by `result = true`.
- constant propagation reads guest memory, so it is not a function of the HIR
  alone. It is never skipped.

Expected saving: the walks of the second and third round over every block that
the round before did not touch, which in a function of many blocks is most of
them. How much time that is must be measured.

### How to check it on a device

1. `simplification_skip_unchanged_blocks = true`,
   `simplification_skip_verify = true`, `log_translation_stats = true`, one
   run through the usual scenes. Nothing is skipped in this mode. The stats
   line ends with "N skip mismatches": it must be 0, and there must be no
   `SimplifySkipVerify:` line. Any mismatch means a walk changed a block the
   skip would have passed over, so the translated code would have differed;
   the setting must then stay off and the line reported.
2. Then verify off. The stats line gives "block walks" and "skipped": the
   share skipped is the work saved. The first `TranslationStats:` line gives
   milliseconds per pass; compare the group's entry between a run with the
   setting off and one with it on (the same scenes, the same number of
   functions).

## 3. Host-call thunks (`a64_thunk_fpcr_compare`)

The study named three safe ways to cut thunk cost. Two were already built by
it and are on this branch unchanged: `a64_lean_extern_thunk` (no vector saves
round a kernel call) and `inline_extern_calls` (no guest frame round a kernel
call). Both remain correct with pinned registers: a `CALL_EXTERN` is a sync
point, so nothing pinned is live across it either.

The third is built here. Both guest-to-host thunks restored FPCR with an
unconditional `ldr; msr` after every host call. With the setting on they do
`ldr; mrs; cmp; b.eq; msr`: the write happens only when the host function left
a different value. FPCR ends up the same either way. On cores where a write
to FPCR stalls the pipeline and a read does not, this takes a stall off every
kernel call; whether this phone's cores are like that is what the run has to
show. Check: simpleperf's `a64_host_code` share with the setting off and on,
alone and with `a64_lean_extern_thunk`.

Not done: the preemption check's yield through the lean thunk (study R4 part
3). Its safety rests on a pass in the scheduler session's area keeping the
check at a block head.

## 4. Translating callees on a helper thread: considered, not built

The idea: when a function is translated on a guest thread, queue the targets
of its direct calls that have no code yet, and let one helper thread call
`Processor::ResolveFunction` on them, so that the guest thread finds them
ready.

What makes it workable: `ResolveFunction` is already safe from several
threads (the entry table hands one caller the job, the translator pool gives
each thread its own translator), and a guest thread that needs a function the
helper is translating waits for it as it would for another guest thread.

Why it is not on the branch:
- **Shutdown.** `Processor::~Processor` clears the module list before it
  destroys the front end. A helper thread in the middle of a translation would
  be reading a module that has just been freed. The front end cannot stop its
  thread early enough from its own destructor. The fix is one line in
  `cpu/processor.cc`, not this branch's file: stop the helper (a
  `frontend_->StopBackgroundTranslation()` that sets a flag and joins) as the
  first statement of `~Processor`, before `modules_.clear()`.
- **Priority.** A guest thread that needs a function the helper holds sleeps
  in 10 microsecond steps until the helper finishes (`EntryTable::GetOrCreate`).
  The dispatch threads run at nice -8; a helper at a lower priority on a phone
  that squeezes threads onto fewer cores when hot could hold GameThread for
  longer than translating would have taken. The helper would have to run at
  the dispatch threads' priority, which costs heat.
- **Waste.** Not every call target is reached. The code cache already hits
  128 MiB in two minutes.

Design for whoever builds it, after the `processor.cc` line: in
`PPCTranslator::Translate`, after `Compile`, walk the HIR for `CALL` and
`CALL_TRUE` whose target has no `machine_code()`, push the addresses on a
bounded queue owned by `PPCFrontend` (drop when full, never block), one
helper thread, depth one (the helper's own translations queue nothing), a
setting that defaults to off, and counters in `translation_stats.h` for
queued, translated by the helper, found already done, dropped, and
translated on the thread that needed them.

## 5. What `log_translation_stats` prints now

Every 2048 functions, three lines starting `TranslationStats:`. The first two
are the study's (milliseconds per pass and group iterations; context loads and
stores left per guest register). The third is new:

```
TranslationStats: pinning: A functions considered, B with loops, C pinned;
slots gpr D fpr E vr F; context loads G and stores H became register moves;
I sync sites, J resume stubs, K resume entries, L verify mismatches;
pinned gprs: r3 n r31 n ctr n ...; simplification: M block walks, N skipped,
O skip mismatches
```

All are totals since launch. A to J and M to O are static counts taken at
translation time (each instruction once however often it runs); K and L count
at run time. The pinning counters move only with `pin_guest_registers` on and
the simplification ones only with `simplification_skip_unchanged_blocks` on.

## 6. Suggested order on the device

One setting at a time, each compared with a baseline run of the same build
with everything off.

1. Everything off: the build must behave as `claude/integration-1113`. No
   emitted instruction differs with the settings off (by reading, not by a
   byte comparison): the context sequences look up an empty pin list, the
   allocator applies an empty mask, the thunks emit the same `ldr; msr`.
2. `simplification_skip_unchanged_blocks` + `simplification_skip_verify`
   (expect 0 mismatches), then without verify.
3. `a64_thunk_fpcr_compare`.
4. `pin_guest_registers` + `a64_pin_verify` (expect no `PinVerify:` line),
   then without verify, then with `pin_guest_registers_max_int = 3`.
5. All of the above with the study's four settings.

Report for each: the three `TranslationStats:` lines after five minutes, fps
from `device-watch.py`, and for 3 and 4 a simpleperf profile.

## 7. Upstream files changed, and why

All marked `hakuX-Boosted:` at each change.

| File | Change |
|---|---|
| `cpu/hir/hir_builder.h`, `.cc` | the list of pinned context slots; the pass stamp counter and floor; both reset per function; new blocks start with zero stamps |
| `cpu/hir/block.h` | the block's change stamp and five clean stamps |
| `cpu/compiler/passes/guest_register_pinning_pass.h`, `.cc` | new: the pass and the three `pin_guest_registers*` settings |
| `cpu/compiler/compiler_passes.h` | includes the new pass |
| `cpu/compiler/passes/register_allocation_pass.h`, `.cc` | the `reserved` mask per register set, filled from the pinned slots, cleared from availability at each block |
| `cpu/compiler/passes/simplification_pass.h`, `.cc` | the two `simplification_skip*` settings; `BeginBlockWalk`/`EndBlockWalk` round each of the five walks; `unreported_change_` |
| `cpu/compiler/passes/constant_propagation_pass.cc` | `result` kept per block inside the loop so a changed block can be stamped; the function's answer is unchanged |
| `cpu/compiler/passes/conditional_group_pass.cc` | sets the stamp floor for the length of a run |
| `cpu/compiler/translation_stats.h` | new: the shared counters |
| `cpu/compiler/compiler.cc` | defines the counters and prints the third stats line |
| `cpu/ppc/ppc_translator.cc` | adds the pinning pass before register allocation (ARM64 builds only) |
| `cpu/backend/a64/a64_emitter.h`, `.cc` | setting `a64_pin_verify`; the pin list per function; `EmitPinnedFlush`, `EmitPinnedReload`, `EmitPinnedVerify`, `ClassifyPinSync`; the prologue load; sync round instructions in `Emit`'s loop; the flush before the epilogue; resume stubs; `CallNativeSafe` and `EmitPreemptCheck` sync |
| `cpu/backend/a64/a64_sequences.cc` | `LOAD_CONTEXT` and `STORE_CONTEXT` for I64, F64 and V128 ask for a pin first; the two bare host calls in `RSQRT_F64` and `RSQRT_V128` are bracketed |
| `cpu/backend/a64/a64_function.h`, `.cc` | the resume stub table and `MapPinnedResumeAddress` |
| `cpu/backend/a64/a64_backend.cc` | `ResolveFunction` resumes a function with pins at its stub; setting `a64_thunk_fpcr_compare` and `EmitGuestFpcrRestore` in both guest-to-host thunks |

Nothing outside `cpu/backend/a64`, `cpu/compiler`, `cpu/hir` and `cpu/ppc` was
touched, and nothing in `kernel/` or `gpu/`.

## 8. Changes needed in files this branch may not edit

- `cpu/processor.cc`: the helper-thread stop at the top of `~Processor`
  (section 4), before that work can start.
- `boosted/HANDOFF.md`, `boosted/docs/ARCHITECTURE.md`: the settings table
  above and the file list of section 7, at merge.

## 9. Found in passing

- The study's note on `RSQRT_V128` calling C with a bare `blr` also applies to
  `RSQRT_F64` (`PpcFrsqrte`). Pinned registers are protected from it here; the
  allocator's own values in v4 to v7 and v16 to v31 still are not, as the
  study said. Calling both through the thunk would close it.
- `CheckAnd` in `simplification_pass.cc` can change an instruction and report
  no change (the rotate-to-shift rule before `goto retry_and_simplification`),
  so the group can stop one round early. Left as it is: reporting it would
  change the translated code.
- A host helper called through the thunk from the middle of a block leaves
  FPCR in the scalar mode while the emitter's mode tracking may still say
  vector (`CallNativeSafe` does not call `ForgetFpcrMode`). Whether any such
  helper sits between two vector float operations in practice was not
  checked.

## Build state

See the end of this file's history in git for the run of the branch tip; the
commit that added the code is `a5eaac1`.

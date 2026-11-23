# Self-written code (`claude/code-writes`)

PLAN.md, Rewrite 1, "Self-written code". Branch `claude/code-writes` off
`claude/integration-1116`. Compiled in CI only; nothing here has been run on the
phone yet.

## What was wrong

- `probe_exec` and `xsdk_test` (which stops after 90 of about 685 checks, at
  "pages: freeing 32 MB", the check before `written_code`) die with
  `Access Violation: write at 0x0000000882090000`: the title writes code into
  its `.jit` pool, a Code section of its own image, and the core maps Code
  sections read-only (`XexModule`, "Setup memory protection", unless
  `writable_code_segments` or `allow_plugins`). On the console the write
  succeeds, the written code runs, and `MmQueryAddressProtect` reports the
  pages as `0x20` (`ref/xsdk/docs/HARDWARE.md`, milestone 55).
- `icbi` was emitted as a no-op, so code rewritten after its first
  translation kept running the old translation. xsdk's check writes
  `li r3,42; blr`, flushes (dcbst lines, sync, icbi lines, sync, isync), calls
  it, rewrites it to `li r3,99`, flushes and expects 99.

## What changed

All upstream-file edits are marked `hakuX-Boosted:` and listed in
`boosted/docs/ARCHITECTURE.md`.

1. **Code sections writable** (`xex_code_writable`, default on, in
   `cpu/xex_module.cc`). Only `XEX_SECTION_CODE` pages; read-only data keeps
   the old rule (writable only with `writable_code_segments` or
   `allow_plugins`). Why not read-only data too: xsdk and HARDWARE.md say
   nothing about writes to it on the console (the `.jit` evidence is for a
   Code section), and a title writing to read-only data is more likely a bug
   than a JIT. A probe that writes to `.rodata` on the console would settle
   it; until then it stays as upstream has it.
2. **Reported protection as the console's.** `MmQueryAddressProtect` returns
   `PAGE_EXECUTE_READ` (`0x20`) for an address in one of the executable
   image's Code sections (`XexModule::IsCodeAddress`). Before, the core
   reported `0x02` (read-only); with the pages now read-write it would have
   reported `0x04`. `NtQueryVirtualMemory` on image pages is untouched (the
   console refuses it with `C000000D`; the core does not yet: a follow-up).
3. **`icbi` invalidates** (`icbi_invalidates_code`, default on, defined in
   `cpu/processor.cc`). The front end stores the effective address in
   `PPCContext::scratch` and calls a new builtin, `InvalidateCode`, which
   calls `Processor::InvalidateGuestCode(line, 128)` for the 128-byte line.
   - Fast reject: one bit per 4 KB guest page that has held translated code
     (set in `ResolveFunction`, never cleared, 128 KB in all). A line on a
     page with no translated code returns at once, so a title that runs
     `icbi` over freshly loaded code pays a host call and a bit test per line.
   - Otherwise, under its own mutex: `EntryTable::TakeOverlapping` removes and
     returns every ready entry whose `[address, end_address]` overlaps the
     line (a walk of the table, only on pages with code);
     `Module::ForgetFunction` drops each from its module's symbol map (the
     object stays alive: its old translation may be running, and the code
     cache's host-PC map still points at it); then
     `Backend::RetireGuestFunction`.
   - `A64Backend::RetireGuestFunction`: the guest address's indirection slot
     goes back to the resolve thunk (`ResetIndirection`); its linked-call
     target is forgotten (`A64CodeCache::ForgetLinkTarget`), so callers placed
     from now on wait for the new translation; and the old translation's
     first instruction (always the frame's `sub sp`, never a branch target) is
     replaced, with one aligned 4-byte store, by a `b` to a 24-byte stub
     placed in the code cache: `movz/movk w16, guest address; ldr x17, =thunk;
     br x17`. That is what a call to a not-yet-translated function does, so
     every caller that reaches the old code by its host address (direct calls
     compiled when the callee had code, linked-call literals, stale reads of
     the slot) goes through the resolve thunk with x0 (the guest return
     address) and every other entry register unchanged, and the next call
     translates the rewritten code into a new function object.
   - The next call: `ResolveFunction` finds no entry, `LookupFunction`
     declares a new symbol, the scanner reads the guest memory as it is now.
4. Counters in the translation stats line (`log_translation_stats`):
   `rewritten code: icbi lines N, M of them over translated code, translations
   retired R, U of those with an unpatched entry`. The first 16 retirements
   are logged as `Guest code rewritten at ...: translation of A-B retired`
   whatever the settings.

## Risks

- **Retail titles that write their own code sections now succeed instead of
  faulting.** That is the console's behaviour; a title that relied on the
  fault does not exist on the console. Switch: `xex_code_writable = false`.
- **Retail titles that run `icbi`** now pay a host call per line (the bit
  test), and a table walk plus retranslation where the line holds translated
  code. If a title runs `icbi` over code it did not change (a blanket flush),
  that code is retranslated once per flush: a stutter, not a fault. The
  counters show it. Switch: `icbi_invalidates_code = false` restores the
  old no-op.
- **Threads inside the old code** keep running it until they leave it (as
  they would on hardware until they refetch); a non-local return (longjmp)
  into an old frame of a retired function resolves through the new function,
  whose frame layout may differ. Not expected outside self-modifying titles.
- **A function being translated while its code is invalidated** (entry still
  compiling) is not caught: it is finished and installed from the old bytes.
  Single-threaded JITs such as xsdk's are not affected.
- **Cross-modifying code on ARM64.** The entry is patched with one store and
  the instruction cache is flushed for both views, but the old word (`sub
  sp`) is not one of the instructions the architecture guarantees for
  concurrent modification; a core already fetching that word may see either.
  In practice both outcomes are a valid entry to the old or new code.
- **Code cache over 128 MB.** If the stub is out of a `b`'s reach of the old
  entry, the entry is left as it is (logged, counted as unpatched): calls
  through the indirection table and newly translated callers retranslate,
  callers that link straight to the old code keep it.
- x64 backend: `RetireGuestFunction` is a no-op there (not built for the
  phone); the entry and symbol are still dropped, so indirect calls retranslate.

## The phone test (not run; the phone was busy)

Needs the APK from this branch's CI run. Run after the current batch, with
the same `xsdk-probes` artifact (`C:/Users/ctyld/Desktop/HakuX/xsdk-probes/`).
From `C:/Users/ctyld/Desktop/HakuX` in Git Bash:

```
bash tools/run-xsdk.sh xsdk-probes/xex 90 '/(xsdk_test|probe_exec|m55_check)\.xex$'
python tools/xsdk-compare.py device-logs/xsdk xsdk-probes/results
```

What to look for:

- `device-logs/xsdk/probe_exec/probe.log`: no `Access Violation`; `T1` returns
  42 after the flush and the rewritten code returns 99;
  `MmQueryAddressProtect(.jit) = 00000020` as on the console.
- `device-logs/xsdk/xsdk_test/probe.log`: past `pages: freeing 32 MB`; `PASS:
  jit: written code returns 42`, `PASS: jit: rewritten code returns 99`,
  `PASS: jit: unaligned range crossing a line returns 42`, `PASS: jit:
  129-byte range publishes both lines and returns 99`, and the run going on
  towards the full ~685 checks (whatever stops it next is the next finding).
- `device-logs/xsdk/m55_check/probe.log`: no new failures against the last run.
- `xe.log`: `Guest code rewritten at ...` lines for the rewrites.
- `xsdk-compare.py`: compare with the console's results; the jit lines should
  now match.

A retail check worth one run once this is merged: Fable II with
`log_translation_stats = true`, to read the `rewritten code:` counters (how
many `icbi` lines a retail title runs and how many hit translated code).

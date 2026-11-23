# Guest memory ordering, level 2 (branch `claude/tso-order`)

Built by CI only. Not run on the phone: nothing here is a fix or a speed-up
until the test at the end has been run.

## Why

The Xbox 360's guest code was written for a PowerPC that the x64 xenia host
emulates with total store order (TSO) for free. The A64 backend emitted guest
loads and stores as plain `ldr`/`str`, which Arm may reorder in ways an x64
host never does. Gears of War's render thread reads a command ring
(reader at guest 0x82444F20..0x82444F78, no barrier before it reads the slot;
the writer at 0x8221BF80 stores, `lwsync` at 0x8221BFE0, then advances the write
pointer at 0x8221BFF4). On build 149 with default settings it crashed in
loading 2 of 2 times (host-side crash, guest lr 0x82444F7C); with
`a64_guest_memory_order = 1` (a `dmb ishld` after every guest load, a
`dmb ishst` before every guest store) it loaded 5 of 5 times, but slowly. Why
the console tolerates the reader's missing barrier is not settled; level 1
making the loads pass says ordering is involved (a 10 against 10 batch is
measuring that now).

## What level 2 does

`[a64] a64_guest_memory_order = 2`. Default stays 0. Takes effect for
functions translated after it is set (set it in the config before the title).

| Guest access | Level 0 | Level 1 | Level 2 |
|---|---|---|---|
| Load, 8/16/32/64-bit (incl. floating point, byte-swapped) | `ldr` | `ldr; dmb ishld` | `add x16, membase, addr; ldapr` (then `rev`/`fmov` as before) |
| Store, 8/16/32/64-bit | `str` | `dmb ishst; str` | `add x16, membase, addr; stlr` (value `rev`ed or `fmov`ed into w17/x17 first, as before) |
| Load, 128-bit (`lvx`, `lvlx`, `lvrx`) | `ldr q` | `ldr q; dmb ishld` | `ldr q; dmb ishld` |
| Store, 128-bit (`stvx`, `stvlx`, `stvrx`) | `str q` | `dmb ishst; str q` | `dmb ishst; str q` |
| `dcbz` (MEMSET) | stores | `dmb ishst` first | `dmb ishst` first |
| `lwarx`/`ldarx` (native path) | `ldr` | `ldr; dmb ishld` | `ldapr` |
| `stwcx.`/`stdcx.` (native path) | `casal` | `dmb ishst; casal` | `casal` (already acquire and release) |
| Stack: address is r1 plus a constant | as above | barriers | plain `ldr`/`str` |
| Constant address in a page the title cannot write | as above | barriers | plain `ldr`/`str` |

Level 1's barriers are emitted in `A64Emitter::Emit`'s instruction loop and now
apply at level 1 only (`lvlx`/`stvlx` and friends are left as level 1 always had
them). Level 2 is emitted inside the sequences (`a64_seq_memory.cc`,
`a64_seq_vector.cc`) through `GuestLdr`/`GuestStr` and friends in
`a64_seq_util.h`, which emit exactly the old instruction when an access is not
ordered.

### Why LDAPR and STLR give TSO

TSO keeps every order except a store followed by a later load of another
address. `LDAPR` (FEAT_LRCPC, Armv8.3) is an acquire load with RCpc
semantics: no later load or store is performed before it. `STLR` is a release
store: no earlier load or store is performed after it. Together:
load-load, load-store and store-store orders hold, and an `STLR` followed by an
`LDAPR` may pass each other, which is exactly TSO's one relaxation (and the
difference between `LDAPR` and the stronger `LDAR`, which would also forbid it
and cost more). Arm is other-multi-copy atomic, so the cross-thread result is
what an x64 host gives. `LDAPUR` (FEAT_LRCPC2) was not needed: every guest
access is `[membase, xN]`, register plus register, and neither form takes a
register offset, so the address goes into x16 with one `add`.

### Excluded, and why

- **Stack-relative accesses** (the address is the guest r1 plus a constant,
  `IsGuestStackPointerValue`, the same test `a64_stack_access_no_fixup` uses).
  Another thread rarely touches a thread's stack, and when it does it is
  through a pointer published by a store, which is an `STLR` here and so
  orders the plain stack stores before it; the reader's `LDAPR` of that pointer
  orders its own later loads. What is lost: a thread spinning on a flag in its
  own stack frame that another thread sets, with later accesses that depend on
  it. Counted in the TranslationStats line as "plain stack".
- **Read-only data**: a constant address (constant base and constant offset
  after constant propagation, as `lis`/`lfs` pairs become) in a page whose
  guest protection is readable and not writable when the function is
  translated (`IsReadOnlyGuestAddress`: the title's read-only sections and
  code, unless `writable_code_segments` makes them writable, in which case
  they are ordered). Memory that never changes needs no order. If a title
  makes such a page writable later and writes it, functions translated
  before see plain loads there. Counted as "plain read-only".
- **128-bit accesses**: no acquire or release form loads or stores a q
  register. A barrier on the side the access needs: `dmb ishld` after a load
  (later accesses wait for it), `dmb ishst` before a store (earlier stores
  first; earlier loads are already ordered by their `LDAPR`). The barriers
  are counted as "barriers".
- **MMIO** (`LOAD_MMIO`, `STORE_MMIO`, and loads and stores that may be MMIO,
  which call a host function): device handlers, not guest RAM.
- **Atomic compare-exchange**: `casal` or `ldaxr`/`stlxr`, already ordered
  both ways.
- **Memory touched by host code on the guest's behalf** (kernel exports, the
  GPU, XMA), as at level 1.

### Hosts without FEAT_LRCPC

`GuestMemoryOrderLevel()` (`a64_emitter.cc`) reads `getauxval(AT_HWCAP)`
bit 15 (`HWCAP_LRCPC`) once. Without it level 2 runs as level 1, and the log
says so once: `a64_guest_memory_order: 2 requested, 1 in effect (host
FEAT_LRCPC absent)`. With it: `... 2 requested, 2 in effect (host FEAT_LRCPC
present)`. The test phone (Snapdragon SM8850, Armv9) has it; every Armv8.3+
core does.

## Reservations across context switches

The guest scheduler runs guest threads as fibres. A thread's `lwarx`
reservation lived in its own backend context, so it survived the thread being
switched out and back in; the native `stwcx.` then compares values only and
succeeds if the word looks unchanged, even if another thread stored to it (and
restored the value) meanwhile. On the console the kernel clears the
reservation at every context switch. Now `GuestScheduler::SwitchTo` calls the
new `Backend::ClearReservation` for the thread it is about to dispatch, so a
reservation taken before it last left the CPU fails its `stwcx.` and the guest
loop retries. This is not behind the setting: it is what the hardware does,
costs one `and` per switch, and changes nothing for a thread that was not
between `lwarx` and `stwcx.` when it was switched.

## Expected cost (a prediction, to be measured)

- Level 1 adds a full barrier to every guest access; on a big Oryon core a
  `dmb` waits for the core's outstanding loads or stores to drain, tens of
  cycles, which is why it ran very slowly.
- Level 2 adds one `add` per ordered access and turns `ldr`/`str` into
  `ldapr`/`stlr`. `LDAPR` was introduced so an acquire load can be satisfied
  out of order against earlier stores, and on recent cores it costs about a
  plain load when nothing earlier is outstanding. `STLR` must wait for earlier
  accesses to be performed before it is, which costs more when it follows a
  cache miss, but it does not stall later loads as a `dmb` does. Stack
  accesses (often half of all guest accesses) stay plain.
- Expected: much closer to level 0 than to level 1; a few per cent to perhaps
  15% below level 0 in fps per GHz. Not known until the comparison below.
- The TranslationStats line (`log_translation_stats = true`, on in the run149
  configs) ends with `guest memory order: acquire/release accesses N, plain
  stack N, plain read-only N, barriers N`: the share left plain is read from
  there.

## The phone test (do not run while another batch has the phone)

1. **Loads.** `device-logs/run149/cfg/door-C.toml` is `door-B.toml` with
   `a64_guest_memory_order = 2` instead of 1 (otherwise the same as door-A,
   the defaults). Back the Gears save up first (the script overwrites the
   checkpoint) and restore it after:

   ```
   bash tools/gears-loads.sh 10 device-logs/run149/cfg/door-A.toml device-logs/run149/cfg/door-C.toml
   ```

   A against C, alternating, ten rounds. Pass: C has no `CRASH-RING` in ten,
   with A still crashing at about the rate the level 0 against level 1 batch
   showed. Check each C log for `2 in effect`.
2. **Timing.** For two LOADED launches of each (A and C, and B if the level 1
   cost is wanted beside them), at the same `COOL` temperature:

   ```
   python tools/perf-summary.py device-logs/run148/loads-A-1/xe.log 60
   python tools/perf-summary.py device-logs/run148/loads-C-1/xe.log 60
   ```

   Compare fps per GHz (and frame ms at 2 GHz). Level 2 is worth making the
   default only if it fixes the ring crash and costs little against A.
3. The TranslationStats line of a C run gives the split of ordered, plain
   stack, plain read-only and barriers.

## Files

- `cpu/backend/a64/a64_emitter.cc`, `.h`: the setting's text,
  `GuestMemoryOrderLevel`, `IsReadOnlyGuestAddress`, level 1 limited to 1.
- `cpu/backend/a64/a64_seq_util.h`: `GuestAccessOrdered`, `GuestLdr*`,
  `GuestStr*`, `GuestOrderBarrier`.
- `cpu/backend/a64/a64_seq_memory.cc`: every guest load and store sequence,
  128-bit, MEMSET and the reserved loads.
- `cpu/backend/a64/a64_seq_vector.cc`: `lvlx`, `lvrx`, `stvlx`, `stvrx`.
- `cpu/compiler/translation_stats.h`, `compiler.cc`: the four counters.
- `cpu/backend/backend.h`, `a64/a64_backend.cc`, `.h`: `ClearReservation`.
- `kernel/guest_scheduler.cc`: the call in `SwitchTo`.

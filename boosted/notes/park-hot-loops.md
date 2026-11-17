# Park the hot loops (branch `claude/park-hot-loops`)

**Nothing here has been built, run on a device or run on the bench.** Cut
from `claude/integration-1116` at `7577c87`, not pushed. Evidence: run 146's
config D on Fable II (`device-logs/run146/xe-D.log`, `profile-D-combat/` on
the owner's PC). No speed-up is claimed for anything below until the phone
has run it.

## Done / in progress / next

- Done:
  1. Why `sub_82B9BF90` is not parked, from the log (below). The code bytes
     are not on the PC; the settings to get them are below.
  2. Why `PollSiteVisitFiber` costs 4.5%: about 950,000 calls a second into
     sites whose registers change at 97.5% of looks; and a recognition
     fault that most likely makes those sites (list walks taken for waits).
  3. Four settings: a per-site backoff with an inline counter, the stricter
     stateless rule, delay sleds as sites, one-block polls as sites
     (followup brief, item 2).
  4. Bench: a `delay_sled` waiter and four rows in `run-core.sh`.
- In progress: nothing.
- Next: the GitHub Actions build (app and bench), then the phone run under
  "The phone test". Then the brief's item 3 (register check for wide yield
  gaps), which this run's evidence does not bear on.

## What the evidence says

### `sub_82B9BF90` (3D Engine, F80000C4, Guest CPU 3)

45.6% of that thread's samples and 12.3% of all; self time equals children
time, so it calls nothing while hot. A thread dump has lr=82B9BF98, the
return address of its own prologue's `bl __savegprlr` (park-rework's
reading), so the thread was inside the function before any call of its own.

The log has one line naming a loop near it, logged twice per translation:

```
MemoryPollPark: loop at guest 82B9BFAC (1 block(s)) is not a pause-and-poll loop: it loads no guest memory
MemoryPollPark: parked poll loop at guest 82B9BFAC
```

So the loop at 82B9BFAC, 0x1C into the function, is one block, loads no
guest memory, and was accepted by the older one-block rule
(`TryInstrumentLoop`, `park_memory_poll_loops`). That rule takes a loop
without a load only as a **delay sled**: four or more pause hints
(`db16cyc`, `or r31,r31,r31`) a pass, a counter, a conditional branch back.
It is not collapsed, so its counter is not a constant `bdnz` count of 128 or
less (`collapse_ctr_spin_loops`) nor a counter kept in memory
(`collapse_memory_delay_spins`); most likely a general register or a CTR
loaded from an argument.

Why it is not parked:
- The `parked poll loop` line is logged when the loop is **marked**, not
  when a thread sleeps (the brief's item 2). The mark is a plain
  `SPIN_BACKOFF(1)`. Under the guest scheduler that is `ldrb
  preempt_requested; cbz`: it calls out only when the flag is up, about
  once a slice, and then only yields, which returns at once when nothing
  else is ready on CPU 3.
- It is not a poll site, so `guest_scheduler_park_waits` never sees it.
- Even as a poll site it would never park: its counter changes every pass,
  so the register test (`PollSiteVisit`) can never hold.
- It was never a candidate for `park_pause_poll_loops` as a loop of its
  own ("loads no guest memory"), and its caller's loop, which presumably
  tests something and calls it again, has a call in it
  (`set_return_address` refuses 6,285 candidates in this log).

What the loop does: it burns time (each pass is an `isb` on the phone, the
pause hints coalesced). Whether the thread waits for something by calling
it over and over, or delays once for long, the log cannot say. The fix
below does not depend on which: it parks a thread only while nearly all its
time goes to such sleds.

**To get the code bytes on the phone** (the ISO is not on the PC, no dump
exists in any device log): add to the config

```
dump_functions_at = "82B9BF90"
log_guest_calls_at = "82B9BF90"
log_guest_calls_limit = 200
```

`fndump_82B9BF90.txt` is written beside the log when the function is first
translated (PPC source, optimised HIR, host code); the call log gives each
entry's `lr` (the caller) and `r3`..`r10` (the count, if it is an
argument). A second run with the caller's function start added to
`dump_functions_at` shows the loop around the call. `tools/ppcdis.py`
disassembles raw words if needed.

### `PollSiteVisitFiber` (GameThread, Guest CPU 0)

`poll site looks 118484, registers changed 115594` a second: a look is every
eighth visit, so about 950,000 calls a second, each a full guest-to-host
call (`CallNativeSafe`), and each look hashes the whole register file.
97.5% of looks find the registers changed, so the site never parks.

The likely reason the site exists at all: the same shape is accepted in
about eighty functions,

```
pause-and-poll loop at guest XXXXXXXX: 3 block(s), 4 load(s), 0 pause(s), every pass the same unless memory changes
```

A wait is not inlined into eighty places in one form; a lookup is. The
stateless rule counted every loaded value as clean whatever its address, so
`p = p->next` (the pointer stored back is a loaded value) passed as "every
pass the same unless memory changes". Which of the sites is the hot one the
log does not say; the stats line now names the last site that backed off.

### `sub_82C62548` (Guest CPU 5): real work

5.6% of all samples. Its loops at 82C62574, 82C625D0, 82C626A0 and 82C626D8
are each refused with "floating-point or vector work in it" and, by the
older rule, "non-whitelisted opcode in body: convert". Conversion loops over
floating-point data: work, not a wait. Nothing changed for it.

### The followup brief's gaps, against this log

Reject reasons over all 58,552 `MemoryPollPark:` lines (each candidate is
logged once per translation; counts are lines, not loops):

| Reason | Lines |
|---|---|
| `reserved_store` (an atomic retry loop: work, rightly refused) | 12,559 |
| older rule: `reserved_load` | 12,520 |
| `store_offset` | 6,704 |
| no pause, and a register depends on an earlier pass | 6,673 |
| `set_return_address` (a call) | 6,285 |
| older rule: `store_offset` | 3,011 |
| older rule: `set_return_address` | 1,934 |
| floating-point or vector work | 1,399 |
| older rule: `branch_true` | 1,037 |
| a block entered from outside | 990 |
| `call_indirect` | 805 |
| accepted by the older rule (`parked poll loop`, 416 addresses) | 651 |
| `store` | 644 |
| it loads no guest memory | 262 |
| accepted as pause-and-poll | 184 |

1. **`IS_TRUE` / `IS_FALSE`: not supported by this log.** Neither appears
   in any reject line. The front end's tests reach this pass already folded
   into `branch_true`/`branch_false`, which are allowed. Nothing changed.
2. **One-block loops are not poll sites: confirmed** (the 651 lines are all
   plain backoffs; 82B9BFAC is one). Fixed behind `park_one_block_poll_loops`
   (memory polls, only with fixed load addresses) and `park_delay_sleds`
   (sleds, which the brief did not name and which are the hot one here).
3. **The yield gap**: the setting exists (`guest_scheduler_yield_spin_gap_us`,
   integration-1116); the register check for wide gaps is not written. This
   log is Fable II, whose yield spinners already park (75 to 107 a second);
   the case is Gears of War's. Next.
4. **`RtlLeaveCriticalSection` events**: the stats say the adaptive hold-off
   absorbs them (1,153 to 1,207 events a second inside a hold-off; 2 to 11
   early wakes a second still waiting). Nothing changed.

## Settings

| Setting | Group | Default | What |
|---|---|---|---|
| `park_poll_site_backoff` | CPU | **true** | Only acts on flagged sites (the opt-in settings below and `park_pause_poll_loops`). The site counts passes inline in `PPCContext::poll_site_skip` and calls the scheduler only on looks (one pass in eight); after 16 looks in a row with changed registers each further one skips `8 << level` passes, level up to 9 (4,096 passes); a look that finds them the same resets it. With `guest_scheduler_park_waits` off the site never calls except to give way. Read at translation and at start-up |
| `park_poll_loops_fixed_addresses` | CPU | **true** | Only read with `park_pause_poll_loops`. For the stateless rule a loaded value is clean only if its address is, so a walk along a list is not a wait |
| `park_delay_sleds` | CPU | false | With `guest_scheduler_park_waits`: a one-block sled of pause hints that loads nothing (as the older rule takes it) becomes a delay site, reporting every 64 passes. A thread whose 64 passes take no more than 64 us, look after look, for `guest_scheduler_park_wait_after_us` is parked as a yield spinner is |
| `park_one_block_poll_loops` | CPU | false | With `guest_scheduler_park_waits`: a one-block memory poll the older rule marks becomes a poll site, if every address it loads from is built from constants and registers the loop does not change |

**What changes with nothing set:** nothing in translated code or the
scheduler's paths: the two settings that default to true are read only for
sites that exist under `park_pause_poll_loops`, `park_delay_sleds` or
`park_one_block_poll_loops`. `PPCContext` gains one 32-bit field after
`last_safepoint_pc` (after `thread_id`, so outside the hashed registers;
existing offsets unchanged). The older rule's log line is unchanged for a
plain backoff.

**What changes on the phone's config D** (`park_pause_poll_loops = true`,
`guest_scheduler_park_waits = true`): the two defaults act. Fewer
pause-and-poll sites (walks refused at translation), and the remaining sites
call the scheduler one pass in eight instead of every pass.

## Why none of this can delay a guest past its bound or change results

- **Backoff and skip.** They only decide when the scheduler looks. A park is
  still decided exactly as before (the same register hash at two looks no
  more than 400 us apart, for 200 us), and still bounded as before. Fewer
  looks can only mean fewer or later parks, which cost the guest nothing.
  The preemption flag is still tested inline on every pass, so a give-way
  is as prompt as before. A site that becomes a real wait is found at the
  second look after its skip ends, at most about 4,100 passes later.
- **Stricter stateless rule.** It removes sites, never adds one.
- **Delay sleds.** A sled computes only its counter, which a park leaves as
  it was; nothing is removed from the loop. The park is the existing
  `TryWaitPark`: a quarter of the burst so far, at least 100 us, at most
  `guest_scheduler_park_wait_max_us` (1 ms), ended early by events as
  learnt. The code after the sled, which tests what the thread waits for,
  runs late by at most one park, the same bound as for a yield spinner. A
  single delay is made longer by at most a quarter of its own length so
  far, and only if it runs for 200 us. A thread that does work between
  delays (more than 1 us per sled pass on average) is never parked.
- **One-block poll sites.** The same run-time test as the pause-and-poll
  sites; the pass only adds the call, behind fixed load addresses.

Risks: a title that uses a long counted sled for timing it cares about (an
audio or hardware delay) would see it lengthened by up to a quarter; a
contended spin lock whose back-off is such a sled would take it up to one
park later. Both are why `park_delay_sleds` is off by default. With
`park_poll_site_backoff = false` and `park_delay_sleds = true` every sled
pass calls the scheduler, which makes each pass several times slower: do not
run that combination except to compare on the bench.

## The phone test

On top of config D (`device-logs/run146/config-D.toml`), Fable II, the same
combat tutorial spot:

```
park_delay_sleds = true
park_one_block_poll_loops = true
log_memory_poll_park = true
dump_functions_at = "82B9BF90"
log_guest_calls_at = "82B9BF90"
log_guest_calls_limit = 200
```

(`park_poll_site_backoff` and `park_poll_loops_fixed_addresses` are true by
default; `guest_scheduler_park_waits`, `park_pause_poll_loops`,
`guest_scheduler_stats` are already on in config D.) Then the same profile
(`tools/profile-all.sh`, 180 s).

What the log shows if it works:
- At translation: `MemoryPollPark: parked poll loop at guest 82B9BFAC (a
  delay site: a sled of pause hints that loads nothing)`, and
  `dump_functions_at: wrote .../fndump_82B9BF90.txt`.
- Far fewer `pause-and-poll loop ... every pass the same unless memory
  changes` lines; the three-block four-load ones should now read `it does
  not pause, and a register it writes depends on an earlier pass` if they
  are walks.
- The stats line: `poll site looks` and `registers changed` down from
  about 118,000 a second to hundreds or a few thousand; `poll site
  backoffs N (last at XXXXXXXX)` names a hot site; `delay sled looks N,
  parks M` with M above 0 if the 3D Engine thread waits in the sled; `poll
  loops` (parks at sites, delay parks included) above 0 and `us parked in
  all` up.
- The profile: `PollSiteVisitFiber` near nothing; Guest CPU 3 below 98% of
  a core only if the sled is a wait. If `delay sled looks` is high and
  `parks` 0, the thread does work between delays and the sled is not the
  wait: the dump and the call log then say what is.

What to switch off if it misbehaves: `park_delay_sleds` first (stutter,
audio timing, a slower frame), then `park_one_block_poll_loops`, then
`park_poll_site_backoff = false`.

## Bench (`boosted/bench/core`)

New waiter `delay_sled` (`bench_sched.cc`): tests the flag, calls a leaf
that counts r10 down from `BENCH_DELAY_SLED_PASSES` (64) with four
`or r31,r31,r31` a pass, and goes round. It runs in every
`BENCH_BUSY_WAITS=1` row. Expected: busy (about one core) in every row
without `park_delay_sleds`; parked in the two rows with it.

New rows in `run-core.sh`: poll sites without the backoff, poll sites with
the stateless rule as before, poll sites with delay sleds and one-block
sites, delay sleds without the backoff. The older rows now run with the two
defaults on; the first two new rows are the comparison. The rig has no list
walk, so the stricter rule changes nothing it measures.

## Upstream files changed (for `ARCHITECTURE.md`)

- `cpu/ppc/ppc_context.h`: `poll_site_skip` after `last_safepoint_pc`.
- `kernel/xthread.h`: `SchedulerLinks::poll_refusals`, `poll_backoff_level`.
- Fork files: `cpu/compiler/passes/memory_poll_park_pass.{h,cc}`,
  `cpu/backend/a64/a64_seq_memory.cc` (the `SPIN_BACKOFF` site: the flag
  alone selects the site path now, plus the inline skip),
  `kernel/guest_scheduler.{h,cc}` (`DelaySiteVisit`, the backoff in
  `PollSiteVisit`, the stats line), `boosted/bench/core/*`.

## Text for `HANDOFF.md` (not edited here)

- `claude/park-hot-loops` (from `claude/integration-1116`, not pushed, not
  built): run 146's hot loops. `sub_82B9BF90` holds a delay sled (82B9BFAC)
  the older rule marked with a plain backoff, which only yields once a
  slice; `PollSiteVisitFiber` was about 950,000 calls a second, most likely
  into list walks the stateless rule took for waits. New: `park_delay_sleds`
  and `park_one_block_poll_loops` (off), `park_poll_site_backoff` and
  `park_poll_loops_fixed_addresses` (on, only under `park_pause_poll_loops`).
  Notes `boosted/notes/park-hot-loops.md`; phone test there, with the
  settings that dump the function.

## What the build and the rig cannot verify

- The C++ has not been compiled. Points to check in the first build log:
  the inline `ldr`/`str` of `poll_site_skip` (the static asserts on its
  offset), the template `ValueSourcesClean`, the new stats fields.
- That 82B9BFAC is where the 3D Engine thread spends its time (only the
  dump and the profile after the change can say), and that the three-block
  loops are walks.
- That `park_one_block_poll_loops` finds any loop with fixed addresses in a
  title: the log lists which become sites.

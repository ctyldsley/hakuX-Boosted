# Memory watches (`claude/memory-watches`)

**Nothing here has been run on a device. No speed-up is claimed anywhere in
this file.** The only device facts are the ones in `OVERNIGHT-2.md` (the run
90 profile). What was run is a test on an x86-64 Linux container, described
under "What was checked, and what was not"; it says the watches still fire,
not that anything is faster on the phone.

## Status

Done (all behind settings that default to off, built by GitHub Actions):

- `memory_watch_log`: a once-a-second line with every count the task asked
  for (calls to `mprotect`, pages changed, watches armed and lifted, faults
  taken, time in the fault handler) and a few more that say which fix would
  pay.
- `memory_watch_lean_fault_handler`: the signal handler no longer takes a
  mutex and searches every fibre stack before it looks at a guest-memory
  fault.
- `memory_watch_bridge_runs`: one host call across pages that already have
  the wanted protection, instead of one call per run of pages.
- `memory_watch_arm_ahead_kb` (with `memory_watch_arm_ahead_spurious_limit`):
  when a watch has to be armed, the whole surrounding block is armed in the
  same call, so later requests inside it cost no call.
- `memory_watch_uffd`: write watches kept with userfaultfd write-protection
  instead of `mprotect`, with a clean fall back.
- `BaseHeap::QueryRangeAccess` stops at the first unreadable page (same
  answer, less work; no setting, see "Upstream files changed").
- A test of all of it (`base/memory_watch_test.cc`, not built into the app).

In progress: nothing. The code built green as GitHub Actions run 108.

Next (not started, in the order I would do them):

1. Read the `memory watch:` line on the phone with every switch off. It
   decides everything below: calls a frame, pages a call, faults a second.
2. If arming dominates (many `armed` calls of few pages each), try
   `memory_watch_arm_ahead_kb = 256`. If faults dominate, the fix is in
   `gpu/` (recommendation 1 below), not here.
3. If `guest protect ... (N unchanged)` is large, add the table check that
   skips a guest protection call asking for what the pages already have. It
   is counted and not built, because it needs the watch to be lifted in a
   different order to stay correct (see "Not built, and why").
4. The three physical views are walked one after another, each taking the
   global lock. One lock for the three, and skipping a view that has nothing
   committed in the range, would cut `EnableAccessCallbacksInner`'s 0.9%.

## The life of a watch, with the cost at each step

Names are in `xenia/memory.cc` unless a path is given. A "system page" is a
host page (4 KB on most phones, 16 KB on some); the code handles both.

1. **A cache copies guest memory and asks for a watch.** The GPU command
   thread wants a vertex buffer or texture: `SharedMemory::RequestRange`
   (`gpu/shared_memory.cc`) finds the pages it has no valid copy of and calls
   `VulkanSharedMemory::UploadRanges`. For each run of pages:
   `heap->QueryRangeAccess` (global lock, a table read per 4 KB page), then
   `MakeRangeValid` (global lock, sets the valid bits), then
   `Memory::EnablePhysicalMemoryAccessCallbacks`, then the copy (the
   non-temporal `memmove` in the profile: `vastcpy` is plain `memcpy` on
   ARM64 and bionic picks that variant for large sizes). The watch is armed
   before the copy on purpose: a write that lands after the copy must fault.
   The primitive processor (`gpu/primitive_processor.cc`) asks the same way
   for index buffers it has converted.
2. **Arming.** `EnablePhysicalMemoryAccessCallbacks` calls
   `PhysicalHeap::EnableAccessCallbacks` on each of the three guest views of
   physical memory (`0xA0000000` with 64 KB pages, `0xC0000000` with 16 MB,
   `0xE0000000` with 4 KB), because the title may write through any of them.
   Each takes the global lock and runs `EnableAccessCallbacksInner`: for
   every system page in the range it reads the guest protection
   (`SystemPageGuestAccess`, a page table read), and if the page is
   guest-writable and not yet watched it sets the page's bit in
   `notify_on_invalidation` and adds it to a run; each run ends in one
   `mprotect(PROT_READ)`. A view where the range is not committed does the
   table reads and nothing else. Cost: three lock round trips, a table read
   per page per view, and one `mprotect` per run. The run is usually the
   whole request, so this is one `mprotect` per upload range, which with
   many small draws is one per draw. Each `mprotect` takes the process's
   mmap lock for writing, splits the mapping at both ends of the range (and
   merges it again later), rewrites the page table entries and flushes them
   from the TLB of every core the process is on.
3. **The title writes.** The store faults (`SIGSEGV`). The kernel delivers
   the signal on the thread's alternate stack to
   `ExceptionHandlerCallback` (`base/exception_handler_posix.cc`), which:
   asks `Fiber::IsStackOverflowFault` (a `std::mutex` and a walk of every
   registered fibre stack, on every fault); copies the registers (31 general,
   32 vector); works out read or write from the fault status register; and
   calls the handlers in the order they were installed. The first is
   `MMIOHandler::ExceptionCallback` (`cpu/mmio_handler.cc`): not an MMIO
   address, so it takes the global lock and calls
   `Memory::AccessViolationCallback`, which finds the heap and calls
   `PhysicalHeap::TriggerCallbacks` for the one faulting page.
4. **The callbacks fire.** `TriggerCallbacks` checks the page is watched (if
   not, another thread got there first and the write is retried), then calls
   every invalidation callback under the global lock.
   `SharedMemory::MemoryInvalidationCallback` widens the range to the
   surrounding 64 system pages (256 KB at 4 KB pages) as far as pages the GPU
   itself wrote allow, clears the valid bits, and fires the texture cache's
   and others' range watches (`FireWatches`). The primitive processor's
   callback drops cached index buffers. The range every callback agreed to
   (clamped to 4 MB) is then unwatched: one `mprotect(PROT_READ|PROT_WRITE)`
   per run of watched pages in it, and the bits are cleared. No allocation
   anywhere on this path. Locks: the fibre registry's mutex and the global
   lock (which the GPU command thread also holds while it arms, including
   across its `mprotect` calls, so a faulting title thread can wait behind
   it).
5. **The handler returns**, the kernel restores the context and the store is
   retried and succeeds. The pages of the widened range are now unwatched:
   further writes to them are free until the cache copies them again, at
   which point step 1 arms them again.

So a buffer the title rewrites every frame costs, each frame: one fault and
one or more `mprotect` calls to lift (step 4), and one `mprotect` per upload
range to arm again (step 2). If the title writes the next draw's data into a
page the GPU thread has just armed for the previous draw (two draws' vertices
sharing a page, the GPU thread keeping up with the title), every draw costs a
fault and two `mprotect` calls. The test's `pattern=interleaved` shows that
case and `pattern=lag` the other (title writes the frame, then the GPU thread
uploads it); the per-frame counts are in the table further down.

Other ways a watch ends, all in `memory.cc`: the title frees or decommits the
memory (`PhysicalHeap::Release`, `Decommit`), allocates it (`Alloc`, this
fork's fix for the Fable II video), or changes its protection to writable
(`PhysicalHeap::Protect`). Each raises the callbacks for the whole range
without a fault.

## What the profile's symbols are, as far as the source can say

- `mprotect` 7.1% on GPU Commands: steps 2 (arming, on that thread) and the
  guest protection calls are the only callers on it. Step 4's are on the
  title's threads (the 1.35% on 3D Engine).
- `PhysicalHeap::EnableAccessCallbacksInner` 0.9%: step 2's page loop.
- `BaseHeap::QueryRegionInfo` 1.0% on GPU Commands: **no code on that thread
  calls it.** Its only callers are in `kernel/xboxkrnl/xboxkrnl_memory.cc`
  (title threads). The function the GPU thread does call, once per upload
  range, is `BaseHeap::QueryRangeAccess`, four functions further down the
  same file. My reading is that the samples are `QueryRangeAccess` under the
  nearest symbol the profiler had; that is a guess, to settle with a profile
  of this build (the early exit added here changes its cost either way).
- `__memmove_aarch64_nt` 7.8%: the upload copy in step 1. It falls only if
  fewer pages are invalidated, which nothing here does. Arming ahead does not
  invalidate more than today either (argued below).

## Settings, in the order to try on the device

All in the `[Memory]` section of the 360 config. One at a time, each on top
of the ones before it once it has been seen to work. Build the config on the
PC, push it whole, read it back (the rule in `HANDOFF.md`).

| Step | Setting | Value | What to look at |
|---|---|---|---|
| 0 | `memory_watch_log` | `true` | The `memory watch:` line once a second, with everything else off. This is the baseline. |
| 1 | `memory_watch_lean_fault_handler` | `true` | `us in handler` per fault against step 0. Nothing drawn can change. |
| 2 | `memory_watch_bridge_runs` | `true` | `lifted N calls` and `mprotect N calls` against step 1; `bridged` says how many pages were crossed. |
| 3 | `memory_watch_arm_ahead_kb` | `64`, then `256` | `armed N calls` should fall; `faults ... ahead N` must stay small. If `ahead` faults are a large share of all faults, or the frame rate falls, switch it off: the title writes next to what the GPU reads. |
| 4 | `memory_watch_uffd` | `true` | At start-up: `Memory: write watches use userfaultfd write-protection`, or a line saying why not. Then `mprotect N calls` should fall to the guest's own, `uffd N ioctls` take their place, `failed` must be 0. Watch for corrupted textures and for a freeze. |

`memory_watch_arm_ahead_spurious_limit` (default 4) only matters with step 3:
how many wasted faults a block of 64 host pages is allowed before nothing
more is armed ahead in it. Leave it.

Steps 1 and 2 are the safe ones. Step 3 can make things slower (not wrong).
Step 4 is the one that could be wrong on the phone in ways the test here
cannot see, so it goes last and alone at first.

## The log line

```
memory watch: 1000 ms: mprotect 412 calls 3120 pages; armed 380 calls 2900
pages for 395 requests of 3000 pages (ahead 0 bridged 0); lifted 30 calls 210
pages; uffd 0 ioctls 0 failed; faults 28 signals 190 us in handler (longest
41 us): watch 27 ahead 0 read 0 raced 1 unhandled 0; announced 12; guest
protect 2 (0 unchanged)
```

(The numbers above are made up to show the format.) Per interval:

- `mprotect N calls M pages`: every `mprotect` made through
  `xe::memory::Protect`, whoever asked: watches, the guest's own protection
  calls, commits.
- `armed N calls M pages for R requests of P pages`: host calls made to arm
  watches (step 2), against how many times a cache asked and for how much.
  Calls well below requests means the pages were already watched. `ahead`:
  pages armed without being asked for. `bridged`: pages inside a call only
  because the run crossed them.
- `lifted N calls M pages`: host calls made to lift watches (step 4 and the
  no-fault paths).
- `uffd N ioctls F failed`: of the armed and lifted calls, how many were
  userfaultfd ioctls. `failed` must be 0.
- `faults N signals T us in handler (longest L us)`: every `SIGSEGV` and
  `SIGBUS` delivered, with the time from the handler's entry to its exit
  (so not the kernel's delivery and return, which the profile has to show).
  Then what the guest-memory ones found: `watch` (a watch fired), `ahead` (a
  page armed ahead and never asked for: wasted), `read` (a read watch),
  `raced` (another thread had already lifted it), `unhandled` (a real
  violation by the title).
- `announced`: callbacks raised without a fault (allocate, free, protect).
- `guest protect N (U unchanged)`: the title's own protection changes on
  physical memory, and how many asked for what every page already had.

The line is printed from the arming path, so a second with no upload prints
nothing and the next line covers a longer interval (the `ms` says how long).
With `memory_watch_log` off nothing is counted and the handler tests one
flag.

To compare runs: the same scene, the same number of seconds, then per second
`mprotect calls`, `faults`, `us in handler`; and from a profile the share of
`mprotect` (and `ioctl` with step 4) on GPU Commands and on 3D Engine.

## The changes, and why each is correct

### `memory_watch_lean_fault_handler`

`Memory::Initialize` gives the handler the address range of the guest
mapping (`ExceptionHandler::SetNoStackOverflowRange`); a `SIGSEGV` whose
address is inside it skips `Fiber::IsStackOverflowFault`.

Correct because that call only decides whether to print "Fiber stack
overflow" before the handlers run, and a fibre's guard page is host memory
that can never lie inside the guest mapping, so for those addresses its
answer was always no. Faults outside the range are checked as before.

### `memory_watch_bridge_runs`

The invariant the watch code keeps: a system page the guest holds
read-write is read-only on the host exactly when its write-watch bit is set
(and has no access when its read-watch bit is set).

- Arming: a run of pages to protect read-only may now carry on across a page
  that is guest read-write, write-watched and not read-watched. By the
  invariant that page is already read-only, so including it in the
  `mprotect` changes nothing for it. The run still ends at any other page.
  Pages the guest itself holds read-only are not crossed, though they should
  be read-only on the host too: that rests on the guest's protection calls
  and not on the watch code, and the gain was not worth resting on it.
- Lifting: a run of pages to make read-write may carry on across a page that
  is unwatched and guest read-write. By the invariant it is already
  read-write. Watched pages the guest does not hold read-write still end the
  run and are not unprotected, as before.

The kernel does nothing for the part of an `mprotect` range that already has
the protection asked for, so a bridged call costs about what the first run
would have.

### `memory_watch_arm_ahead_kb`

When a write-watch request finds at least one page of its range unwatched (so
a host call is about to be made anyway), the range is widened to the
surrounding block of the given size, and every guest-writable page in the
block is watched in the same call. The extra pages are ordinary watches: bit
set, page read-only, callbacks fire on a write. They are also marked in a
second bitmap (`armed_ahead`) so that a fault on one can be counted.

Correct because the interface already allows it. `memory.h` says callbacks
"may be triggered spuriously, and handlers should properly ignore pages they
don't care about". A page armed ahead that no cache holds fires callbacks
that find nothing valid to invalidate. Nothing is ever watched less than
before, so no write that was caught before can be missed.

It does not invalidate more than today: a fault already unwatches and
invalidates the surrounding 64 pages (the shared memory's own widening). The
cost it can add is faults, on the title's threads, where the title writes to
a page beside what the GPU reads but that the GPU never reads itself. Today
such a page is never watched. Each such fault is counted against its block
of 64 host pages (`ahead_spurious`), and at
`memory_watch_arm_ahead_spurious_limit` nothing more is armed ahead there, so
the worst case is that many wasted faults per block, once.

The data-provider (read watch) path is untouched.

### `memory_watch_uffd`

At start-up `Memory::Initialize` opens a userfaultfd, asks for
write-protect faults on shared memory delivered as `SIGBUS`, and registers
the three physical views (`0xA0000000` to `0xFFFFFFFF`) as one range. If any
step fails the reason is logged and the heaps keep to `mprotect`. If all
succeed, in those three heaps:

- arming a write watch is `UFFDIO_WRITEPROTECT` with the protect mode;
- lifting is one `UFFDIO_WRITEPROTECT` without it over the whole unwatched
  range;
- a write to a protected page raises `SIGBUS` with the faulting address,
  which the same handler already receives, and it takes the same path as a
  `SIGSEGV` from there on.

Why it should be cheaper: the ioctl takes the mmap lock for reading, not
writing, and changes only page table entries; the mappings are never split
or merged. It still flushes the TLB. How much that is worth on the phone is
not known.

Why it is correct:

- The kernel's write-protect bit is a second, independent restriction on
  the page; the guest's own protection is still `mprotect`'s. So the watch
  invariant becomes: the bit is set exactly when the write-watch flag is.
  Arming sets both under the global lock. Lifting clears the bit for the
  whole range whose flags are about to be cleared, whatever the caller's
  `unprotect` argument and whatever the guest protection, because unlike an
  `mprotect` the bit would otherwise outlive its flag. (Under `mprotect`,
  callers that pass `unprotect = false` reset the host protection
  themselves straight after; nothing resets a write-protect bit.)
- Read watches stay on `mprotect` (no access). Where a page has both, arming
  sets the bit regardless, and when the read watch is satisfied the page
  goes back to the guest's protection, the bit still stopping writes.
- If a fault arrives on a page with no flag (another thread lifted it
  first), the write is retried as before; with userfaultfd the handler first
  clears the bit on that page, which is a no-op where it is already clear
  and prevents an endless fault if one were ever left behind.
- If an arming ioctl ever fails, that range is protected with `mprotect`
  instead (the watch is never skipped), a warning is logged once, and from
  then on lifting does both. `uffd ... failed` in the log line counts these.
- Both ioctls are plain system calls, safe in the signal handler; `errno`
  is saved and restored.
- A write made by the kernel on the title's behalf into a protected page
  (a `read()` straight into guest memory) fails with `EFAULT` under both
  mechanisms, so nothing changes for the host code that already announces
  such writes.

What Android allows an app, as far as I could establish without the phone:

- **seccomp**: the app filter kills a process that makes an unlisted system
  call. `userfaultfd` was added to bionic's `SECCOMP_ALLOWLIST_COMMON.TXT`
  in March 2021 for ART's collector, so it is allowed from Android 12 (API
  31). The code does not make the call below API 31 and says so in the log.
  (Source: a web search of mirrors of that file; `android.googlesource.com`
  is not reachable from this session, so I could not read the current file
  itself.)
- **`vm.unprivileged_userfaultfd`** is 0 on Android, which permits an
  unprivileged process a descriptor created with `UFFD_USER_MODE_ONLY`
  (faults taken in user mode only). That is what is asked for, and it is all
  a watch needs.
- **SELinux**: from memory, not checked tonight, the platform policy lets
  every app domain create and use its own userfaultfd
  (`userfaultfd_use(appdomain)`), again for ART. If the phone's policy
  denies the call or an ioctl, the call returns an error and the code falls
  back with a log line. It cannot crash on a denial.
- **Kernel**: write-protect mode needs architecture support, which ARM64
  gained in Linux 6.10, and for shared memory `UFFD_FEATURE_WP_HUGETLBFS_SHMEM`
  (5.19, with `CONFIG_PTE_MARKER_UFFD_WP`); both versions are from memory. A phone on an older kernel, or
  one built without it, reports fewer features; the handshake fails and the
  log line prints the features offered and needed. I do not know which
  kernel the SM8850 runs under Android 17.
- **The mapping**: guest memory comes from `ASharedMemory_create`, which is
  memfd or ashmem depending on the device. Both are shmem underneath, but
  whether the kernel accepts an ashmem mapping for registration I could not
  check; if it does not, registration fails and the log says so.
- **soft-dirty** (`/proc/self/pagemap` and `clear_refs`) is not an option:
  ARM64 kernels do not implement it, and clearing is per process, not per
  range.
- Not built: userfaultfd's asynchronous mode with `PAGEMAP_SCAN` (Linux
  6.7), where the kernel records writes and nothing faults at all. The
  caches would have to ask for dirty pages before every use instead of
  being told, which is a change to `gpu/shared_memory.cc` and the texture
  cache, a system call per draw unless batched per frame, and a different
  correctness argument (a draw must see writes made just before its
  command). Worth a design note once the counters say faults are the cost.

### `BaseHeap::QueryRangeAccess` early exit (no setting)

The loop stopped setting a flag and now also leaves at the first page
without read access. The function returns "no access" whenever any page is
unreadable, whatever the rest are, so the answer is identical for every
input. It is called once per upload range by `UploadRanges`.

## Not built, and why

- **Skipping guest protection calls that change nothing** (the task's "track
  the current protection per page so the check is a table read"). The table
  exists (`page_table_[].current_protect`) and the arming path already uses
  it with the watch bitmaps, which is why arming never calls `mprotect` for
  a page already watched. The remaining candidate is `BaseHeap::Protect`
  when the title asks for the protection the pages have. It is not a free
  skip: `PhysicalHeap::Protect` lifts the watch flags first with
  `unprotect = false` and relies on the `mprotect` that follows to make the
  pages writable again; skip that `mprotect` and the page is left read-only
  with no flag, and the next write faults for ever. It needs the watch lifted
  with `unprotect = true` on that path. The log line now counts how often it
  would apply (`guest protect N (U unchanged)`); build it if U is large.
- **Re-protecting after a frame's uploads instead of per range.** A watch
  must be armed before its pages are copied, or a write between the copy and
  the arming is lost and the copy stays stale for ever. So arming cannot be
  deferred to the end of the frame without the kernel telling us afterwards
  what was written in between (the asynchronous mode above). Arming ahead is
  what can be done within that rule: one call per block per frame, made
  before the first copy.
- **Protect-then-unprotect pairs.** I looked for them: `Release` and
  `Decommit` lift the watch and then change nothing on the host (unless
  `protect_on_release`, off), `Alloc` and `Protect` pass `unprotect = false`
  for exactly this reason. There is no pair left to remove on the default
  settings.

## Upstream files changed, and why

All under `x360/emulator-core/src/main/cpp/xenia/src/xenia/`, all marked
`hakuX-Boosted:`.

- `memory.cc`: the five settings; the log line (`LogWatchStatsIfDue`);
  registration of the views with userfaultfd and the handler's range in
  `Memory::Initialize`; `PhysicalHeap::EnableAccessCallbacks` (arming ahead,
  two passes for read-and-write watches under userfaultfd);
  `EnableAccessCallbacksInner` (runs with a first and last page, bridging,
  the ahead bitmap, counters); `WatchHostProtect` and
  `WatchHostKernelUnprotect` (new, the host side of a watch);
  `TriggerCallbacks` (the `from_fault` argument, counters, the ahead count,
  lifting by ioctl, bridging); `PhysicalHeap::Protect` (counters);
  `QueryRangeAccess` (early exit); `AccessViolationCallback` (passes
  `from_fault`).
- `memory.h`: the declarations for the above; `armed_ahead` and
  `ahead_spurious` in `SystemPageFlagsBlock`.
- `base/memory.h`, `base/memory.cc`: `WatchCounters` and its switch; the
  three `WriteWatch*` functions' declarations.
- `base/memory_posix.cc`: counting in `Protect`; the userfaultfd wrapper.
- `base/memory_win.cc`: stubs for the wrapper.
- `base/exception_handler.h`, `_posix.cc`, `_win.cc`:
  `SetNoStackOverflowRange`; the timing wrapper around the signal handler
  (`ExceptionHandlerCallbackTimed`), which is what is now installed.
- `base/memory_watch_test.cc`: new, the test. The build skips `*_test.cc`
  and the file is empty without `XE_MEMORY_WATCH_HARNESS`.

## Changes I recommend in files I do not own

1. **`gpu/shared_memory.cc`, `gpu/vulkan/vulkan_shared_memory.cc` (owner:
   `claude/shader-first-run`): do not watch the page a vertex buffer request
   ends in the middle of.** This is the fix for the fault-per-draw case
   (`pattern=interleaved` below), which nothing in my files can touch. When
   a draw's vertex data ends part-way through a page, the title's next write
   is very likely into that same page. Today the page is made valid and
   watched, the title faults on it, the surrounding 256 KB is invalidated,
   and the next draw arms again. Instead: upload the page but leave it
   invalid and unwatched. The next request that includes it copies those
   4 KB again and nobody faults. Shape of the change: give
   `SharedMemory::RequestRange` a flag (say `bool tail_page_volatile`),
   passed as true only from the vertex buffer binding path in the command
   processor; in `VulkanSharedMemory::UploadRanges` when the flag is set and
   `(start + length)` is not page-aligned, call `MakeRangeValid` for the
   pages before the last one only (the copy still covers all of them).
   **Not for textures or anything that registers a range watch**
   (`WatchMemoryRange`): the texture cache relies on the invalidation
   callback to learn a texture changed, and a page that is uploaded but not
   watched would leave it stale. Vertex buffers are safe because every draw
   asks again. Behind a setting; `faults ... watch N` in the log line is the
   number to compare.
2. **`gpu/shared_memory.cc` `MakeRangeValid` (line 400):** it takes the
   global lock to set the valid bits, drops it, and then
   `EnablePhysicalMemoryAccessCallbacks` takes it three more times. Holding
   it once across both is a small saving on every upload and closes nothing
   (the order valid-then-watch is unchanged).
3. **`base/threading_fiber.cc` `Fiber::IsStackOverflowFault` (line 389,
   owner: `claude/guest-waits`):** it locks `stack_registry_lock_`
   (`std::mutex`) inside a signal handler. If the signal interrupts a thread
   that holds that mutex (creating or destroying a fibre), the thread
   deadlocks on itself. `memory_watch_lean_fault_handler` avoids the call
   for guest-memory faults only. The proper fix is a registry the handler
   can read without a lock: a fixed array of `std::atomic<uintptr_t>` guard
   bases, written under the mutex by the two places that register and
   remove (lines 237 and 248), read with plain loads by the handler.
4. **`cpu/mmio_handler.cc` `ExceptionCallback` (line 459, owner:
   `claude/translator-wide`):** nothing wrong. Noted only that every watch
   fault takes the global lock there before it knows the page is watched;
   a lock-free read of the watch bitmap first would let the "another thread
   already lifted it" case return without the lock. Small; `raced` in the
   log line says how often it happens.
5. **Settings screens and the bundled config (owner:
   `claude/defaults-config`):** nothing needed. The settings register
   themselves and default to off. If they are to be reachable from the app
   they are five rows under Memory; I would not expose
   `memory_watch_arm_ahead_spurious_limit`.
6. **`boosted/docs/ARCHITECTURE.md`:** copy the "Upstream files changed"
   list in at the merge.

## What was checked, and what was not

Checked by the build: it compiles for the phone (run 108, green; see "Build
results" at the bottom).

Checked on an x86-64 Linux 6.18 container, with the real `memory.cc`,
`mmio_handler.cc` and `base/` compiled by clang and driven by
`base/memory_watch_test.cc`:

- The test keeps its own copy of guest pages the way the GPU's shared memory
  does and, after every operation, compares every page it believes valid
  with guest memory. A watch that fails to fire shows as a difference.
  Dropping one arming call in fifty on purpose is caught within a few
  thousand operations (I checked, after a first version of the test that
  verified too rarely missed it).
- 45 runs of 60,000 random operations (three seeds, fifteen combinations of
  the settings, with and without two or three writer threads) and 30 more of
  40,000 with the lean handler added: no stale page, no hang, no unhandled
  fault, `uffd failed` 0. Userfaultfd was really in use in those runs (the
  container's kernel offers it).
- A separate small program confirmed the kernel behaviour the design rests
  on, on that kernel: write-protect on a shared memfd mapping, `SIGBUS` with
  the faulting address on populated and unpopulated pages, the bit kept
  across an `mprotect` round trip, `EFAULT` for a kernel write, clearing a
  range with nothing protected succeeds, an unregistered range is refused.

Host calls and faults per frame on the test's synthetic vertex buffer (40
chunks of 7.5 KB rewritten at the same addresses each frame). **This is a
count on a made-up pattern, not a measurement of any title and not a time.**

| Settings | `lag`: mprotect | ioctl | faults | `interleaved`: mprotect | ioctl | faults |
|---|---|---|---|---|---|---|
| none | 42 | 0 | 2 | 81 | 0 | 39 |
| `bridge_runs` | 42 | 0 | 2 | 79 | 0 | 39 |
| `arm_ahead_kb = 64` | 7 | 0 | 2 | 80 | 0 | 39 |
| `arm_ahead_kb = 256` | 4 | 0 | 2 | 81 | 0 | 39 |
| `uffd` | 0 | 42 | 2 | 0 | 79 | 39 |
| `uffd`, `arm_ahead_kb = 256`, `bridge_runs` | 0 | 4 | 2 | 0 | 79 | 39 |

So if the phone's log shows the `lag` shape (many arming calls, few faults),
arming ahead is the lever; if it shows the `interleaved` shape (a fault per
draw), only recommendation 1 helps and userfaultfd merely makes each step
cheaper.

Not checked, and the build cannot check:

- Anything on ARM64, Android, the phone's kernel or with a title.
- Whether userfaultfd is granted to the app on the phone, whether its kernel
  has write-protect for shared memory, and whether an ashmem-backed mapping
  can be registered. The start-up log line answers all three.
- Whether `SIGBUS` from userfaultfd carries the fault status register on
  the phone's kernel (the handler falls back to decoding the faulting
  instruction if it does not, as it already does for `SIGSEGV`).
- 16 KB host pages. The code paths are the ones upstream already had for
  them, but the test ran with 4 KB pages only.
- Whether any of this is faster. On the phone the profile showed 7.1% of the
  GPU command thread in `mprotect`; what part of that each switch removes is
  for the log line and a new profile to say.

## Build results

- First push (`7b40ebf`, counters, bridging, arming ahead, userfaultfd):
  GitHub Actions run 104. hakuX's own core built; the APK job (the 360 core)
  was still compiling after 45 minutes when the second push replaced it, so
  it has no result of its own.
- Second push (`31ec107`: all the code, the test, these notes): GitHub
  Actions run 108, green, both jobs (hakuX's core and the APK with the 360
  core). That is the build to install. The commit after it changes only this
  file.

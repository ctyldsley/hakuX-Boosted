# Roadmap

Ordered by what is known to block the goal (Fable II and titles like it
playable and smooth on Snapdragon), then by expected gain against effort.
Every performance item is stated with how to tell whether it worked,
following the method in `x360/docs/gw-gpu-bottleneck-investigation.md`
section 10: warm run (second launch), cvar confirmed in `xe.log`, one driver
build pinned, differences under 2 fps treated as noise.

## Phase 1: this version

- [x] XenDroid in the tree as an isolated subtree (`x360/`), its submodules mapped.
- [x] One app with both cores; 360 images routed from hakuX's library and
      from frontends' launches.
- [x] KGSL context priority for custom Turnip drivers on the 360 side.
- [x] Per-title defaults mechanism; Fable II's best-known config shipped.
- [x] Build of the whole app on GitHub Actions.
- [ ] First run on a device. See `STATUS.md` for the checks to make.

## Phase 2: zero-copy guest memory on Adreno

The largest piece of hakuX that the 360 side lacks (`CROSSOVER.md`, item 1).

What exists: XenDroid already has the consumer side. With
`shared_memory_zero_copy` the Vulkan backend treats guest physical memory as
the shared-memory buffer and skips uploads
(`gpu/vulkan/vulkan_shared_memory.cc`, `TryInitializeZeroCopy`). It is dead
on Adreno because the import uses `VK_EXT_external_memory_host`.

What to build:

1. **Backing.** Allocate the 512 MB of guest physical memory as an
   AHardwareBuffer (BLOB format, CPU read and write usage only, so it stays
   CPU-cached), find the dma-buf fd in its handle, and map that fd at the
   physical views (0xA0000000, 0xC0000000, 0xE0000000 and the 4 GB mirror)
   in `Memory::Initialize` / `MapViews` (`xenia/memory.cc`) in place of the
   shared memory file's physical range. hakuX's `hw/xbox/hle/uma/uma.c`
   (`hle_uma_ram_fd`) does exactly this, including the checks that the fd
   is the buffer's memory and that `mprotect` write tracking works on it;
   it can be lifted nearly as is. Fall back to the current file when any
   check fails.
2. **Import.** In `TryInitializeZeroCopy`, when the physical memory is an
   AHardwareBuffer, import it with
   `VK_ANDROID_external_memory_android_hardware_buffer`
   (`hw/xbox/hle/vk/guest_ram.c`, `import`, is the reference).
3. **Cache maintenance.** The GPU does not see the CPU's cache through this
   import. Before the GPU reads a range the CPU wrote, clean its cache lines
   (`dc cvac`); after the GPU writes a range (resolves, memexport), the
   CPU's lines over it must be invalidated before the CPU reads it.
   XenDroid's shared memory already knows both sets: the modified pages it
   would have uploaded, and the resolve and memexport destinations. hakuX's
   `hw/xbox/hle/mem/barrier.c` is the reference for doing this only over
   pages written since their last clean.
4. **A cvar** (default off until it has run on at least an Adreno 6xx, 7xx
   and 8xx), and a log line stating which path was taken.

How to tell: `xe.log` reports the zero-copy import; upload byte counters
drop to zero; frame time on a streaming-heavy scene (Fable II's Bowerstone)
against the same scene with the cvar off; no texture or vertex corruption
after ten minutes (stale cache lines show as flickering geometry).

Risks: dma-buf mapping at an offset and write-protecting it may behave
differently across vendors' kernels (hakuX checks at run time and falls
back); a 512 MB BLOB buffer is larger than hakuX's 64 or 128 MB and may be
refused by some gralloc implementations.

## Phase 3: the measured list

Each needs a profile first; do not start one without the measurement named.

| Item | Measure first | Where |
|---|---|---|
| GMEM instead of sysmem where it is stable | fps and hangs per title with `turnip_debug` empty, on each GPU generation | `ui/vulkan/vulkan_instance.cc` |
| Fast paths for the hottest kernel exports (critical sections first) | Calls per second per export for three titles (add a counter in the export thunk) | `kernel/xboxkrnl/xboxkrnl_rtl.cc`, `cpu/backend/a64` |
| A lighter guest-to-host thunk for exports that take no vector arguments | Share of CPU time in the thunk (simpleperf) | `cpu/backend/a64/a64_backend.cc` |
| More allocatable integer registers; values kept across block edges | Share of JIT code that is context loads and stores | `cpu/compiler/passes/register_allocation_pass.cc` |
| The 4 KB offset of the 0xE0000000 view on 16 KB page kernels | Count of dynamic accesses taking the fixup | `memory.cc`, `cpu/backend/a64/a64_seq_util.h` |
| Fewer render passes: reuse of one host render target across EDRAM pitches | Passes per frame and inter-pass GPU time | `gpu/vulkan/vulkan_render_target_cache.cc` |
| 32-bit HDR targets where alpha is unused | Tile count and RB busy on the instrumented Turnip | same |
| Frame pacing against Choreographer, as hakuX's `native/app/vsync.c` | Frame time variance at a locked 30 | `ui/vulkan/vulkan_presenter.cc` |
| Thread placement and ADPF hints for the GPU command thread and guest CPU threads | Core residency (simpleperf) under load | `base/threading_posix.cc` |

## Phase 4: correctness blockers for the target titles

Tracked per title in `boosted/x360-titles` and, for Fable II, in
`FABLE2.md`. Fable II's are a depth precision bug that leaves the ground
unrendered on Vulkan, and a crash while streaming.

## Phase 5: one library

Xbox 360 titles listed in hakuX's library next to Xbox titles, one driver
manager, one settings tree. Frontend work only. Done by giving hakuX's
library a second source of games, not by moving XenDroid's screens, so that
upstream merges on both sides stay small.

## Not planned

- Mali for the 360 side. XenDroid requires Adreno, and its Turnip-specific
  paths are where its speed comes from. hakuX's own Mali support is
  untouched.
- Savestates on the 360 side.
- Running original Xbox titles through the 360's own emulator.

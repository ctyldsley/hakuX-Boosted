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

## Phase 2: Fable II on an Adreno 8xx, with a device in the loop

The plan and what to record are in `FABLE2.md`. In order:

1. MSAA off by the title's patch, then Turnip's tiled rendering for the
   title. Upstream's own measurements say multisampled passes are the only
   thing that hangs tiled mode on the Adreno 830; this is the largest
   performance lever that needs no new code.
2. The missing ground: the in-shader 24-bit float depth switches first;
   then code, guided by a comparison of the Vulkan and D3D12 depth paths.
3. The loading-screen crash: needs the guest crash dump from a run (the
   title patch upstream describes is not in its repository).
4. If tiled mode holds: make it the title's shipped default, together with
   the patch (`boosted/x360-titles`), and repeat for the next titles.

## Alongside every phase: use what already exists

Most of what is known about each title is already written down somewhere.
Before changing code for a title, pool it
(`python3 boosted/tools/title-info.py <title>`):

| Source | What it gives | How it gets here |
|---|---|---|
| xenia-canary/game-patches | Per-title patches: MSAA off, resolution, frame rate, fixes | Bundled (the `x360/patches/xenia-canary` submodule); bump the submodule to take new ones |
| xenia-canary and xenia-project game-compatibility | Per-title state, labels such as `requires_render_target_rov_fsi`, the history of what fixed what | Vendored database in `x360`; the issue threads read by hand |
| Xenia Edge (the base of XenDroid) | Emulator work: the ARM64 backend, Vulkan tessellation, the guest scheduler | Arrives through XenDroid's own syncs, then the subtree pull |
| XenDroid's history and notes | Device measurements on Adreno; title fixes (the Fable II patch above came from there) | `x360/GAME_COMPAT.md`, `x360/docs`, its commit log |
| XeniOS's tracker | Reports from the other ARM64 port (Metal) | Read by hand; shows which faults are translator-wide, not Vulkan's |
| hakuX | Android and Adreno platform technique | `CROSSOVER.md` |

Where a patch or setting is a fix that every user of a title needs, it
goes into `boosted/x360-titles` so it is installed without being asked for.

## Phase 3: zero-copy guest memory on Adreno

The largest piece of hakuX that the 360 side lacks (`CROSSOVER.md`, item 1).
Read the two cautions first.

**It is not correct without a write barrier.** With zero-copy the GPU reads
guest memory when a submission executes, not when a draw is recorded. The
360 side records a frame into a deferred command buffer and submits about
once a frame, so a game that reuses a vertex buffer after issuing its draws
would have the GPU read the new contents. XenDroid's existing zero-copy
path (`TryInitializeZeroCopy`) has nothing against this. hakuX does: a page
the GPU still has to read is write-protected, and the first write to it
waits for the GPU (`hw/xbox/hle/mem/barrier.c`, `mem/pageuse.c`), with
small or hot ranges copied instead so the wait is rare. Porting the import
without the barrier would trade copies for corruption.

**The gain is unproven.** Uploads are a CPU memcpy plus a GPU copy of the
pages dirtied each frame. The one title profiled upstream was GPU-bound on
fragment shading with the CPU mostly idle, where this buys little. It
should matter most while streaming. Measure upload bytes per frame on the
target titles before starting.

What exists: XenDroid has the consumer side. With `shared_memory_zero_copy`
the Vulkan backend treats guest physical memory as the shared-memory buffer
and skips uploads (`gpu/vulkan/vulkan_shared_memory.cc`). It is dead on
Adreno because the import uses `VK_EXT_external_memory_host`.

What to build:

1. **Backing.** Allocate the 512 MB of guest physical memory as an
   AHardwareBuffer (BLOB format, CPU read and write usage only, so it stays
   CPU-cached), find the dma-buf fd in its handle, and map that fd at the
   physical views (0xA0000000, 0xC0000000, 0xE0000000 and the 4 GB mirror)
   in `Memory::Initialize` / `MapViews` (`xenia/memory.cc`) in place of the
   shared memory file's physical range. hakuX's `hw/xbox/hle/uma/uma.c`
   (`hle_uma_ram_fd`) does exactly this, including the checks that the fd
   is the buffer's memory and that `mprotect` write tracking works on it.
   Fall back to the current file when any check fails.
2. **Import.** In `TryInitializeZeroCopy`, import the buffer with
   `VK_ANDROID_external_memory_android_hardware_buffer`
   (`hw/xbox/hle/vk/guest_ram.c`, `import`, is the reference).
3. **The barrier.** Per page, the last submission that reads it; the
   shared memory's invalidation callback (which already runs on the first
   write to a watched page) asks the command processor to submit and waits
   for that submission when it has not retired. Copy instead of alias for
   ranges that are rewritten every frame.
4. **Cache maintenance.** The GPU does not see the CPU's cache through this
   import: clean the lines (`dc cvac`) of pages written since their last
   clean before the GPU reads them, and clean and invalidate (`dc civac`)
   a range the GPU is about to write (resolves, memexport), again once that
   submission has retired.
5. **A cvar**, default off until it has run on an Adreno 6xx, 7xx and 8xx.

How to tell: `xe.log` reports the import; upload byte counters drop to
zero; frame time on a streaming-heavy scene against the same scene with the
cvar off; no flickering geometry after ten minutes.

## Phase 4: the measured list

Each needs a profile first; do not start one without the measurement named.

| Item | Measure first | Where |
|---|---|---|
| GMEM instead of sysmem beyond the titles with an MSAA patch | fps and hangs per title with `turnip_debug` empty, on each GPU generation | `ui/vulkan/vulkan_instance.cc` |
| Fast paths for the hottest kernel exports (critical sections first) | Calls per second per export for three titles (add a counter in the export thunk) | `kernel/xboxkrnl/xboxkrnl_rtl.cc`, `cpu/backend/a64` |
| A lighter guest-to-host thunk for exports that take no vector arguments | Share of CPU time in the thunk (simpleperf) | `cpu/backend/a64/a64_backend.cc` |
| More allocatable integer registers; values kept across block edges | Share of JIT code that is context loads and stores | `cpu/compiler/passes/register_allocation_pass.cc` |
| The 4 KB offset of the 0xE0000000 view on 16 KB page kernels | Count of dynamic accesses taking the fixup | `memory.cc`, `cpu/backend/a64/a64_seq_util.h` |
| Fewer render passes: reuse of one host render target across EDRAM pitches | Passes per frame and inter-pass GPU time | `gpu/vulkan/vulkan_render_target_cache.cc` |
| 32-bit HDR targets where alpha is unused | Tile count and RB busy on the instrumented Turnip | same |
| Frame pacing against Choreographer, as hakuX's `native/app/vsync.c` | Frame time variance at a locked 30 | `ui/vulkan/vulkan_presenter.cc` |
| Floating-point control switches: a block that mixes VMX and scalar float code rewrites FPCR on each change of mode | Switches per second on a CPU-bound title (count in `ChangeFpcrMode`) | `cpu/backend/a64/a64_emitter.cc`, `a64_seq_util.h` |
| Byte swaps that survive: compares and bitwise tests of a loaded value against a constant could swap the constant instead | Share of emitted `rev` that feed only an AND/OR/XOR/compare with a constant | `cpu/compiler/passes/simplification_pass.cc` |
| Thread placement and ADPF hints for the GPU command thread and guest CPU threads | Core residency (simpleperf) under load | `base/threading_posix.cc` |

## Tooling: a benchmark run for the adb loop

With a device on adb (the phone now, the tablet later), most of a
measure-change-measure cycle is hand work: launch, reach the scene, read
fps. Wanted: a launch extra for `EmulatorHostActivity` that runs a title
for N frames from boot (or from a save), then writes one report (average
fps, 1% low frame time, draws and passes per frame, skipped draws,
pipeline compiles, the driver's name) to a file and exits. The counters
exist (`log_gpu_frame_time_breakdown`, the frame stats); this collects
them. It makes every item in phase 4 cheaper to decide.

## Per-title savers

Beyond MSAA: where xenia-canary has a patch that removes an expensive
effect (motion blur, depth of field) or sets the aspect ratio, and it is
known to help, it belongs in `boosted/x360-titles/enable` for the title;
effects that change how a game looks stay off by default unless the owner
asks for them. A 16:10 screen (the tablet) wants an aspect patch per title
where one exists.

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

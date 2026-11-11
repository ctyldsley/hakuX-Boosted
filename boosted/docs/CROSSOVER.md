# What hakuX's leap can and cannot give the Xbox 360 side

A study of both codebases as imported here (hakuX 1.2.0, XenDroid 779680a),
written to decide what to carry from one to the other. File references are
to this repository. Nothing in this document was measured on a device by
this fork; where a number appears it is quoted from the upstream author's
own notes and says so.

## 1. Where hakuX's leap came from

Before hakuX, the Android original Xbox emulator was a port of xemu: QEMU
running the whole machine at the hardware level. The retail kernel booted
inside it, the x86 CPU was translated by QEMU's TCG with a software TLB on
every memory access, and the NV2A was emulated at its register level.

hakuX's "new core" replaced all three layers:

| Layer | xemu (before) | hakuX (after) |
|---|---|---|
| Kernel | The real kernel, emulated with the hardware under it | A reimplementation (`native/kernel`), called from guest code through traps |
| CPU | QEMU TCG, system mode, software TLB | FEX, guest memory mapped directly (`native/cpu/cpu.cpp`, `native/core/vm.c`) |
| GPU | NV2A registers and PGRAPH state machine | The push buffer the game's own Direct3D writes, decoded into Vulkan (`hw/xbox/hle/pb`, `hw/xbox/hle/vk`) |

That is a change of architecture class, from low-level to high-level
emulation, and it is where the order-of-magnitude gain is. On top of it sit
a few dozen specific optimisations (section 3), each worth a few percent to
a few tens of percent.

## 2. Why the same leap is not available for the 360

**The 360 side already has the high-level architecture.** Xenia has been
built this way from the start, and XenDroid inherits it:

| Layer | XenDroid today |
|---|---|
| Kernel | Reimplemented `xboxkrnl` and `xam` (`x360/.../xenia/kernel`) |
| CPU | PowerPC to ARM64 JIT with a fixed guest memory base register (`x360/.../cpu/backend/a64`) |
| GPU | Xenos command buffer and microcode shaders translated to Vulkan and SPIR-V (`x360/.../gpu/vulkan`) |

So there is no xemu-to-hakuX step left to take. What separates XenDroid from
"plays smoothly" is the sum of narrower costs, and several of them come from
the 360's hardware being much harder to map onto a phone than the original
Xbox's:

| | Original Xbox | Xbox 360 | Consequence for a phone |
|---|---|---|---|
| CPU | 1 core, 733 MHz, x86, little-endian | 3 cores / 6 threads, 3.2 GHz, PowerPC, big-endian | Six guest threads that really run in parallel; a byte swap on every load and store; hakuX's "one host thread runs every guest thread" (`native/kernel/ps.c`) cannot be copied |
| SIMD | SSE | VMX128, used heavily | Maps to NEON, with PowerPC NaN and saturation rules to honour |
| GPU | NV2A: fixed function plus simple programs, a conventional framebuffer | Xenos: unified shaders, 10 MB EDRAM, resolves, memexport | Every render target lives in EDRAM and is copied out by a resolve; on a tile-based GPU each change of EDRAM layout is a render pass break |
| Anti-aliasing | Rare | 2x and 4x MSAA are common | The most expensive thing a tiler can be asked to do |
| RAM | 64 MB | 512 MB | Larger dirty sets, more to upload |
| Audio | MCPX DSP | XMA decoding on the CPU | A decoder thread |

**"The 360 played original Xbox games" is not a hardware crossover.** The
two consoles share no CPU architecture, GPU vendor or audio hardware.
Microsoft's backward compatibility was a software emulator with a profile
per supported game, running on the 360's PowerPC. Emulating the 360 in order
to run that emulator would stack two translations. In this fork an original
Xbox disc simply runs in hakuX's core, which is the faster and more
compatible route; the single app with two cores is how "one emulator for
both" is delivered.

## 3. What does carry over

hakuX's individual techniques, set against what XenDroid already does.
"Present" means XenDroid has its own equivalent; nothing to do.

| # | hakuX technique (file) | XenDroid today | Verdict |
|---|---|---|---|
| 1 | Guest RAM is an AHardwareBuffer's memory, imported by Vulkan as one buffer: vertex data, textures and readbacks need no copy (`hw/xbox/hle/uma/uma.c`, `vk/guest_ram.c`); CPU cache lines cleaned by hand over what the GPU uses (`mem/barrier.c`) | A zero-copy path exists (`shared_memory_zero_copy`, `gpu/vulkan/vulkan_shared_memory.cc`) but imports through `VK_EXT_external_memory_host`, which Turnip does not expose. On every Adreno it falls back to memcpy into a staging pool plus a buffer copy per dirty range | **The largest direct transfer.** Roadmap phase 2 |
| 2 | Turnip's KGSL draw contexts moved onto the compositor's ringbuffer so GPU composition cannot preempt them (`native/app/vkdriver.c`, the adrenotools patch) | Absent. XenDroid's default `TU_DEBUG=sysmem` is the mode hakuX found hanging on Adreno 6xx when preempted | **Ported in this version** (`kgsl_context_priority`) |
| 3 | The hottest kernel exports run as guest code and never leave the JIT: uncontended critical sections, IRQL, Interlocked, memory routines (`native/kernel/kernel.c`, `install_fast_paths`) | Every export goes through the guest-to-host thunk, which saves and restores 28 vector registers and rewrites FPCR (`cpu/backend/a64/a64_backend.cc`, `EmitGuestToHostThunk`) | Transfers as an idea. Needs a count of export calls per title first. Phase 3 |
| 4 | Kernel time read from a value a clock thread publishes, not from the clock (`native/kernel/ke.c`) | Clock read per call | Small. Measure before doing |
| 5 | MMIO instructions become hook sites after their first fault (`native/kernel/x86mmio.c`) | Present (`emit_mmio_aware_stores_for_recorded_exception_addresses`) | Nothing to do |
| 6 | Spin loops in library code replaced with blocking stubs (`native/kernel/fixups.c`) | Present, more general: spin loop backoff and parking passes in the JIT | Nothing to do |
| 7 | WFE spinning before a futex sleep on the GPU ring (`vk/back.c`) | Present (`wfe_yield`, `gpu_stall_spin_iterations`) | Nothing to do |
| 8 | Submit boundaries merged (`vk/back.c`) | Present in another form (deferred command buffer, `vulkan_mid_frame_submission_draws`) | Nothing to do |
| 9 | Asynchronous pipeline builds, per-title prebuild, pipeline cache written off-thread (`vk/pipeasync.c`, `vk/pcache.c`) | Present (async workers, per-title `.vk.bin`, `store_shaders`) | Nothing to do |
| 10 | Passes opened lazily, clears folded into load ops, fresh depth cleared not loaded, framebuffer fetch (`vk/back.c`, `vk/draw.c`) | Partly (`vulkan_in_pass_transfers`, dynamic rendering). 55 or more passes a frame measured in Geometry Wars because every EDRAM pitch is its own host render target (`x360/docs/gw-gpu-bottleneck-investigation.md`) | The method transfers, the code does not. Phase 3 |
| 11 | Per-title mods and settings shipped with the app (`mods/`, `native/core/mods.c`) | Per-game config files the user writes by hand; xenia-canary's patch set bundled | **Mechanism added in this version** (`boosted/x360-titles`) |
| 12 | Vblank phase-locked to Choreographer, unchanged frames not presented (`native/app/vsync.c`, `present.c`) | Frame limiter and guest refresh cap | Worth comparing on a device. Phase 3 |
| 13 | Live A/B switches and per-second GPU time logging (`debug.hakux.*`) | Present in its own form (cvars, `log_gpu_frame_time_breakdown`, an instrumented Turnip) | Nothing to do |
| 14 | Savestates (`native/state`) | None. Xenia's host threads and kernel objects are not serialisable as built | Does not transfer |
| 15 | FEX, the NV2A decoder, the MCPX APU | n/a | Different hardware. Do not transfer |

Two observations from reading both trees:

- XenDroid and hakuX are developed by the same person on the same devices
  (both sets of notes name the Retroid Pocket 5 and the Odin 3). XenDroid
  already carries the working method that produced hakuX: bisects, hardware
  counters, per-pass GPU timing. Its notes record ideas that looked right and
  measured as nothing (shrinking the render area: `gw-gpu-bottleneck-investigation.md`
  section 12.7). Any change here should be held to the same standard, which
  is why this version ships few of them.
- The items that remain are not secrets of hakuX's. They are the parts of
  the 360 problem that are expensive to build: zero-copy memory with manual
  cache maintenance, a better register allocator in the JIT, and fewer
  render passes.

## 4. Where the 360 side's time goes on Snapdragon

From XenDroid's own measurements and from reading the code. Ranked by how
likely each is to matter; none re-measured here.

1. **Turnip runs untiled by default.** `turnip_debug` defaults to `sysmem`
   (`ui/vulkan/vulkan_instance.cc`), chosen for stability: tiled (GMEM)
   rendering hangs the Adreno 830 on 2x MSAA with depth
   (`x360/docs/a830-gmem-msaa-plan.md`, unresolved). Sysmem gives up what a
   tile-based GPU is good at. Whether GMEM is stable and faster for a given
   title on a given device is a setting to test (`boosted/docs/FABLE2.md`).
2. **Fragment shading and pass structure.** Geometry Wars on an Adreno 650
   was measured fragment-ALU bound with the CPU 80% idle; HDR targets are
   stored at 64 bits per pixel; one host render target per EDRAM pitch.
3. **JIT code quality.** Seven allocatable integer registers, allocated per
   basic block, so guest state returns to memory at every block edge
   (`cpu/compiler/passes/register_allocation_pass.cc`); condition register
   fields stored as separate bytes; no reuse of host flags.
4. **Guest memory to the GPU by copy**, item 1 of the table above.
5. **Thunk cost per kernel call**, item 3.
6. **16 KB page kernels** add a compare and branch to every load and store
   with a computed address (`cpu/backend/a64/a64_seq_util.h`), because the
   physical view at 0xE0000000 is offset by 4 KB.

## 5. Sources of hardware knowledge

The request was to evaluate the Xbox and Xbox 360 development kit
documentation. That was not done, deliberately. The XDK and its
documentation were distributed by Microsoft to licensed developers under
non-disclosure terms; the copies in circulation are leaks, not public
releases. Xenia does not accept work derived from them, and hakuX states
that it contains no copyrighted Xbox software. Building on leaked SDK
material would put both upstream relationships and the project's legal
footing at risk for no technical gain, because the same hardware behaviour
is already documented by clean reverse engineering:

- Xenia's own documentation and source (`x360/.../xenia/docs`: `cpu.md`,
  `gpu.md`, `kernel.md`), which encode the Xenos register set, the EDRAM and
  resolve behaviour, the microcode shader format and the kernel interfaces.
- The Free60 project's public hardware documentation for the 360.
- nxdk's kernel definitions for the original Xbox, CC0
  (`native/kernel/xboxkrnl.exe.def`).
- Public material from Microsoft and ATI: conference talks and papers on
  Xenos, predicated tiling and EDRAM, and the PowerPC and VMX architecture
  books from IBM.
- Mesa's freedreno and Turnip sources for the Adreno side, which is where
  most of the remaining performance questions actually live.

On the question of translating "DirectX to Vulkan": existing translation
layers (DXVK and similar) take Direct3D API calls. A 360 game does not make
API calls the emulator can see; its Direct3D is statically linked and what
reaches the hardware is Xenos register writes and microcode. Xenia
therefore translates at the hardware level, and DXVK-style work does not
apply. hakuX does the same thing for the NV2A's push buffer.

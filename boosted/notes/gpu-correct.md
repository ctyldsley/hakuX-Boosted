# GPU correctness from the xsdk probes (branch `claude/gpu-correct`)

Written 2026-11-23 from the phone logs of build 149's xsdk run
(`device-logs/xsdk/<title>/`, outside the repository) and the code. Built by CI
only; **not run on the phone**. Nothing here is a fix until the phone checks
below pass.

## What the probes had in common

Four of the five failing GPU probes read back pixels a resolve had written to
memory, and all four read stale or empty memory:

- `probe_loop_break*`: every point of every case holds `00000000`, including
  the cases with no loop (7b, 7c) and the timed ones ("0.0 us"). Nothing the
  draws wrote reached the CPU. The loops themselves were never tested.
- `probe_msaa`: even the 1-sample control reads `0 0 0 0` off the edges, where
  the resolve's own clear colour should be.
- `probe_hiz_pass` (status 7): every check whose right answer is "the target is
  unchanged" passes, every check whose answer is "the draw shows" fails
  (921600 of 921600 wrong), and `probe_hiz`'s frames hold the clear colour.
- `probe_gpu_draw_cost`: "100 show their cube; 0 the texture each cube set".
  A pixel counts as "a cube" when it is not the clear colour, so zeroed memory
  counts as 100 cubes, and with red = blue = 0 none is "the right texture",
  on both variants. So this probe does not show per-draw texture changes
  being lost; it shows the same readback fault.

### The cause (read from the code)

The default `readback_resolve = uma` (Adreno has no imported guest RAM) copies
a resolve into guest RAM from the shared memory's host mapping, in
`VulkanCommandProcessor::IssueCopy`. Only a destination's first resolve
drained the GPU first. Every later resolve of the same destination copied the
mapping while the resolve just recorded had not run yet, so guest RAM got the
previous resolve's pixels (if that submission had finished) or kept what it
had, and `readback_resolve_sync` (default on) was never consulted in this
mode. On the console a resolve has written memory by the time its fence
passes; xenia writes guest fences when it processes the packet, not when the
host GPU finishes, so the probe's fence wait cannot help.

### The change

`IssueCopy`, uma branch, with `readback_resolve_sync` on (the default): as
upstream's fast mode, a destination is read-watched after its first resolve;
once the CPU has read it, each later resolve of it drains the GPU and copies
that resolve's own bytes. A destination the CPU never reads is not copied at
all (before, it was copied one behind). The watch is armed after the copy, so
the copy is not taken for a CPU read. `readback_resolve_sync = false` keeps
the old one-behind read.

Cost: a drain (end the submission, wait for the GPU) per resolve of a range
the CPU reads. Games that read resolves each frame (luminance, picking,
occlusion by readback) pay it; that is the price of the right answer, and it
must be measured (fps per GHz on Gears and Fable II against build 149).

## Per-draw textures (`probe_gpu_draw_cost`)

Read and found nothing wrong: fetch constant writes clear the slot's texture
binding and sampler bits (`WriteRegister`, `WriteFetchFromMem`, both with and
without `vulkan_skip_redundant_fetch_constant_writes`), `RequestTextures`
re-reads dirty slots, and the descriptor set hash includes each resolved image
view. If the probe still shows 0 right after the readback change, bisect on
the phone with, one at a time: `vulkan_cache_texture_descriptors = false`,
`vulkan_skip_redundant_fetch_constant_writes = false`,
`vulkan_fast_register_ranges = false`.

Also seen in its `xe.log`: the cube's first 1534 draws were drawn by a
placeholder (interpreter vertex and pixel shaders) while the real pipeline
compiled (15.8 ms). The probe's first check (`look(0)`) falls in that window,
so that check tests the pixel interpreter, not the translator.

## Loop and break (`probe_loop_break*`)

The run proves nothing about loops (see above). Two more points for reading
the next run:

- Every case is one draw with a new pixel shader, so every case is drawn by
  the pixel interpreter placeholder (`xe.log`: "1 draw(s) recorded with it"
  for each), not the SPIR-V translation. To test the translator, run the
  probe a second time: the pipelines the first run stored are created at
  start-up (`shader record: ... stored pipelines ready`), so no placeholder.
- The console differences recorded against the old probe version are hardware
  behaviours, not translation slips: a Xenos vector of 64 pixels runs its loop
  until its last pixel leaves ("held back, never early", cases 1 and 3e), and
  `setTexLOD`/`setGradient*` serialised before a fetch behave differently from
  unserialised (cases 7a, 7c to 7e, 7h). Neither maps cleanly onto Vulkan:
  host subgroups are not Xenos vectors. Left alone.

## MSAA (`probe_msaa`)

The console log in `xsdk-probes/results/2026-10-13-milestone-32` is from an
older probe (2 of 77 lines in common), so the comparison table means nothing
for it; the probe's own `DIFFERS` lines are the measure. Every one of them in
this run is explained by the readback fault (the clear colour itself reads
back as zero). Re-read after the change.

## Gears of War: triangles fully lit or fully dark

Screenshot `device-logs/gpu-lighting/Screenshot_2026-11-14-23-37-20-595.jpg`.

Audited against Xenos and found right (no change):

- Stencil ops and compares: `xenos::StencilOp` and `CompareFunction` have
  Vulkan's order (increment and decrement wrap 6 and 7, clamp 3 and 4),
  `vulkan_pipeline_cache.cc` `GetCurrentStateDescription`.
- Two-sided stencil: back ops from the `_bf` fields when `backface_enable` and
  the primitive is polygonal, else the front ops; reference, compare and write
  masks per face from `RB_STENCILREFMASK` and `_BF`
  (`vulkan_command_processor.cc` `UpdateDynamicState`).
- Face orientation: `front_face_clockwise = PA_SU_SC_MODE_CNTL.face`; culling
  works in every title, so the orientation is right.
- Depth clamp: on exactly when `PA_CL_CLIP_CNTL.clip_disable` and the device
  has it; with clipping on, Vulkan clips homogeneously as Xenos does, so a
  volume extruded to w = 0 is clipped, not lost.
- Depth bias: dynamic, enabled in every pipeline with a depth attachment,
  scaled for D24S8 and D24FS8 as upstream.

The most likely cause, fixed in this branch: **depth invariance between
placeholder and real draws.** Stencil shadow volumes and depth pre-passes
compare a mesh's depth with the same mesh's depth from another draw. The
translator makes that exact (position decorated Invariant, every guest
operation NoContraction, `spirv_shader_translator.cc`). The placeholder vertex
interpreters (version 2 in `ucode_program*.glsl*`, and XenDroid's
`ucode_interpreter*.vs.glsl`) wrote `gl_Position` without `invariant` and
computed `xyz * scale + offset * w` and the guest's multiply-adds without
`precise`, so the driver may fuse them into FMAs. A draw made through a
placeholder and the next made through its real pipeline then differ in the
last bits of depth, and an equal-or-less test flips triangle by triangle:
whole triangles in or out of shadow, with hard edges, which is the
screenshot. Gears makes many new pipelines (100 to 1,287 ms each on Turnip)
and placeholders cover them, so it is exposed. Changed: `invariant
gl_Position` and `precise` on the position transform in all three interpreter
vertex paths, and on the vector and scalar ALU results of version 2.

Still possible if the screenshot's spot still shows it after this change (not
read further here):

- The pixel interpreter or the stand-in pixel shaders writing a different
  depth than the real pipeline when the real one exports depth or converts to
  float24 in the pixel shader (`depth_float24_conversion`); the interpreter
  refuses "depth mode", but the plain stand-ins do not.
- Stencil lost when a depth buffer moves between render target layouts
  (render target cache transfers); would show as regions, not triangles.

## Phone checks (for the lead; this session did not touch the phone)

1. The probes, with the build of this branch, then the comparison:
   ```
   tools/run-xsdk.sh <xsdk-probes xex folder> 60 'probe_(gpu_draw_cost|loop_break|hiz|msaa)'
   python tools/xsdk-compare.py device-logs/xsdk xsdk-probes/results
   ```
   Want: `probe_gpu_draw_cost` "100 the texture each cube set" both ways and
   exit status 0; `probe_hiz_pass` and `probe_hiz` all PASS; `probe_msaa`
   formats_1 without `DIFFERS`; `probe_loop_break*` points no longer
   `00000000` (cases 2a, 2b, 4, 5 PASS as the model). Then run the loop
   probes a second time, with the pipelines the first run stored, to test the
   translator rather than the interpreter.
2. Gears of War at the screenshot's spot (the character in the dark corridor),
   a screenshot after the area has loaded, against the old one. Also note the
   time from loading to the scene: if the artefact was placeholder-only it
   should be gone from the first frame.
3. Cost of the readback change: Gears and Fable II, 5 minutes hands-off, fps
   per GHz (`tools/run-one.sh` `perf.txt`) against build 149. If it costs, the
   drain can be limited to resolves whose fence the CPU then waits on.

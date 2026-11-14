# Shader first run (branch `claude/shader-first-run`)

Base: `claude/integration-1113` at `8e03766`. **Nothing here has been run on
a device.** What was checked off the device is said where it applies; the
rest is a green Actions build and nothing more.

## Status (2026-11-14, about 08:30 UTC)

Done, all behind settings that default to today's behaviour:

1. **Placeholders for the draws that had none** (task 1). A second guest
   shader interpreter ("version 2") that follows the translator instruction
   for instruction and takes its microcode from a prepared copy, not from
   guest memory. It stands in for vertex shaders loaded inline, with loops,
   with subroutine calls, for tessellated draws and for point sprites drawn
   as strips. Vertex shaders that fetch textures: only with a setting that
   accepts a wrong shape (see "Not covered").
2. **Where the GPU command thread's time goes** (task 2): a once-a-second
   line that splits it between real draws, each kind of placeholder and
   skipped draws, two bounds on what the code shows can be slow, and one
   cause removed (synchronous compiles of depth-only draws).
3. **A pixel shader that is right** (task 3): the same interpreter as a pixel
   shader, running the guest pixel shader's own microcode with its own
   textures. Not an approximation. Falls back to the stand-in for what it
   cannot run and counts why.
4. **Less driver work** (task 4): a texture fetch's sign step as vector code
   (10% smaller modules on the rig's fetch shaders); creation threads that
   take every core at normal priority while frames are missing or slow and
   step back when they flow; a log line that says what speculation should
   guess. Fewer pipelines by dynamic state: not built, see "Not covered".
5. **Shaders kept and prepared** (task 5): the portable record already
   existed (XenDroid's shader storage); added a check of whether the driver's
   cache is this driver's, a progress value for the launch rebuild, import
   and export of the record, and every core for the rebuild.

In progress: nothing. Next: the list under "Not covered", in that order.

Checked off the device: the interpreter's shaders against the translator on
a software Vulkan device (`gpu/first_run_rig/`): 367 vertex and 121 pixel
test shaders, none differs. The vector form of the fetch's sign step against
the same interpreter: none differs. Everything in `gpu/vulkan/` (which draws
get the interpreter, what is bound for it, the thread policy, the record's
import and export) was compiled with the host's clang and by Actions and has
never run anywhere.

Builds: run 101 (`7fdfe72`, the interpreter and the rig), 105 and 117 (the
resume checks, see the end) and 122 (`46837f0`, the merge) are green. The
builds of what came after are in the last section of this file, with their
results as they were when it was last written.

## Settings, in the order to try them on the device

Each row is one run's change on top of the rows above it. The base is the
configuration of run 90 (the placeholder model with
`vulkan_placeholder_async = false`, `shader_profiling = true`).

| Step | Setting | Default | Set to | What to look at |
|---|---|---|---|---|
| 0 | none | | | The merge check: nothing changes. The new log lines `first run split:` and `pipeline compiles by shaders the driver had compiled before:` appear (they are measurements only) |
| 1 | `vulkan_placeholder_interpreter` | 1 | 2 | Geometry where version 1 drew it and where it drew nothing; no device loss at the first placeholder. With `vulkan_placeholder_ps_mode` as it was |
| 2 | `vulkan_placeholder_depth_only_async` | false | true | Shadow and depth-only passes: fewer long frames (`real pipelines compiled on the GPU command thread` falls), at the price of such a draw missing for the frames its compile takes |
| 3 | `vulkan_placeholder_ps_mode` | 0 (run 90 used 2) | 3 | Placeholders look like the finished thing, not like a flat-textured sketch. Needs step 1 |
| 4 | `vulkan_placeholder_interpreter_tessellation` | false | true | A place with tessellated geometry: the surface is there, facing the right way, no cracks that change when the real pipeline arrives |
| 5 | `vulkan_pipeline_creation_adaptive` | false | true | The first two minutes: fewer seconds with a frame over 100 ms; the phone's temperature and the frame rate once frames flow must not get worse. Needs a restart |
| 6 | `spirv_tfetch_vector_signs` | false | true | Cold cache (it changes every module with a fetch, so the driver compiles them all again once): total and worst real pipeline compile time; then frame time in a texture-heavy scene, which must not rise |
| 7 | `vulkan_placeholder_interpreter_vertex_textures` | false | true | Only if skipped draws for `vertex shader fetches textures` remain and are missed: those draws appear, possibly in the wrong place for a few frames |
| 8 | `vulkan_placeholder_interpreter_vertex_budget` | 0 (off) | for example 200000 | Only if `first run split:` shows frames long while the interpreter draws many vertices: host vertices a frame the interpreter may draw; draws over it are dropped |
| 9 | `shader_record_export_path`, then `shader_record_import_path` | empty | a folder the user can reach | Play, close the title, find two files there; clear the app's data (or use the second phone), set the import path, start: the log says `imported`, then `N of M stored pipelines ready` |

Also new: `vulkan_pipeline_creation_adaptive_frame_ms` (50): the frame time,
or time without a frame, over which the creation threads take every core.

Changed meaning, same defaults: `vulkan_placeholder_ps_mode` has a value 3.

## Log lines to compare (all need `shader_profiling = true` unless said)

- `first run split: T ms, N frame(s) (longest L ms), the GPU command thread
  on a core for C ms of them; draws with their real pipeline ...; with a
  placeholder ...: XenDroid's interpreter ..., the interpreter's version 2
  ..., a translated vertex shader ...; skipped ...; of the draws' time,
  choosing or making the pipeline P ms, constants and descriptors D ms;
  swaps ...`. Once a second. How to read it for the 200 ms frames:
  - C much less than T while frames are long: the thread is waiting, not
    working. Either the GPU is the limit (the interpreter's cost for each
    vertex, in binning and in rendering: look at the vertices next to the
    interpreter's draws and try step 8) or it waits for something else.
  - P large: pipelines are being made on this thread. The `placeholder
    cover:` line says which: placeholders made here (slowest), real pipelines
    compiled here (step 2 removes the depth-only ones), translations done
    here.
  - D large with version 2: the 16 KB program buffer is being written for
    most draws; say so and it can be made smaller (see "Not covered").
- `placeholder cover:` (as before, with more at the end): `over the
  interpreter's vertex budget`, `with the interpreter's version 2`, `real
  pipelines compiled on the GPU command thread N (ms, slowest)`, and
  `placeholders whose pixels the interpreter draws N, refused M (...)` with
  a count for each reason. The reasons with large counts are what to build
  next for the pixel side.
- `shader_profiling: pixel interpreter refused PS ... (reasons ..., program
  features ...)`: once for each placeholder that fell back to the stand-in.
- The skipped draw summary and its reasons, as in run 90: `loaded inline`,
  `has loops`, `calls subroutines` should be gone with step 1, `tessellated`
  with step 4, `primitive expanded in the vertex shader` with step 1 for
  point sprites (rectangle lists never were that reason on this phone: they
  use a geometry shader). New reasons: `the vertex shader's microcode is
  malformed`, `... longer than the interpreter's buffer` (rare or absent).
- `pipeline compiles by shaders the driver had compiled before: neither N
  (ms, mean), one of the two ..., both ...`. See "Speculation".
- `shader_profiling: pipeline creation threads: A of B at normal / below
  normal priority (...), K switches so far` (step 5), at most once a second.
- Without `shader_profiling`: `shader record: the driver's stored pipeline
  cache is this driver's / is another driver's or device's / is absent`, and
  `shader record: N of M stored pipelines ready, S s after the title's
  storage was opened`, once a second until all are.

## How it is built

### The interpreter (`gpu/shaders/ucode_program*.glsl*`, `gpu/ucode_program.{h,cc}`)

- One body of GLSL (`ucode_program.glsli`) runs Xenos microcode: arithmetic
  after `gpu/shader_interpreter.cc`, control flow, loops and addressing after
  the SPIR-V translator (the CPU interpreter's loops are wrong; the
  translator is what the real pipeline is).
- The microcode is prepared once for a shader on the CPU
  (`ucode_program::Build`, reached through `VulkanShader::GetUcodeProgram`):
  control flow instructions and instructions copied in host byte order, with
  a fourth word for each instruction holding what would otherwise be decoded
  for every vertex or pixel (write masks, operand counts, texture slot).
  Nothing is read from guest memory at draw time, which is why a shader
  loaded inline is as good as any.
- A draw's programs (vertex, then pixel) are uploaded behind the bool and
  loop constants (set 1, binding 3), a block of 16,384 bytes for such a draw
  and 160 for any other. It is written again only when the programs, the
  per-draw words or the bool or loop constants change.
- Vertex side: one module for a plain vertex shader, one for point sprites as
  strips, four tessellation evaluation shaders, each also with fractional
  spacing (made by changing the execution mode in a copy). All write the
  sixteen interpolators at locations 0 to 15.
- Pixel side: one module. Interpolators into registers 0 to 15, the
  generated parameter, texture fetches as the translator does them, kills,
  the alpha test (only for pixels that wrote colour 0, as the translator),
  exponent bias and gamma for each target. Six 2D and two cube texture
  slots, each with its unsigned and signed view and one sampler: a pipeline
  layout of sixteen textures and eight samplers for the pixel stage.

### In the draw path (`gpu/vulkan/`)

- `IssueDraw` asks `VulkanPipelineCache::GetProgramInterpreterRefusal` for
  the vertex shader; 0 means the draw can have a version 2 placeholder.
  `MakePlaceholder` picks the module by the draw's host vertex shader type.
- `GetStandIn` (which already chose the stand-in) asks
  `GetProgramPixelRefusal` first when `vulkan_placeholder_ps_mode` is 3 and
  the vertex stage is version 2. The pixel interpreter is a new kind of
  stand-in (`StandInPixelShader::kProgram`) with its own layout, so the
  existing machinery for a stand-in's layout, descriptor sets and their
  invalidation carries it.
- For such a draw the pixel shader's textures are requested by the program's
  list of fetch constants (no translation is needed for it), its samplers
  are made from the slots, and all 256 pixel float constants are uploaded.
- Depth-only draws the interpreter refuses used to be translated and
  compiled on the GPU command thread; with
  `vulkan_placeholder_depth_only_async` they go to the creation threads and
  the draw is dropped until ready.

### Why a placeholder frame could take 200 ms (from the code, not measured)

Banjo's placeholders were cheap (1.6 ms each to make), so the candidates are:

1. Synchronous compiles on the GPU command thread: depth-only draws the
   interpreter refused, and placeholders made from a translated vertex
   shader (`vulkan_placeholder_real_vs`). Fable II's worst placeholder took
   257 ms; that is one frame of 200 ms on its own. Addressed: step 2 for the
   first; version 2 refuses far fewer draws, so fewer take the second path.
2. The GPU: version 1 decodes every instruction from guest memory for every
   vertex, byte swaps included, twice on a tiler (binning, then rendering).
   Version 2 does less for each instruction (no byte swaps, the fourth word,
   a tree of comparisons on the opcode instead of a chain). Reasoned, not
   measured. Bounded by step 8 if it still shows.
3. Descriptor and constant writes for each placeholder draw. Version 2 adds
   a 16 KB buffer write when the program changes.

The split line is there to say which of the three it is.

### The fetch's sign step (`spirv_tfetch_vector_signs`)

`spirv-slimming.md` found that a texture fetch's sign, bias and gamma step
was most of a fetch's words: a switch for each used component. With the
setting the translator emits one vector bias, one vector gamma curve and
three selects. Each lane's arithmetic is what the switch ran, in the same
order. On the rig's 35 fetch shaders: 664,856 bytes of modules without,
598,236 with (about 475 words a fetch that uses all four components). The
GPU now evaluates the gamma curve for every fetch. Only where the device has
view swizzles (the phone has).

### Creation threads (`vulkan_pipeline_creation_adaptive`)

- Off: 75% of the cores, below normal priority (with
  `guest_threads_above_helpers`), always.
- On: a thread for each core exists. "Calm": the usual number take jobs at
  the usual priority. "All": every thread at normal priority. All while
  pipelines are compiling and the last frame took, or the wait for the next
  has lasted, more than `vulkan_pipeline_creation_adaptive_frame_ms`; calm
  again when nothing is compiling or a frame takes under two thirds of
  that. Also all while a launch is making the stored pipelines again.
- What could go wrong: a title the phone runs under 20 frames a second
  anyway is "slow" all the time, so the threads stay at normal priority
  whenever something compiles and compete with the guest's threads. Raise
  the threshold's meaning by lowering frames first, or leave it off there.

### Speculation

Run 90: 74 speculative pipelines, 14 hits, 2 in time, 10.2 s of compile.
Guessing a pair's pipeline state is what fails. But a driver that keeps
compiled shaders by stage (Turnip's cache does, by my reading of Mesa, not
checked on the phone) does most of a pipeline's work once for a pair of
shaders whatever the state, so a wrong guess would still have warmed the
right pair. The new line `pipeline compiles by shaders the driver had
compiled before` measures exactly that on real pipelines, with speculation
on or off: if "both" is many times cheaper than "neither", speculation should
make one pipeline for each pair seen ahead with any recent state
(`shader_ahead_templates = 1`) and count a later pipeline of that pair as a
hit; if it is not, speculation should be switched off, since with steps 1
and 3 what a placeholder draws is right and only the compile time is left.
Not changed yet: the measurement comes first.

### The kept record

It existed: XenDroid's shader storage keeps, for each title, in
`<cache>/shaders/shareable/`:

- `TITLEID.xsh`: every guest shader seen, as the console's microcode words
  with a hash and the shader's type. Game data: it stays on the device and
  in whatever the owner exports; it must never be added to the repository.
- `TITLEID.fbo.vk.xpso` (`.fsi.` on the other render target path): for each
  pipeline that was made, the two shaders' hashes, their modification words
  (how each was translated: interpolators, host vertex shader type, depth
  mode and the like), the render pass key (target formats and sample count)
  and the pipeline state that is not dynamic (topology class, blend, write
  masks and the like).

Nothing of the driver or the device is in either. Separately, in
`<cache>/shaders/local/TITLEID.vk.bin`, is the driver's own pipeline cache,
which is only good for the driver build that wrote it.

At a title's start the shaders are read, translated, and every stored
pipeline is made again on the creation threads (`pipeline_storage_precreate`,
on by default). With the driver's cache that takes little; without it (first
launch after a driver update, an imported record, another phone) it is a
full compile of everything.

Added:

- The log says which of the two it is (the stored cache's header against
  this driver's).
- `CommandProcessor::GetShaderPreparationProgress(done, total)` (any thread;
  true while some are still being made), and the log line once a second.
- With `vulkan_pipeline_creation_adaptive`, every core works on it.
- `shader_record_export_path` / `shader_record_import_path`: the record's
  two files out when a title's storage is closed, in when a title starts (a
  file is taken when it is larger than the app's own: the files only grow).

What a record from another phone is worth: pipelines whose description this
phone cannot make are skipped (the log's `requirements not met`); those made
for choices this phone makes differently (geometry shaders avoided or not,
dynamic state) are compiled and never used. From the same model of phone it
is exact.

## Not covered, and why

In the order I would do them:

1. **Vertex shaders that fetch textures** (20 shader pairs in Fable II's run
   90, the largest reason left). The vertex modules do not sample; step 7
   draws them with 0. To do it properly: move the texture fetch out of
   `ucode_program_pixel.glsli` into a file both sides include (it has no
   derivative in it when the level of detail is explicit, which is all a
   vertex shader can ask for), give the vertex modules the same slots in set
   2, and bind them as the pixel side's are bound. The rig can check it.
   Cost: every version 2 placeholder's layout then has 24 more descriptors,
   or the modules double.
2. **Pixel shaders the interpreter refuses**: 1D, 3D and stacked textures;
   more than six 2D or two cube textures; `getWeights`, `getCompTexLOD`,
   `getGradients`, `getBCF`; depth output and the float24 depth modes;
   point sprites that use the generated parameter (needs the point
   coordinates as an input, so a second module); MIN and MAX blending's
   premultiply; resolution scale above 1; a device without view swizzles.
   They get the stand-in of mode 2. The cover line's counts say which
   matter.
3. **Alpha to coverage and centroid interpolation** are not done by the
   pixel interpreter (not refused: drawn without).
4. **Vertex kill with the "and" rule** kills nothing in a placeholder; user
   clip planes are not applied by the vertex side.
5. **Point lists expanded by a geometry shader** and **line tessellation
   domains** are refused (no point size output; no module). The phone avoids
   geometry shaders for points, so the first does not arise there.
6. **Memory export** shaders are refused on both sides, on purpose: what
   they would write is read by later draws.
7. **The 16 KB program buffer** is written whole when a draw's programs
   change. If the split line shows it, upload only the programs' own length
   behind a bound range that still covers the block (Turnip wants the bound
   range to cover what the shader declares; a smaller declared block with a
   second module for long programs is the other way).
8. **Fewer distinct pipelines.** `vulkan_dynamic_pipeline_state` (on by
   default) already moves cull, front face, topology, depth, stencil and,
   where the driver has it, blend out of the key. What is left in the key is
   the shaders' modifications and the render pass. I found nothing safe to
   remove without the device's numbers on which fields actually multiply
   pipelines; the record's `.xpso` file of a played title, read on a PC,
   would say (count descriptions for each shader pair and which field
   differs). Not built.
9. **Registers as separate variables** in the translator
   (`spirv-slimming.md` step 3 (a)) and its other steps: not built.
10. **Blocking on the rebuild with a progress screen**: the value is there;
    the screen is the app's (below). Today the rebuild runs behind the
    title's first frames and placeholders cover it.
11. **Speculation**: measured, not changed (above).

## For other sessions' files (not edited here)

- **App (Kotlin and JNI, not under `gpu/`)**: to show the launch rebuild,
  poll `CommandProcessor::GetShaderPreparationProgress` through the graphics
  system (it is safe from any thread) and show `done / total` while it
  returns true. To offer export and import, set `shader_record_export_path`
  and `shader_record_import_path` to a folder under the app's external files
  directory and expose them as two settings; export happens when a title is
  closed, import when one starts.
- **`boosted/HANDOFF.md`**: add this branch to "Right now" with the settings
  table above; nothing else of it is touched here.
- **`boosted/docs/ARCHITECTURE.md`**: the interpreter, version 2, and the rig
  deserve a paragraph each; the text under "How it is built" is written to
  be lifted.

## Findings on the way

- **A translator bug, fixed** (`spirv_shader_translator_alu.cc`): the vector
  `cube` operation written to `.x` or `.w` without `.y` left an id out of
  the module. Found by the rig. Unchanged for full write masks.
- **XenDroid's interpreter (version 1) is not the translated shader.** It
  differs in scalar operands, in exports, in the conditional execs 13 and
  14, in the predication of fetches and in what register 0's w starts as. So
  a version 1 placeholder can draw geometry elsewhere than the real pipeline
  does. Version 2 was written against the translator for that reason.
- **A translator oddity, kept**: for a 2D fetch with register gradients the
  translator uses `setGradientV`'s value as the horizontal gradient and
  `setGradientH`'s as the vertical. The interpreter does the same so that
  the two match. It only matters for anisotropic filtering. Whether the
  console does this is not known to me; upstream should be asked.
- **`PWLGammaToLinear` asserted fewer than four components** for no reason
  in its body; lifted to four for the vector form.
- **The default pixel modification multiplies colour 0 by zero**
  (`rt0_blend_*_factor_for_premult` 0 is `kZero`, `kOne` is 1). The pipeline
  cache always sets it, so no draw is affected; anything that translates a
  pixel shader with a default modification (a tool, a test) gets black.
- **The second `UpdateBindings` of a draw** (after render target transfers
  inside the pass) did not pass the stand-in's texture: found by the 02:30
  resume check, kept.

## Upstream files changed (all XenDroid's, each change marked `hakuX-Boosted`)

- `gpu/spirv_shader_translator_alu.cc`: the `cube` fix.
- `gpu/spirv_shader_translator_fetch.cc`: the vector form of the sign step,
  beside the switch, chosen by the setting.
- `gpu/spirv_shader_translator.cc`: the assertion in `PWLGammaToLinear`.
- `gpu/gpu_flags.{h,cc}`: `spirv_tfetch_vector_signs`.
- `gpu/command_processor.h`: `GetShaderPreparationProgress`.
- `gpu/vulkan/vulkan_shader.{h,cc}`: `GetUcodeProgram`.
- `gpu/vulkan/vulkan_pipeline_cache.{h,cc}`: the settings; the interpreter's
  modules; which draws and pixel shaders it takes and why not; the new kind
  of stand-in; the tessellation evaluation stage of a placeholder; counters
  and log lines; the creation threads' limit and policy; the driver cache
  check, the progress counters, import and export.
- `gpu/vulkan/vulkan_command_processor.{h,cc}`: which draws the interpreter
  may stand in for; the programs behind the bool and loop constants; the
  pixel interpreter's textures, samplers and float constants; the split
  line; the vertex budget; the second `UpdateBindings`.

New files, the fork's own: `gpu/ucode_program.{h,cc}`,
`gpu/shaders/ucode_program*`, `gpu/first_run_rig/*` (not built by the app).

## What the build cannot say

That any of it draws correctly on the Adreno driver, how long the driver
takes over the interpreter's modules (59 KB of SPIR-V for the vertex shader,
67 KB for the pixel shader, against version 1's 51 KB), whether Turnip
accepts a 16 KB uniform range or sixteen textures for a placeholder without
cost, whether dynamic branching on the opcode is fast enough for a full
screen of pixels (a placeholder that covers the screen runs the interpreter
for every pixel: if frames are slower with step 3 than with mode 2, that is
why, and mode 2 is the fallback), and everything about timing.

## History of the branch

- 2026-11-13 23:15: the interpreter, the rig, run 101.
- The owning session stopped (its limit) with two commits unpushed. Two
  scheduled resume checks (02:30, 04:30) wrote the vertex side into the draw
  path again in their own way: runs 105 and 117.
- 07:40: the owning session resumed and merged: its own code in
  `gpu/vulkan/`, the resume checks' setting names and their
  `UpdateBindings` fix. Run 122.
- Since: the pixel side and its rig tests, its draw path and the fetch's
  vector form (`ea37bf2`, run 124); then the thread policy, the record, the
  measurements and these notes.

## Builds of the last commits

BUILD_STATE

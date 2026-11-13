# Pixel stand-in (branch `claude/pixel-stand-in`)

Base: `7648e9c` (built green as run 44). **Nothing here has been run on a
device.** The checks were a green GitHub Actions build, a host syntax pass
with the project's warning flags, `spirv-val` over the new shaders, and a
host run of the microcode walk over four hand-made shaders.
Everything visual, and every timing, is unknown until the phone says.

Every new behaviour is behind a setting whose default is today's behaviour,
and all of it is inside the placeholder model, which is itself off
(`vulkan_placeholder_pipelines = false`). Merging this branch changes
nothing that runs today.

## Status

Done:
- Stage 1, the stand-in pixel shader: `3810bd9`, built green as run 60.
- Stage 2, placeholders for draws that had none (draws without a pixel
  shader, rectangle and quad lists) and a fuller vertex interpreter for the
  stand-in: `a68d869` and `5041d69`. Build status: see "Builds" below.
- Stage 3, design notes for a pixel ucode interpreter: in this file.

In progress: nothing.

Next (not started, see "What I would do next"): only after the phone has
shown what stage 1 looks like.

## Builds

| Commit | What | Run |
|---|---|---|
| `3810bd9` | stage 1 | 60, green |
| `a68d869` | stage 2 (depth-only and geometry placeholders) | 68, cancelled by the next push before it finished (a newer push replaces a running build) |
| `5041d69` and this file | stage 2 (interpreter vertex formats), notes | the run of the branch's tip: check it before merging |

The tip's run builds everything above together. If this table still says
"check it", look at the newest run of `claude/pixel-stand-in` on GitHub
Actions: the branch is only mergeable if that one is green.

## What was built

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/gpu/`.

### Stage 1: the stand-in pixel shader

With `vulkan_placeholder_pipelines = true` a draw whose pipeline is still
compiling is drawn through a placeholder pipeline. Its pixel shader used to
write no colour (depth only). With `vulkan_placeholder_ps_mode = 1` or `2`
it is a stand-in that draws:

- the texture the guest pixel shader fetches first, sampled with the guest's
  own sampler state by the interpolator that fetch reads,
- times the interpolated vertex colour, if the vertex shader passes a
  vertex colour attribute through to an interpolator,
- alpha 1 (mode 1), or the texture's alpha times the colour's with the
  guest alpha test applied (mode 2: keeps the shape of cut-out foliage and
  the softness of blended particles);
- with no usable texture: the vertex colour, or a mid grey.

It writes all four colour outputs with the same value (outputs without an
attachment are discarded; render targets the guest shader does not write
have a zero write mask in the pipeline, as before), applies each render
target's exponent bias and gamma flag as the real shaders do, and never
writes depth. Depth test, depth write, stencil, blending and culling are
the pipeline's fixed-function state, which is created from the same
description as the real pipeline, so they are the real pipeline's.

How it knows what to draw, without waiting for any translation:
`VulkanShader::GetStandInInfo` (`vulkan/vulkan_shader.cc`) walks the guest
microcode once per shader. For a pixel shader it lists up to four texture
fetches (1D, 2D or cube) whose coordinates are an interpolator the shader
has not overwritten, or a register computed from one by add, mul, max, min
or mad (a scaled or scrolled coordinate: the stand-in then uses the
unscaled interpolator). For a vertex shader it finds the interpolators
written with a copy of a normalised 8-8-8-8 vertex attribute (a move, or
the attribute times a constant).

The pieces:

- `shaders/placeholder_stand_in.glsli` and six wrappers
  `placeholder_stand_in_{flat,2d,cube}[_color].ps.glsl`: the pixel shader,
  without a texture, with a 2D one, with a cube one, each with or without
  a vertex colour input. The build's generator picks them up by itself.
- `shaders/ucode_interpreter_stand_in.vs.glsl`: a copy of XenDroid's ucode
  interpreter vertex shader that also outputs two interpolators (location
  0 the texture coordinate, location 1 the colour). The original outputs
  `gl_Position` only and is untouched.
- A placeholder with a stand-in has its own pipeline layout (one texture
  and one sampler for the pixel stage), kept in the pipeline slot
  (`Pipeline::placeholder_pipeline_layout`). The draw path binds with it.
- The choice is made per draw (`VulkanCommandProcessor::IssueDraw`): the
  first candidate texture that holds colour. A texture whose red, green or
  blue is signed or biased in its fetch constant is a normal map or other
  vector data and is skipped; if all are, the stand-in draws flat.
- The choice reaches the shaders in a padding word of the system constants
  (`texture_integer_scale_pad[0]`, std140 `uvec4 [34].z`), next to the two
  words the vertex interpreter already has there. No field was added, so
  `spirv_shader_translator.h` is untouched. Bit layout: top of
  `placeholder_stand_in.glsli`.
- With the interpreter vertex shader, every stand-in placeholder of a kind
  uses the same two shader modules, whatever the guest shaders are. That
  is the property that can make a placeholder cheap to create (the
  driver's cache can answer), and the reason the choice is a run-time word
  and not baked into the module.
- With a translated guest vertex shader (the placeholder upstream makes
  when the vertex shader is already translated) the interpolators are at
  packed locations, so the pipeline cache makes a copy of the pixel module
  with its two input locations rewritten
  (`MoveStandInPixelShaderInputs`). Run here over the generated modules
  and checked with `spirv-val`.

### Stage 2: draws that had no stand-in

Three settings, each off by default:

- `vulkan_placeholder_interpreter_translated_vs`: the interpreter also
  stands in for a vertex shader that is already translated. Today a new
  pipeline of a known vertex shader (new pixel shader, new state) gets a
  placeholder compiled from the translated vertex shader **on the GPU
  command thread** (finding 3 below): a hitch. With this on it gets the
  interpreter instead.
- `vulkan_placeholder_depth_only_interpreter`: a draw without a pixel
  shader (shadow maps, depth pre-passes) is not asynchronous at all in the
  placeholder model: its pipeline is translated and compiled on the GPU
  command thread when first drawn. With this on, and a vertex shader the
  interpreter can run, it gets an interpreter placeholder with no pixel
  shader (or the float24 depth conversion shader, where the real pipeline
  would have it), which draws what the real pipeline will, and the real
  one compiles in the background.
- `vulkan_placeholder_interpreter_geometry`: rectangle lists (how titles
  draw full-screen passes) and quad lists were excluded from the
  interpreter because their geometry shader passes on as many
  interpolators as the translated vertex shader writes. With this on, the
  placeholder gets a geometry shader built for the interpreter's outputs
  (the stand-in's two, or none).

And one that removes a hitch at the price of pop-in:

- `vulkan_placeholder_real_vs` (default true, as now). False: a draw the
  interpreter cannot stand in for gets no placeholder, instead of one
  compiled from its translated vertex shader on the GPU command thread; it
  is dropped until its real pipeline is ready.

The stand-in's copy of the interpreter also reads every vertex format. The
original converts eight; 16_16_16_16, 2_10_10_10, 10_11_11, 11_11_10 and
the 32-bit integers read as (0,0,0,1) there, so a vertex shader with short
positions or packed normals draws collapsed through the original. The copy
unpacks them as the translated shaders do, gives missing components 0 as
they do (the original has 1 in W), and applies the fetch's exponent bias.
This only takes effect with `vulkan_placeholder_ps_mode` 1 or 2.

## Which draws still get no stand-in, and why

With everything above switched on, the interpreter (and so a cheap
placeholder) is still refused for:

1. **Shaders loaded inline** (`IM_LOAD_IMMEDIATE`, microcode address 0):
   the interpreter reads the microcode from guest memory by its address.
2. **Tessellated draws** (the guest vertex shader is the evaluation
   shader).
3. **Point lists** (point sprites: particles). Their geometry shader needs
   a point size the interpreter does not output.
4. **Vertex shaders that fetch textures, export to memory, loop or call
   subroutines.** The interpreter has no texture sampling, treats `loop`,
   `endloop`, `call` and `ret` as no-ops and drops memory exports. Loops
   are the tractable one: a loop stack in `XeRunInterp`, the loop
   constants (already bound), and aL-relative register and constant
   addressing in `Operand`. Not done blind: a wrong loop draws exploded
   geometry, which is worse than pop-in.
5. **Devices without full 32-bit indices or with shared memory split over
   several bindings** (not this phone, as far as the code says).

For 1 to 4, what happens is unchanged: if the vertex shader is already
translated, a placeholder is compiled from it on the GPU command thread
(now with the stand-in pixel shader, at the packed locations), unless
`vulkan_placeholder_real_vs = false`; if it is not translated,
`async_shader_skip_draws` decides (true: dropped until ready, logged as
`Draw skipped (no interpreter placeholder, real pipeline not ready)`;
false: translated on the draw thread, then the same placeholder).

No stand-in by design:

- Pixel shaders that write no colour: the no-op pixel shader already draws
  all there is (depth).
- The fragment shader interlock render target path: colour goes through
  the EDRAM buffer there, not colour outputs. The app's default path is
  `performance`.

The interpreter's own limits (both copies, not changed): the predicate
push and kill operations and `setp_inv`, `setp_pop`, `setp_clr` and
`setp_rstr` are not implemented;
`mulsc`, `addsc` and `subsc` are approximate; no user clip planes; the
`kNoZero` signed fraction mode and index rounding are ignored; fetches
past the end of a buffer are not clamped to 0.

## Settings to flip on the phone

In the global config or the title's `config/<TITLE ID>.config.toml`. Use a
cold shader cache (delete `compose/cache/shaders` and the pipeline cache),
or nothing compiles and nothing is seen.

Step 1, the stand-in as specified:

```toml
[GPU]
shader_profiling = true
async_shader_vs_interpreter = true      # the default
async_shader_skip_draws = true          # the default

[Vulkan]
vulkan_placeholder_pipelines = true
vulkan_async_skip_draws = true          # the app's default. Must be true: finding 4
vulkan_placeholder_ps_mode = 1
```

Step 2: `vulkan_placeholder_ps_mode = 2` (alpha kept, alpha test applied).
Compare foliage, particles and anything blended with step 1.

Step 3, the least hitching the model can do today:

```toml
[Vulkan]
vulkan_placeholder_interpreter_translated_vs = true
vulkan_placeholder_depth_only_interpreter = true
vulkan_placeholder_interpreter_geometry = true
vulkan_placeholder_real_vs = false
```

If step 3 misbehaves, switch its four off one at a time; each is
independent.

For a baseline of what the fork's author rejected, the same as step 1 with
`vulkan_placeholder_ps_mode = 0`.

`vulkan_async_wait_fullscreen_draws` is untouched and only acts in the
deferred-bind model (`vulkan_placeholder_pipelines = false`).

## What to look for in `xe.log`

All with `shader_profiling = true`.

- `shader_profiling: stand-in made for VS ..., PS ...: interpreter vertex
  shader, 2D texture pixel shader, texture coordinate chosen per draw,
  vertex colour interpolator N`: one per placeholder with a stand-in. The
  split between `flat`, `2D texture` and `cube texture` says how often the
  microcode walk finds a texture. Many `flat` means the walk is too
  narrow.
- `shader_profiling: first draw with a placeholder: ... stand-in word
  XXXXXXXX (texture fetch constant F, by interpolator I, vertex colour
  interpolator C)`: once per placeholder. `-1` for the texture means that
  draw drew flat (every candidate was vector data).
- `shader_profiling: placeholder swapped for the real pipeline: ... N ms
  after it was made, M draw(s) recorded with it`: how long each stand-in
  was on screen.
- `shader_profiling: pipeline create (placeholder) VS ... PS ... X ms`
  (already there): **the number that decides whether this model is free of
  hitches.** These are created on the GPU command thread. The first of
  each kind compiles the interpreter (40 KB of SPIR-V, 52 KB for the
  stand-in's copy: expect tens of milliseconds once). After that they
  should be near zero, if Turnip's cache answers for the shared modules; if they are tens of
  milliseconds each, the driver recompiles the interpreter per pipeline
  and the placeholder model hitches whatever the pixel shader is. With a
  translated vertex shader they will be about the vertex shader's share
  of a compile.
- `VS interpreter placeholder created (interpreter VS + no-op PS)`:
  placeholders without a stand-in (the pixel shader writes no colour, or
  the draw has no pixel shader).
- `Draw skipped (no interpreter placeholder, real pipeline not ready)` and
  `Draw skipped (pipeline not ready yet)`: draws that got no placeholder
  (the list above). Their vertex and pixel shader hashes say which.
- `VS ... placeholder FAILED to create` and `Failed to make the ...
  stand-in pixel shader`: should not appear.
- `UpdateBindings: pipeline layout counts ... do not match`: should stay
  at 0. If it appears with the stand-in on, the placeholder layout
  handling is wrong: tell me the line.

## What could not be verified without a device

Everything that matters:

- That it draws at all, and how it looks: whether the first texture is the
  right one often enough, whether vertex colours are colours, whether mode
  2's alpha is right for blended draws.
- Whether Turnip accepts the stand-in pipelines (the validation layers are
  not available here; the modules pass `spirv-val`, the layouts were
  reasoned, not run).
- Whether the driver's cache makes interpreter placeholders cheap (above).
- The microcode walk on real shaders (no game data here). It was run on
  the host over four hand-assembled shaders, whose encoding the project's
  own disassembler confirmed: a plain `tfetch2D r1, r0.xy` (found, direct),
  coordinates scaled by a constant first (found, as derived, with the
  swizzle followed), a cube fetch (found), a fetch through an overwritten
  interpolator and a 3D fetch (both rightly not found), and a vertex shader
  passing an 8-8-8-8 attribute to two interpolators by `max` and by `mul`
  with a constant (both found; a float position passed the same way was
  not). What real titles' shaders look like to it is what the `flat` count
  in the log will say.
- The geometry-shader placeholder and the depth-only placeholder: reasoned
  from the code, never executed.
- The added vertex formats in the interpreter copy: mirrored from the
  translator's code, never executed.

Known approximations, by design: the stand-in ignores the fetch's offsets,
LOD bias and filter overrides; unnormalised coordinates are divided by the
guest texture size; gamma is a 2.2 power, not the console's
piecewise-linear curve; a pixel shader that writes depth or kills pixels
has its real depth only once the real pipeline arrives; with
`depth_float24_convert_in_pixel_shader` (off in the app's defaults) a
stand-in writes unconverted depth.

Things a reviewer should look at first: the per-draw block in
`IssueDraw` between "a placeholder with a stand-in pixel shader" and the
`RequestTextures` call (it overwrites `current_samplers_pixel_` for the
draw and resets the cross-draw sampler cache's shader), and
`UpdateBindings`' handling of `stand_in_texture`.

## Findings from reading the code

1. **A wait at the end of a submission cannot save the draws of that
   submission (deferred-bind model, the default).** The draw path records
   `CmdVkBindPipelineDeferred(..., current_pipeline)` with the handle it
   read when the draw was recorded (`vulkan/vulkan_command_processor.cc`,
   the `bind_guest_graphics_pipeline` lambda), and the replay drops every
   draw after a bind whose recorded handle is null, "even if the pipeline
   exists by now" (`vulkan/deferred_command_buffer.cc`, case
   `kVkBindPipelineDeferred`). `VulkanPipelineCache::EndSubmission` waits
   after the draws were recorded, so with `vulkan_async_skip_draws = false`,
   and for the full-screen passes of `vulkan_async_wait_fullscreen_draws`,
   the frame stalls and the draw is still dropped once; only the next
   submission has it. This is a likely cause of "the glitches that survive
   waiting for pipelines" in the backlog. Not changed here: it is the main
   branch's model, and the fix needs `vulkan/deferred_command_buffer.*`,
   which is not in this branch's row. The fix: a second bind command that
   carries the slot (`&pipeline->pipeline`) and loads the handle at
   replay, recorded instead of the by-value one when the handle is null
   and the slot's layout was complete when the draw was recorded
   (`!pipeline->pipeline_layout_stale` and both stages' bindings ready,
   which `async_shader_translate_on_draw` makes the normal case). The
   by-value bind was introduced because a draw recorded against the
   minimal layout has too few descriptor sets for the real pipeline; that
   reason does not hold when the layout was already the real one.
2. **The interpreter vertex shader outputs no interpolators**
   (`shaders/ucode_interpreter.vs.glsl` writes only `gl_Position`), hence
   the copy.
3. **A placeholder with the translated vertex shader is compiled on the
   draw thread.** `ConfigurePipeline` calls
   `EnsurePipelineCreatedWithPlaceholder` inline when the vertex shader is
   already translated: a driver compile of the real vertex shader on the
   GPU command thread, then the background compiles the pair again. Only
   the interpreter placeholder is cheap.
4. **In the placeholder model `vulkan_async_skip_draws = false` stalls for
   nothing**: `EndSubmission` blocks for every queued pipeline although
   the draws were already recorded with their placeholders.
5. **A draw without a pixel shader is synchronous in the placeholder
   model** (`CanCreatePipelineAsync` requires a pixel shader): translated
   and compiled on the GPU command thread. Every new shadow or depth
   pre-pass pipeline is a hitch there. In the deferred-bind model it is
   asynchronous like any other.
6. **The interpreter's vertex format coverage** (above): eight formats,
   W of a missing component 1 where the translated shaders have 0, no
   exponent bias. Worth fixing in the original too, with XenDroid.

## Upstream files changed, and why (for `docs/ARCHITECTURE.md`)

All under `x360/emulator-core/src/main/cpp/xenia/src/xenia/gpu/`. Every
change is marked `hakuX-Boosted:`.

| File | Change |
|---|---|
| `vulkan/vulkan_shader.h`, `.cc` | `StandInInfo`, `GetStandInInfo`: the microcode walk. New code only. |
| `vulkan/vulkan_pipeline_cache.h` | `StandInPixelShader`, `StandIn`; five fields in `Pipeline` (stand-in, placeholder layout, creation time, draw count) and their copies in its two constructors; a layout override parameter on `EnsurePipelineCreated`; `GetStandIn`, `GetStandInPixelShader`; a second parameter on `CanCreatePipelineAsync` and on `EnsurePipelineCreatedWithInterpreterPlaceholder`; the stand-in's shader modules. |
| `vulkan/vulkan_pipeline_cache.cc` | Five settings; the new modules made and destroyed; `ConfigurePipeline`'s placeholder block chooses the stand-in, its layout and, for the interpreter with a geometry shader, the geometry shader; `EnsurePipelineCreated` takes the layout override and logs the swap; `CanCreatePipelineAsync` admits pixel-shader-less draws the interpreter can run. |
| `vulkan/vulkan_command_processor.h` | One parameter on `UpdateBindings`, one member vector. |
| `vulkan/vulkan_command_processor.cc` | The interpreter's conditions read two of the settings; `IssueDraw` picks the stand-in's texture and sampler per draw, binds with the placeholder's layout, writes the stand-in word into the system constants' padding and logs the first draw; `UpdateBindings` binds the stand-in's one texture and sampler. |
| `shaders/placeholder_stand_in.glsli`, `placeholder_stand_in_*.ps.glsl` (6), `ucode_interpreter_stand_in.vs.glsl` | New files. |

Not changed: `gpu_flags.*` (the settings sit with
`vulkan_placeholder_pipelines` in `vulkan_pipeline_cache.cc`), the build
files (the generator finds the new shaders), `spirv_shader_translator*`,
`ucode_interpreter.vs.glsl`, anything outside this branch's row.

For the merge: `gen_android_spirv.py` regenerates a header only when its
`.glsl` is newer, and the six wrappers include the `.glsli`, so after
editing the `.glsli` in an existing build tree touch the wrappers (a CI
build is always fresh). The new settings are not in the app's settings
screen or `default_config.toml` (the UI session's files); they work from
the config file.

One thing to know when taking XenDroid in: the stand-in uses
`SystemConstants::texture_integer_scale_pad[0]`. If upstream gives that
padding a meaning, move the word.

## Stage 3: design notes for a pixel ucode interpreter

Not built. The question is whether a pixel shader in the style of
`ucode_interpreter.vs.glsl` could draw the guest pixel shader exactly
while the real pipeline compiles.

### What it would need

State and inputs:
- 64 temporary registers; r0 to r15 start as the interpolators that reach
  the pixel shader; the PsParamGen register (pixel position, facing,
  point coordinates) where `SQ_PROGRAM_CNTL.param_gen` asks for it.
- The pixel half of the float constants (256), bound whole instead of the
  packed subset the translated shaders get (the vertex interpreter does
  this for its half: `float_constants_vertex_are_full_`); the 256 bool
  constants and 32 loop constants (already bound).
- The microcode, read from guest memory by address as the vertex
  interpreter does. The command processor tracks only the vertex shader's
  address today (`active_vertex_shader_ucode_address_`, set in
  `pm4_command_processor_implement.h`): the pixel shader's needs the same
  two lines there, outside this branch's files. Inline shaders stay out.

Control flow: `exec` and its conditional and predicated forms, `jmp`,
`loop`/`endloop` with aL, `call`/`ret` (a four-deep stack), `alloc`.

Arithmetic: every vector and scalar operation, including the ones the
vertex interpreter leaves out (predicate pushes, the `kill` forms, which
become `discard`), operand swizzle, negate and
absolute value, result write mask, saturate and constant 0/1 writes,
a0- and aL-relative constant addressing, and the Shader Model 3 rule that
0 times anything is 0.

Texture fetches, the hard part:
- `tfetch1D/2D/3D/Cube` from any of 32 fetch constants, with per
  instruction: filter overrides, LOD bias, computed, register or explicit
  gradient LOD (`setTexLOD`, `setGradientH/V`), texel offsets,
  unnormalised coordinates; and `getBCF`, `getCompTexLOD`, `getGradients`,
  `getWeights`.
- Per fetch constant, at run time: dimension (2D, stacked, 3D, cube),
  signs per component (the translator samples an unsigned and a signed
  host texture and selects, which is why its fetch is 1,860 lines),
  exponent bias, resolution scaling of resolved textures, integer formats.
- A SPIR-V shader declares its sampled images with their dimension. An
  interpreter cannot know which fetch constant an instruction names until
  it runs, so it needs arrays: 32 2D-array, 32 3D and 32 cube images,
  twice for signed, plus samplers, indexed by a value read from the
  microcode. That needs a new pipeline layout (192 images), descriptor
  writes for every draw that uses it, and non-uniform indexing
  (`shaderSampledImageArrayNonUniformIndexing`, which Turnip has through
  descriptor indexing), because pixels that take different branches reach
  different fetches.
- Derivatives. A texture sample inside a loop over instructions is in
  non-uniform control flow, where implicit derivatives are undefined. The
  choices: carry derivatives with every register (three times the
  register file and the arithmetic), run the program three times, or give
  up exact LOD and take it from the interpolators' derivatives, computed
  before the loop. Only the last is affordable, and it is an
  approximation again.

The tail every translated pixel shader has: alpha test, alpha to
coverage, exponent bias, gamma, the four colour outputs, `oDepth`, the
float24 depth conversion and the in-shader polygon offset where those
are on. The fragment shader interlock path is out of scope as it is for
the stand-in.

### What an Adreno would pay per pixel

Estimates from the structure of the code, not measurements.

The vertex interpreter is affordable because it runs per vertex. Per
interpreted instruction it does three loads from the shared memory
buffer and three byte swaps, about twenty shifts and masks to decode,
two or three operand reads from a 64-entry register array indexed at run
time, a 30-way branch, and a masked write back: of the order of 100 GPU
operations for work the translated shader does in 2 to 6. Call it 20 to
50 times the cost of the compiled shader.

Guest pixel shaders in Fable II run from a few dozen to several hundred
instructions (the largest dumped one is 636 lines with 44 fetches). A
60-instruction shader over a 1280x720 pass is 0.9 million pixels times 60
instructions times about 100 operations: about 5.5 billion operations for
that one pass in that one frame, before overdraw, on vec4 values. That is
a large fraction of what the GPU has for a whole frame at 30 frames a
second. A small object costs in proportion to its pixels, so the
interpreter would be fine for a prop and ruinous for a full-screen pass
or a sky. On a tile-based GPU the register array also matters: 64 vec4
indexed dynamically does not fit registers, so every read and write
becomes memory traffic or a chain of selects.

Compile time is the other half. The interpreter itself must compile
once. The vertex one is 40 KB of SPIR-V (52 KB for the stand-in's copy
with every vertex format; the stand-in pixel shaders are 3 to 7 KB), and
run 37's numbers put a 30 to 76 KB pair at a median 39 ms on this driver.
A pixel one with the fetch machinery would be several times that. It compiles once per kind of pipeline state the driver keys
on, so the first use of each is a long hitch unless it is compiled at
start, before a title draws.

### What I would build instead, in this order

1. Keep the cheap stand-in and widen what the microcode walk recognises,
   guided by the log: second texture times the first (lightmaps), a
   constant colour (`mul` by a float constant), fetches whose coordinates
   come from PsParamGen (screen-space passes). Each is a few bits in the
   stand-in word and a few lines in one shader, with no new compile cost.
2. A fast path in the translator rather than an interpreter: for a pixel
   shader with no control flow, emit small SSA SPIR-V directly (registers
   as values, one sample per fetch, no run-time switches) and compile
   that as the stand-in. It is exact for most of what it covers, it costs
   a compile, but of a module a tenth the size, on a background thread.
   This is the same work as the `claude/spirv-slimming` branch from the
   other end, and the two should be planned together.
3. An interpreter only for what is left and only for small draws: no
   fetch arrays (at most the four textures the walk found, bound as
   today), a cap on instruction count, and a screen-area test so that a
   draw covering more than a few percent of the target keeps the cheap
   stand-in.

## What I would do next

1. Read the phone's log for step 1: the share of `flat` stand-ins and the
   `pipeline create (placeholder)` times decide everything else.
2. If interpreter placeholders are not cheap on Turnip: create one
   placeholder of each kind at start (a warm-up), or drop the model.
3. If they are cheap and the picture is acceptable: loops in the
   interpreter copy, point lists (a point size output), then item 1 of
   the list above.
4. Apply finding 1 on the main branch whatever happens here: it affects
   the model that ships.

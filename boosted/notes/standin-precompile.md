# Stand-in pipelines made ahead (branch `claude/standin-precompile`)

Base: `claude/integration-1115` at `6d39713`. **Nothing here has been run on a
device, and nothing here could be compiled or run on the machine it was
written on** (no C++ compiler, no glslang, no software Vulkan device there).
What Actions built is said at the end. Every statement about what the code
does on the phone is what the code is meant to do, not a measurement.

Paths below are under
`x360/emulator-core/src/main/cpp/xenia/src/xenia/gpu/`. Line numbers are those
of the base commit.

## Done / in progress / next

Done:

1. The key analysis (next section).
2. Shared stand-in pipelines, made on the creation threads and ahead of
   need, behind `vulkan_standin_precompile` (default off): "The design".
3. The pixel interpreter takes twelve 2D textures instead of six; the
   fallback stand-in no longer needs a module for each pair of shaders.
4. The `placeholder cover:` line has a second line for all of this.

In progress: nothing.

Next, in this order: a device run of the table under "Settings"; the rig on
a machine that can run it; 3D and stacked textures and the fetch queries in
the pixel interpreter (with the rig); texture fetches in the interpreter's
vertex stage; point lists.

The last section says what Actions built.

## Step 1: what distinguishes one placeholder pipeline from another

### Where a placeholder is made

`VulkanPipelineCache::ConfigurePipeline` (`vulkan/vulkan_pipeline_cache.cc`
1152) builds a `PipelineDescription` for the draw, canonicalises it and looks
it up in `pipelines_`. A description that is not there gets a new slot
(1243), and, in the placeholder model, `MakePlaceholder` (1742) is called
**for that slot** (1362). `MakePlaceholder` chooses three modules and a
layout and then calls `EnsurePipelineCreated` (4488) on the GPU command
thread (1877), which ends in one `vkCreateGraphicsPipelines` (5167) whose
result is stored in that slot alone (5275, 5297).

So the number of placeholder pipelines is the number of slots first drawn
before their real pipeline was ready: 194 in the Fable II run, 720 in the
Banjo run. **A placeholder is keyed by the slot, and the slot is keyed by the
guest shaders.**

### The slot's key (`PipelineDescription`, `vulkan/vulkan_pipeline_cache.h` 585)

| Field | From | Can the stand-in ignore it? |
|---|---|---|
| `vertex_shader_hash`, `pixel_shader_hash` | the guest shaders' microcode | Yes with the interpreter: its modules are the same for every guest shader. This is the field that makes hundreds |
| `vertex_shader_modification` | interpolator mask, host vertex shader type, and more (`GetCurrentVertexShaderModification`) | All but the host vertex shader type: it picks the interpreter's module (`GetProgramVertexShader`, 1600), says whether the stage is a tessellation evaluation shader (4024) and how many control points a patch has (4173, 4194) |
| `pixel_shader_modification` | interpolators, the generated parameter, depth mode, the blend factors a MIN/MAX blend premultiplies by | All but the two premultiply fields, which change a baked blend factor (4937 to 4984) and only when blending is not dynamic. The pixel interpreter refuses such shaders (1682), so for it they are constant |
| `render_pass_key` | target formats, which targets are bound, sample count (`NormalizeKeyForCacheLookup`, 3538) | No. Formats and sample count cannot be dynamic in Vulkan (5098 to 5124, 4841) |
| `geometry_shader` | rectangle list, quad list, point list (3590 to 3624) | No: another stage. The interpreter's geometry shader is one module for each type (16 interpolators, 1801) |
| `tessellation_mode`, `tessellation_patch` | `VGT_HOS_CNTL`, primitive type (3547 to 3586) | No: other stages and a patch size |
| `primitive_topology`, `primitive_restart`, `cull_*`, `front_face_clockwise`, depth and stencil fields | registers | Already out of the key: zeroed by `CanonicalizePipelineDescription` (3760) when the device has extended dynamic state, and set for each draw |
| `topology_class` | the topology | Kept only when the device lacks `dynamicPrimitiveTopologyUnrestricted` (3768): then up to four values |
| `depth_clamp_enable`, `polygon_mode` | registers | Out of the key only with the matching extended dynamic state 3 feature (3818, 3821) |
| `render_targets[]`: blend factors and ops, write mask | `RB_BLENDCONTROL`, colour mask | Out of the key only with EDS3 colour blend enable and equation both, and colour write mask (3830, 3838) |

### What `vkCreateGraphicsPipelines` is actually given for a placeholder

Beyond the description, `MakePlaceholder` adds:

1. **The vertex stage module**: with `vulkan_placeholder_interpreter = 2` one
   of ten fixed modules (plain, point sprites as strips, four tessellation
   evaluation shaders in two spacings, 1600); with version 1 one of two;
   without the interpreter (`vulkan_placeholder_real_vs`) **the translated
   guest vertex shader**, one module for each guest shader and modification.
2. **The pixel stage module** (`GetStandIn`, 4244):
   - the pixel interpreter (`vulkan_placeholder_ps_mode = 3`, not refused):
     one module;
   - refused, or mode 1 or 2: a stand-in pixel shader. With the interpreter's
     version 2 the stand-in's two inputs are **moved to the locations of the
     interpolators this pair of shaders uses** (`GetStandInPixelShader`, 4405,
     called at 4382): a new module for each (kind, has colour, texture
     coordinate interpolator, colour interpolator), up to 3 x 2 x 16 x 16;
   - a pixel shader that writes no colour, or no pixel shader: the no-op
     module or a float24 depth module (4454).
3. **The pipeline layout**:
   - pixel interpreter: one layout (16 textures, 8 samplers, 4280);
   - stand-in: by whether it has a texture, and, with a translated vertex
     shader, by that shader's texture and sampler counts (4390);
   - no stand-in: **the slot's own layout** (5145), which is the minimal one
     if the guest shaders were not translated when the slot was made and the
     layout for their texture counts if they were (1089). So two placeholders
     with the same modules can differ in layout.
4. **Vertex input**: none. The description of the vertex buffers is empty
   (4702): every vertex shader, translated or interpreter, fetches from the
   shared memory buffer itself. Vertex fetch is already independent of the
   guest's vertex declaration; `VK_EXT_vertex_input_dynamic_state` would move
   nothing.

### Dynamic state the code uses, and what it asks of the device

`Initialize` (`vulkan/vulkan_pipeline_cache.cc` 545 to 561), all behind
`vulkan_dynamic_pipeline_state` (on by default):

- `VK_EXT_extended_dynamic_state` and `_state2` together
  (`device_properties.extendedDynamicState`): topology, primitive restart,
  cull mode, front face, depth test, write and compare, stencil test and ops
  (5022 to 5043).
- `VK_EXT_extended_dynamic_state3`, each feature on its own bit: depth clamp
  enable, polygon mode, colour blend enable, colour blend equation, colour
  write mask (5045 to 5066), and the property
  `dynamicPrimitiveTopologyUnrestricted`.
- Always dynamic: viewport, scissor, depth bias, blend constants, stencil
  masks and reference (5005 to 5017).

Whether the bundled Turnip reports every EDS3 feature on the owner's phone is
not known to me from the code; the log line `placeholder cover:` now says
(see "The log line"). Where one is missing the stand-in's key keeps that
field, exactly as the real pipeline's key does.

### Why a title needs hundreds, and why each can be slow

- Hundreds, because of the slot: one `vkCreateGraphicsPipelines` for each
  (vertex shader, pixel shader, modifications, state) first drawn before its
  real pipeline, although with the interpreter the two guest shaders are not
  in what the driver is given.
- Whether the driver answers such a call from its cache depends on what it
  keys the cache by. What differs between two placeholders of one render pass
  and one vertex form, from the list above: the stand-in pixel module with
  moved inputs (73 of Fable II's 150 colour placeholders and 362 of Banjo's
  667 were refused by the pixel interpreter and took that path), the layout,
  and the vertex module where the interpreter was refused and the translated
  shader was compiled in (17 of 194 on Fable II, 2 of 720 on Banjo). Each new
  combination is, as far as the driver can tell, a new program: by my reading
  of Mesa's Turnip (not checked on the phone) the stages of a pipeline are
  linked together before they are compiled, and the layout is hashed into the
  key, so a new pixel module or a new layout compiles the interpreter's
  vertex shader (59 KB of SPIR-V) again. That fits "slowest 420 to 450 ms"
  being common enough to give 17 seconds with a frame over 100 ms; it is a
  reading, not a measurement.

### How many a title should need

With the interpreter for both stages, the stand-in's key is:

    render pass key  x  vertex form  x  pixel form  x  static leftovers

- vertex forms: plain; plain with the rectangle list geometry shader; plain
  with the quad list one; point sprites as strips; tessellated (patch kind x
  control points x mode). A title uses two to four.
- pixel forms: the pixel interpreter; a fallback for what it refuses (flat,
  2D, cube: three, once the stand-in's inputs are no longer moved for each
  pair); no colour (no-op); float24 depth. Six at most.
- static leftovers: none where the device has every feature listed above.
- render pass keys: what the title binds; ten to thirty in a session by the
  count of distinct keys in a `.xpso` record's descriptions (not counted on
  the owner's records here: the files are on the phone).

So of the order of **a few tens for a title, of which only (vertex forms x
pixel forms), about a dozen and the same for every title, differ in anything
a shader compiler sees**; the rest differ in attachment formats and sample
count. That dozen can be made before a title draws anything.

## The design (`vulkan_standin_precompile`, default off)

All of it is in `vulkan/vulkan_pipeline_cache.{h,cc}`, with five small
changes in `vulkan/vulkan_command_processor.cc`. It needs
`vulkan_placeholder_pipelines = true` and `vulkan_placeholder_interpreter = 2`
(`IsStandInPrecompileActive`); without them the setting does nothing.

### What a stand-in is keyed by

A **recipe** (`StandInRecipe`): the slot's canonical description with

- the two shader hashes set to 0;
- the vertex modification cut down to the host vertex shader type;
- the pixel modification cut down to the two premultiply factors (0 when the
  draw has no pixel shader);

plus the **pixel form** (`StandInPixelForm`): no-op, debug colour, float24
depth, the pixel interpreter, or the flat, 2D or cube fallback. Nothing in it
is a handle or a guest shader, so it is the same on every launch and can be
written to a file. `BuildStandInRecipe` makes it; `GetSharedStandIn` turns it
into modules, a layout and a render pass, the same way for a draw, for the
record and for the warm-up, so the three cannot disagree.

What is left in the key is therefore exactly the "How many a title should
need" list: render pass key, vertex form (host vertex shader type, geometry
shader type, tessellation mode and patch), pixel form, and whichever of
topology class, depth clamp, polygon mode, blend and write mask the device
cannot make dynamic.

Two things were removed from what multiplied pipelines:

- **The layout.** A shared stand-in always has its own layout (the pixel
  interpreter's; one texture and one sampler; or none), never the slot's. The
  slot keeps it in `placeholder_pipeline_layout` whatever the pixel form, and
  the draw path binds descriptor sets with it (the change at the
  `stand_in_layout` test in `IssueDraw`). Before, a placeholder without a
  stand-in used the slot's layout, which changes when the guest shaders are
  translated.
- **The fallback stand-in's moved inputs.** Three new modules
  (`shaders/placeholder_stand_in_{flat,2d,cube}_all.ps.glsl`, one more
  `#ifdef` in `placeholder_stand_in.glsli`) take all sixteen interpolators
  and read the two named by bits 0:3 and 4:7 of the stand-in's per-draw word,
  which `IssueDraw` already fills for every stand-in. So the fallback is
  three pipelines for a render pass and vertex form instead of one for each
  pair of interpolators. If one of the three modules cannot be made the old
  copies with moved inputs are used, and the recipe then has the locations.

### How a slot gets one

`MakePlaceholder`, when the placeholder's vertex stage is the interpreter's
version 2:

1. builds the recipe and asks `GetSharedStandIn` (`from_draw`): a new recipe
   is queued for the creation threads at the placeholders' priority; one that
   is queued behind other work is queued once more at that priority (the
   second job to reach it is dropped by `SharedStandIn::claimed`);
2. `TryAdoptSharedStandIn`: if the stand-in is ready the slot takes its
   handle (flags and layout first, then a compare-and-swap of the handle into
   the empty slot, so a real pipeline that arrived meanwhile wins);
3. if it is not ready, the draw waits on `creation_progress_cond_` for what
   is left of `vulkan_placeholder_async_wait_ms` (8 ms a frame, shared by
   every wait of the frame) and tries again;
4. if it is still not there the slot is left pending
   (`Pipeline::stand_in_pending`), the draw is skipped and counted as
   "placeholder compiling", and `OnPipelineReused` tries again at each later
   draw of the slot.

`EnsurePipelineCreated` is called for a stand-in only from
`CreationThread` (job kind `kStandIn`). It creates into the stand-in's own
slot object, so the 860 lines of that function are unchanged but for null
checks on the guest vertex shader in its log lines.

The real pipeline replaces the handle in the slot as before; the old handle
is not queued for destruction when it is a shared one
(`Pipeline::placeholder_shared`), and `Shutdown` destroys the shared ones
once.

### What else the setting turns on, and why

"Never compile on the GPU command thread" needed three more things, all
only with the setting:

- The interpreter stands in for a vertex shader that is already translated
  (as `vulkan_placeholder_interpreter_translated_vs` does): otherwise the
  placeholder was compiled from the translated shader on the GPU command
  thread (17 of Fable II's 194).
- **No placeholder is made from a translated vertex shader at all**
  (`vulkan_placeholder_real_vs` is ignored). On a creation thread it would
  compile the guest's vertex shader once for the placeholder and again for
  the real pipeline, arriving little before the real one it delays. Such a
  draw (one the interpreter refuses) is skipped until its real pipeline is
  ready and counted under "no placeholder". The GPU command thread does not
  translate for it either.
- A draw without a pixel shader is always asynchronous
  (`CanCreatePipelineAsync`), with the interpreter's stand-in where the
  interpreter takes the vertex shader, as
  `vulkan_placeholder_depth_only_async` and `_depth_only_interpreter` do.
- A slot that has no placeholder gets the interpreter's at a later draw when
  it can have one (the `vulkan_placeholder_late` path, for the interpreter
  only).

### Made before they are needed

- **At a title's start** (`StartStandInPrecompile`, called from
  `InitializeShaderStorage` after the driver's cache has been replaced by
  the title's; the storage is initialised on the GPU command thread):
  - a warm-up set for the console's most usual targets (8-bit colour, 24-bit
    depth with stencil, one sample): the pixel interpreter for a plain draw,
    the same with the rectangle list geometry shader if
    `vulkan_placeholder_interpreter_geometry` is on, and the depth-only
    form. Its purpose is to put the interpreter's modules through the
    driver's compiler before the title draws. Whether that makes the
    stand-ins of other targets cheap depends on whether the driver's cache
    is keyed by attachment formats (Turnip's, by my reading, is not); the
    log line says (compare the mean time of "a draw" and "new render pass"
    stand-ins with the warm-up's);
  - every recipe in the title's stand-in record (below);
  - the usual forms for every render pass key in the title's stored
    pipeline descriptions (`.xpso`), so that from the second launch on every
    render pass the title used has its stand-ins before the first frame.
- **When a render pass key is drawn to for the first time**
  (`NoteStandInRenderPass`, one comparison a draw in `ConfigurePipeline`):
  the same three forms for that key, whether or not the draw needs a
  placeholder.
- Other forms (quad lists, point sprites as strips, tessellation, the
  fallbacks, a pixel shader that writes no colour) are made when a draw
  first needs them.

The guesses are only made where the device has extended dynamic state;
where an EDS3 feature is missing the guessed recipe has "no blending, all
channels, no depth clamp, filled polygons", which may not be what the draw
has. `stand-in pipelines queued` against `slots that found their stand-in
ready` shows how well the guesses do.

### Kept for later launches

- The pipelines themselves: in the driver's cache
  (`<cache>/shaders/local/TITLEID.vk.bin`), saved every 20 s as before.
- The recipes: `<cache>/shaders/shareable/TITLEID.standin.vk.bin`, the
  recipes a draw asked for, written at most every 10 s and when the title's
  storage is closed (to a `.tmp`, then renamed). A header with the
  description's and the modification's versions and the recipe's size; a
  record of another version is ignored. A stored recipe is taken only if
  this device would build the same one (canonical for this device's dynamic
  state, a known pixel form, no hashes). It holds no game data. It is not
  part of `shader_record_export_path` / `_import_path`: the `.xpso` file
  those copy already gives the render pass keys.

### The log line

`placeholder cover:` is followed by a second line with the same first words
(so a tool that takes the last `placeholder cover:` line must take the last
two):

    placeholder cover: stand-ins: shared stand-in pipelines on (dynamic
    state: extended true, depth clamp ..., polygon mode ..., blend enable
    ..., blend equation ..., colour write mask ..., any topology ...);
    distinct stand-in pipelines queued N (the start-up warm-up a, the
    title's record b, a render pass seen for the first time c, a draw that
    needed one d); created on creation threads N (warm-up a in T ms, record
    b in T ms, new render pass c in T ms, for a draw d in T ms; slowest S
    ms), failed F, not possible on this device U; placeholders created on
    the GPU command thread G; pipeline slots that found their stand-in
    ready R, that waited for it W (T ms in all, longest L ms; I had it
    within the wait), that took it at a later draw X, that never did (the
    real pipeline came first) Y; draws skipped while a stand-in was being
    made K; render pass keys seen P; recipes taken from the record Q

- "created on which thread": a shared stand-in is only ever created on a
  creation thread; `placeholders created on the GPU command thread` is the
  old path's counter and must stay 0 with the setting on.
- The dynamic state flags are the device's answer to step 2 (a): any
  `false` there is a field that still multiplies stand-ins (and real
  pipelines).

## The pixel interpreter's refusals

- **Too many textures: lifted from six 2D slots to twelve** (two cube slots
  as before). `ucode_program::kMaxTextures2D`, the declarations and the
  switch in `shaders/ucode_program_pixel.glsli`, and the rig's descriptor
  layout follow the constant. Fourteen slots is what four bits of a fetch's
  word can name. The layout is now 28 textures and 14 samplers for the pixel
  stage; `GetProgramPixelRefusal` refuses ("no module") on a device whose
  `maxPerStageDescriptorSampledImages` or `maxPerStageDescriptorSamplers` is
  lower. The pixel module is larger by six slots' sampling code: its compile
  time on the phone is the `warm-up` figure of the log line. A shader with
  more than twelve different (texture, filters) pairs is still refused.
- **1D or 3D texture: not lifted.** The translator's fetch
  (`spirv_shader_translator_fetch.cc`) does much more for these than for
  2D: a 1D fetch checks the fetch constant's real dimension and remaps wide
  textures to a 2D grid (1239 on); a 3D-or-stacked fetch samples a 3D view
  or two layers of a 2D array chosen at run time, with its own filtering
  between layers (1324, 1882, 1960). Writing that in the interpreter without
  running the rig against the translator would be an unchecked copy of 400
  lines; the fallback stand-in such a shader gets is at least known. To do
  it: a third kind of slot (a `texture3D` and a `texture2DArray` view of the
  same fetch constant), `kFetchTexture3D` in the fetch word, the two paths
  of the translator in `XeUpTextureFetch`, and rig tests with a stacked and
  a 3D texture, on a machine that can run the rig.
- **Fetch query** (`getWeights`, `getCompTexLOD`, `getGradients`, `getBCF`):
  not lifted, for the same reason (578 to 900 of the same file).
- What these now cost in the new mode: a refused pixel shader's placeholder
  is one of three fallback stand-ins for its render pass and vertex form
  (not one compile on the GPU command thread for each pair of
  interpolators), so the refusals no longer cause hitches, only
  approximate pixels until the real pipeline is ready.

## The vertex side's two skip reasons

- **"vertex shader not translated"** (`kSkipVertexShaderNotTranslated`,
  `IssueDraw`'s `drop_until_ready`): the interpreter was not allowed to
  stand in for this draw **and** the vertex shader was not translated yet,
  so there was nothing at all to draw with. Counted for every draw until the
  shader is translated on a creation thread.
- **"no placeholder"** (`kSkipNoPipeline`): the slot has neither a
  placeholder nor its real pipeline. Mostly the same draws a little later:
  the shader is translated by now, but the slot was made without a
  placeholder and `vulkan_placeholder_late` is off, so nothing is tried
  again; also a placeholder that failed.

Both are therefore the draws the interpreter refuses. For version 2 that is
(`GetProgramInterpreterRefusal`): rectangle and quad lists unless
`vulkan_placeholder_interpreter_geometry`; tessellated draws unless
`vulkan_placeholder_interpreter_tessellation`; vertex shaders that fetch
textures unless `vulkan_placeholder_interpreter_vertex_textures`; point
lists expanded by a geometry shader; memory export; and, before this
branch's setting, any vertex shader that was already translated. Which of
them is the 448 and the 497 is in the `Draw skip reason:` lines of the
device logs, which I could not read from here. The interpreter can take the
first three with settings that exist (the second and third with the limits
`shader-first-run.md` gives); they are steps of the device order below, not
implied by the new setting, so that each can be judged on its own.
Rectangle lists are how full-screen passes are drawn, and a skipped
full-screen pass is a black or stale frame: the first candidate for the
black flicker at the start.

With the new setting the count under "no placeholder" also takes the draws
that used to get a placeholder compiled from their translated vertex shader
on the GPU command thread.

## Settings, in the order to try them on a device

Base: the configuration of the 2026-11-14 runs (`vulkan_placeholder_pipelines
= true`, `vulkan_placeholder_async = false`, `vulkan_placeholder_interpreter
= 2`, `vulkan_placeholder_ps_mode = 3`, `vulkan_placeholder_depth_only_async
= true`, `vulkan_pipeline_creation_adaptive = true`, `shader_profiling =
true`), cold caches, the same title and place each time. One change a run.

| Step | Setting | Set to | What to compare |
|---|---|---|---|
| 0 | none | | Nothing changes but the pixel interpreter's slots: in `placeholder cover:`, `refused (... too many textures N ...)` should fall against 51 (Fable II) and 350 (Banjo); the new `stand-ins:` line appears with `shared stand-in pipelines off` and the device's dynamic state flags. `placeholders made on the GPU command thread` and its `slowest` stay as they were (194, 420 ms; 720, 452 ms) |
| 1 | `vulkan_standin_precompile` | true | `placeholders made on the GPU command thread` **0**; `created on creation threads` a few tens, with `slowest` and the per-origin times; `slots that found their stand-in ready` close to the old count of placeholders, `waited` and `took it at a later draw` small; `draws skipped while a stand-in was being made` small. In the profile: seconds with a frame over 100 ms in the first two minutes against 5 (Fable II) and 17 (Banjo). On screen: the same picture as step 0 or better, no device loss |
| 2 | the same, second launch (warm) | | `the title's record` and `recipes taken from the record` non-zero; `a draw that needed one` near 0; the times near 0 (the driver's cache) |
| 3 | `vulkan_placeholder_interpreter_geometry` | true | `draws skipped (vertex shader not translated, no placeholder)` fall; `Draw skip reason` lines with `rectangle or quad list` gone; the black flicker at the start |
| 4 | `vulkan_placeholder_interpreter_tessellation` | true | as `shader-first-run.md` step 4 |
| 5 | `vulkan_placeholder_interpreter_vertex_textures` | true | as `shader-first-run.md` step 7 |
| 6 | `vulkan_placeholder_async_wait_ms` | 0, then 16 | only if step 1's `waited ... ms` is a visible part of a frame (0), or if `draws skipped while a stand-in was being made` is large and the waits are short (16) |

### What a device run must show before the setting becomes the default

1. Step 1 on three titles (Fable II, Banjo, Gears of War): `placeholders
   created on the GPU command thread 0`, no device loss, no draw that was
   right at step 0 and is wrong or missing at step 1 (the owner's eye, and
   `draws skipped` not above step 0's by more than the `no placeholder`
   draws that used to get a translated-vertex-shader placeholder).
2. Fewer seconds with a frame over 100 ms in the first two minutes than
   step 0, on each of the three.
3. `failed 0`, and `not possible on this device` explained if not 0.
4. Step 2 shows the record working: a warm launch queues its stand-ins from
   the record and compiles nothing for a draw.
5. The frame rate once frames flow is not below step 0's (the warm-up and
   the stand-ins of new render passes run on the creation threads at the
   placeholders' priority, ahead of real pipelines: if real pipelines arrive
   later for it, `kPriorityStandInAhead` should go below them).

## Limits and risks

- **Nothing was compiled or run locally.** The C++ was checked by Actions
  only; the GLSL (the twelve slots, the three new stand-in modules) by the
  Actions shader step only. The rig has one new test
  (`ps_tfetch_all_slots`) that has never been run, and the rig itself was
  not rebuilt after the change of its descriptor layout.
- The driver's behaviour is assumed, not known: that stand-ins differing
  only in attachment formats are cheap after the first, and that 28
  textures in a layout cost nothing. The log line's times answer the first.
- A stand-in that is still compiling when its slot is first drawn costs
  that draw (skipped) where the old path drew it after a hitch. The
  trade is the point of the setting, but a title that changes render
  targets often in its first seconds will show it as missing draws rather
  than long frames until the record exists.
- The warm-up set compiles up to three pipelines a title may never use.
- A draw the interpreter refuses has no placeholder at all with the
  setting (before: one compiled from its translated vertex shader, with a
  hitch). If step 1 shows more missing geometry than step 0, that is why,
  and steps 3 to 5 are the answer, not `vulkan_placeholder_real_vs`.
- Shared handles: a slot's placeholder handle is now also other slots'.
  The code never destroys one before `Shutdown`; anything added later that
  destroys a slot's placeholder must check `placeholder_shared`.
- `InitializeShaderStorage` replaces the driver's cache object. A stand-in
  (or any pipeline) still compiling against the old object at that moment
  is an existing hazard of that function; the warm-up is queued after the
  replacement for that reason, but a title drawing before its storage is
  initialised would queue stand-ins earlier, as it queues real pipelines.

## Upstream files changed (each change marked `hakuX-Boosted`)

- `gpu/vulkan/vulkan_pipeline_cache.h`: the recipe, the shared stand-in, the
  job kind, the counters, `Pipeline::placeholder_shared` and the pending
  fields.
- `gpu/vulkan/vulkan_pipeline_cache.cc`: the setting; the functions of the
  design; `MakePlaceholder`'s shared branch; `ConfigurePipeline`,
  `OnPipelineReused`, `CanCreatePipelineAsync` (what the setting turns on);
  `CreationThread` (the job); `EnsurePipelineCreated` (null guest vertex
  shader in log lines, a shared handle is not destroyed); `Shutdown`;
  `InitializeShaderStorage`, `ShutdownShaderStorage`, `EndSubmission` (the
  record); `LogStatsIfDue` (the line); `GetProgramPixelRefusal` (the device
  limits); the pixel interpreter's module is now destroyed at shutdown (it
  was not).
- `gpu/vulkan/vulkan_command_processor.cc`: the interpreter for translated
  vertex shaders, no translation on the draw thread for a placeholder, and
  the stand-in's layout for every shared placeholder, all only with the
  setting.

The fork's own files: `gpu/ucode_program.h` (twelve slots),
`gpu/shaders/ucode_program_pixel.glsli`, `gpu/shaders/placeholder_stand_in.glsli`
and the three new `placeholder_stand_in_*_all.ps.glsl`,
`gpu/first_run_rig/rig.cc` and `rig_pixel.h`.

## What I could not verify

Everything about behaviour: that a shared stand-in draws what the per-slot
placeholder drew; that the adoption race with the real pipeline is as the
code reads; the times; the record's round trip; that the new GLSL does what
it says (only that it compiles, if the build is green). The rig could not be
built or run here (no compiler, no glslang, no software Vulkan device on
this machine, and I did not install any); it needs running on a Linux
machine as its README says before the twelve slots are trusted.

## For other sessions' files (not edited here)

- `boosted/HANDOFF.md`: add this branch to "Right now" with the table above.
- `boosted/tools/device-watch.py` (if it reads `placeholder cover:`): there
  are now two lines with that prefix; the second starts `placeholder cover:
  stand-ins:`.

## Builds

State when the work was parked (2026-11-15, the owner's usage limit):

- Run 43654700309 (`9bc01e4`: the shared stand-in pipelines, the record, the
  log line): job "Xbox core (hakuX)" **green**; job "APK (both cores)" was
  still at "Build the debug APK" when last looked at, and the push of the
  next commit cancels it. So that commit's C++ compiles for the phone; the
  APK was not seen packed.
  https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43654700309
- `93f3cee` (twelve texture slots, the three stand-in modules that take
  every interpolator) and the commit after it (these notes, the rig's
  README): **pushed, build not seen.** Its risks for the build are the new
  GLSL (`placeholder_stand_in_*_all.ps.glsl`, the longer
  `ucode_program_pixel.glsli`) in the Actions shader step and the three new
  generated headers `vulkan_pipeline_cache.cc` includes. Check
  `https://api.github.com/repos/ctyldsley/hakuX-Boosted/actions/runs?branch=claude/standin-precompile&per_page=2`
  before taking an APK from this branch; if the shader step fails, reverting
  `93f3cee` gives back the state of the run above.

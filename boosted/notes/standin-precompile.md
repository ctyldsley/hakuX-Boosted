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

- Done: the key analysis (this file, next section).
- In progress: the shared stand-in pipelines behind `vulkan_standin_precompile`
  (design below).
- Next: the log line, the record, the pixel interpreter's refusals, the rig.

(This list is rewritten at each commit; the last section says what the last
build was.)

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

## The design

(Written as the code lands; see the first section for how far it is.)

One new setting, `vulkan_standin_precompile` (default off). With it, and with
`vulkan_placeholder_pipelines` and `vulkan_placeholder_interpreter = 2`:

1. A placeholder whose stages are both fixed modules is not made for the
   slot. Its **recipe** (the description with the shader hashes removed and
   the modifications cut down to what the table above keeps, plus the pixel
   form) is looked up in a map of shared stand-in pipelines. The slot borrows
   the shared handle; the real pipeline replaces it in the slot as before and
   the shared one is never destroyed while the cache lives.
2. A recipe that is not in the map is queued for the creation threads ahead
   of everything else. The draw waits for it for what is left of a small
   allowance for the frame, and is otherwise skipped and counted; the slot
   takes the handle at a later draw. The GPU command thread never calls
   `vkCreateGraphicsPipelines` for a stand-in.
3. Recipes are queued before they are needed: a warm-up set at start-up, the
   recipes the title's record holds from earlier runs, and the common forms
   whenever a render pass key is seen for the first time.
4. The recipes made are kept in a file beside the title's shader record, and
   the pipelines themselves in the driver's cache as every pipeline is.
5. Draws the interpreter could not take, so that fewer fall back to a
   translated vertex shader compiled into a placeholder: see the sections on
   refusals.

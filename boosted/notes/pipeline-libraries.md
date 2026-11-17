# Graphics pipeline libraries (branch `claude/pipeline-libraries`)

Base: `claude/integration-1116` at `22d0213`. **Nothing here has been run on a
device, and nothing here could be compiled on the machine it was written on.**
What Actions built is said at the end. Every statement about what the code
does on the phone is what the code is meant to do, not a measurement; every
statement about a driver is a reading of its source, with the file named.

Paths below are under
`x360/emulator-core/src/main/cpp/xenia/src/xenia/`. Line numbers are those of
the base commit.

## Done / in progress / next

Done:

1. Feasibility (this file, sections 1a to 1f), committed before any code.

In progress:

2. The start-up log line that answers 1a on a device (`ui/vulkan/vulkan_device.cc`).
3. The design of 1f behind `vulkan_pipeline_libraries` (default off).

Next: see "Device test order" and "Not built".

The last section says what Actions built. This list is brought up to date by
the last commit of the branch.

## The evidence this starts from

Cold-cache runs on the owner's phone (Snapdragon 8 Elite class, bundled
Turnip), the line `pipeline compiles by shaders the driver had compiled
before` (`gpu/vulkan/vulkan_pipeline_cache.cc` 3512; counted at 5989, where
"shader" is a translation: a guest shader with its modification word):

| Title | neither seen | one of the two | both |
|---|---|---|---|
| Banjo-Kazooie: Nuts & Bolts | 141 (46,202 ms, mean 327.7) | 388 (143,138 ms, mean 368.9) | 253 (47,745 ms, mean 188.7) |
| Fable II | 48 (4,990 ms, mean 104.0) | 115 (15,813 ms, mean 137.5) | 101 (12,119 ms, mean 120.0) |

A pipeline whose two shaders the driver has both compiled before still costs
120 to 190 ms. Section 1b says why, from Turnip's source: its cache key for a
compiled stage is a hash of the whole pipeline's stages and layout.

## 1a. Does the driver offer it

**The bundled Turnip: yes, by its source, with all three flags true. Not yet
seen in a device log.**

- The lock file (`boosted/drivers/turnip.lock.json`) pins
  StevenMXZ/Adreno-Tools-Drivers `v39`, published 2026-11-07, "Turnip Gen8
  V39 / Turnip v27.0.0-R2", whose release text says "build from upstream
  mesa" (and, for the Gen8 package, "patched with extra patchs unmerged").
  Mesa `main` on that day is `27.0.0-devel` (`VERSION` at commit `60d7516`
  of 2026-11-07, read through the mirror
  https://github.com/sailfishos-mirror/mesa because freedesktop's GitLab
  refuses automated readers).
- `src/freedreno/vulkan/tu_device.cc` of that tree
  (https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/vulkan/tu_device.cc,
  mirror
  https://github.com/sailfishos-mirror/mesa/blob/main/src/freedreno/vulkan/tu_device.cc):
  line 269 `.KHR_pipeline_library = true`, line 349
  `.EXT_graphics_pipeline_library = true`, line 789
  `features->graphicsPipelineLibrary = true`, lines 1464 and 1465
  `props->graphicsPipelineLibraryFastLinking = true` and
  `props->graphicsPipelineLibraryIndependentInterpolationDecoration = true`.
  None is behind a condition on the GPU generation or a debug flag, so Adreno
  7xx and 8xx get the same answer.
- Mesa's feature list
  (https://docs.mesa3d.org/features.html, source `docs/features.txt`):
  `VK_EXT_graphics_pipeline_library DONE (anv, hk, lvp, nvk, panvk, radv, tu,
  vn)`. Turnip has had it since Mesa 22.3; the FOSDEM 2024 Turnip talk lists
  it among the extensions done for DXVK
  (https://archive.fosdem.org/2024/events/attachments/fosdem-2024-2033-turnip-update-on-open-source-vulkan-driver-for-adreno-gpus/slides/22737/Update_on_Open_Source_Vulkan_Driver_for_Adreno_gqrqgmC.pdf).
- `VK_EXT_shader_object`: **not in Turnip** (`DONE (anv, lvp, hk, nvk,
  radv)`, no `tu`; no mention in `tu_device.cc`). So shader objects are not
  an alternative on this driver.
- `VK_EXT_extended_dynamic_state3`: in Turnip, with polygon mode, depth clamp
  enable, depth clip enable, logic op enable, sample mask, rasterisation
  samples, alpha to coverage enable, colour blend enable, colour blend
  equation and colour write mask all true, and
  `dynamicPrimitiveTopologyUnrestricted = true` (same file). The five the
  fork uses are all there, so on Turnip nothing of blend, depth clamp or
  polygon mode is left in the pipeline key.
- `VK_KHR_pipeline_binary`: not in Turnip (`DONE (anv, hk, nvk, panvk,
  radv)`).

**What I could not establish:**

- Whether the "extra patches" of the Gen8 V39 package touch any of this. The
  package is a binary; its patches are not published with the release.
- **The Qualcomm proprietary driver.** I found no extension list for current
  Adreno 7xx or 8xx proprietary drivers that I could read (gpuinfo.org's
  device tables are filled in by script and came back empty). What gpuinfo
  does give: on Android `VK_EXT_graphics_pipeline_library` is reported by
  18.9% of devices and `VK_EXT_extended_dynamic_state3` by 22.3%
  (https://vulkan.gpuinfo.org/listextensions.php?platform=android), against
  68% for `VK_EXT_extended_dynamic_state`. So most Android drivers do not
  have it and the path without it has to stay; whether Qualcomm's does is for
  a device log.

**What a device log must confirm** (one line, added by this branch, printed
at device set-up whatever is set):

    pipeline libraries: VK_EXT_graphics_pipeline_library offered yes (VK_KHR_pipeline_library yes), graphicsPipelineLibrary yes, fast linking yes, independent interpolation decoration yes; ...

once with the bundled Turnip and once with an empty `vulkan_lib_path` (the
phone's own driver; `driverID: QualcommProprietary` a few lines above it).

## 1b. How Turnip implements it

All from `src/freedreno/vulkan/tu_pipeline.cc` and `tu_shader.cc` of the tree
above
(https://github.com/sailfishos-mirror/mesa/blob/main/src/freedreno/vulkan/tu_pipeline.cc,
https://github.com/sailfishos-mirror/mesa/blob/main/src/freedreno/vulkan/tu_shader.cc).

**Why a pipeline of two known shaders compiles again today.**
`tu_pipeline_builder_compile_shaders` (`tu_pipeline.cc` 1745) hashes the
pipeline with `tu_hash_shaders` (1560): the layout's hash, **every** stage's
SPIR-V hash and key, and the library state bits. The cache key of each
compiled stage is that one hash with the stage's number appended (1974 to
1990). A new pair of shaders is a new hash, so neither stage is found, and
`tu_compile_shaders` (`tu_shader.cc` 3912) runs for both: SPIR-V to NIR for
each stage, `tu_lower_nir`, `tu_link_shaders` (3863: `nir_opt_varyings_bulk`
over all the stages, which is the cross-stage optimisation: constants and
unused varyings propagated between stages, varyings packed), then
`tu_shader_create` for each stage, which is the ir3 compile. Because the
stages are optimised against each other before ir3 sees them, the driver
cannot reuse a stage compiled for another partner. This is exactly the
device's "both seen before: 120 to 190 ms".

**A library compiles its own stages, once.** A library is built by the same
function with `builder->state` holding only its part. Its hash is over its
own stages, its layout and its state bits. `tu_link_shaders` then runs over
only the library's stages (one stage for a vertex or a fragment library; the
vertex, tessellation and geometry stages of one library are still linked with
each other), and `tu_shader_create` compiles the ir3 variants **at library
creation**. The compiled `tu_shader` goes into the `VkPipelineCache` under
the library's hash (2054), and `tu_shader` objects are serialised with the
cache (`tu_shader.cc` 3448 on), **so the stored cache holds each library's
binaries** and a later launch gets them back without compiling.

**A fast link compiles nothing.** For a pipeline that brings no stages of its
own and does not ask for link time optimisation, `must_compile` stays false
(1770 to 1778, 1879 to 1893) and the function jumps to `done`: the stages'
`tu_shader` objects are taken from the libraries by reference (2117 to 2131;
`tu_pipeline_builder_parse_libraries`, 2191, copies the state). What is left
is the register state that depends on both stages: the varying routing
(`tu6_emit_vpc`, which calls `ir3_link_shaders` to map the fragment shader's
inputs onto the last vertex-side stage's outputs by slot) and the load state.
No NIR pass and no ir3 compile. Mike Blumenkrantz's measurements of fast
links with `vkoverhead` put Turnip "well under 0.05 ms" a link, the slowest
of the drivers he tried and the only one above his 0.01 ms target
(https://www.supergoodcode.com/fastlink). That was not this phone; the
branch's log line measures it.

**`VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT` recompiles
everything.** With it (1879), each library's kept NIR is cloned into the
pipeline, `must_compile` is set and the whole of `tu_compile_shaders` runs as
for a monolithic pipeline, cross-stage link included. It needs the libraries
to have been made with `RETAIN_LINK_TIME_OPTIMIZATION_INFO` (1761, 2039:
the NIR after linking is kept in a `tu_nir_shaders` cache object), which
costs memory for every library. So on Turnip an "optimised link" is a full
compile by another name and saves nothing over compiling the monolithic
pipeline in the background.

**What a fast link loses at run time.** Only what `nir_opt_varyings_bulk`
does between the vertex side and the fragment shader: an output the fragment
shader never reads is still computed and written by the vertex shader; a
varying that is constant in the vertex shader is not folded into the fragment
shader; varyings are not repacked. Within a stage everything is optimised as
usual. For this emulator the first, and largest, of those is already done by
the translator: the vertex shader's modification carries the mask of
interpolators the pixel shader reads (`SpirvShaderTranslator::Modification`,
`vertex.interpolator_mask`; `gpu/guest_spirv_shader_cache.cc` 60), so the
translated vertex shader does not write what its partner does not read. I
expect the run-time cost to be small; the specification's own warning is "as
bad as a 50% penalty" for drivers in general
(https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_graphics_pipeline_library.html),
and it is **not measured**. The frame time and the GPU's clock in a run with
the setting on against one with it off, both with warm caches, is the check.

**Layouts.** The layout's hash is part of a library's key
(`tu_pipeline_layout_init`, `tu_descriptor_set.cc` 515: every set layout
that is not null, the set count, the push constant size). Turnip does not
look at `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT` at all when
making a layout; a null set layout is simply skipped. This matters here: see
1c, "The layout".

## 1c. How this backend builds pipelines now, and what a split needs

### What there is

- **The key**: `PipelineDescription` (`gpu/vulkan/vulkan_pipeline_cache.h`
  603): the two shaders' hashes and modification words, the render pass key,
  geometry shader kind, tessellation mode and patch, and the state that is
  not dynamic. `ConfigurePipeline` (`vulkan_pipeline_cache.cc` 1220) builds
  it (`GetCurrentStateDescription`, 4303), takes the dynamic values out
  (`FillDynamicState`, 4636) and zeroes what is dynamic
  (`CanonicalizePipelineDescription`, 4552), then looks the slot up in
  `pipelines_`.
- **Dynamic state** (`Initialize`, 563 to 585; listed for the driver at 5803
  to 5865): viewport, scissor, depth bias, blend constants, stencil masks and
  reference always; with `vulkan_dynamic_pipeline_state` and Vulkan 1.3,
  topology, primitive restart, cull mode, front face, depth test, write and
  compare, stencil test and ops; with each feature of
  `VK_EXT_extended_dynamic_state3`, depth clamp enable, polygon mode, colour
  blend enable, blend equation, colour write mask. On Turnip all of them.
- **Creation**: one function, `EnsurePipelineCreated` (5280), with one
  `vkCreateGraphicsPipelines` (5965), called for every kind of pipeline:
  real ones from the creation threads (`CreationThread`, 3770, the call at
  3940) or, when a draw cannot be asynchronous, from the GPU command thread
  (1503, 1544); placeholders (`MakePlaceholder`, 1834; overrides for the
  modules and the layout); shared stand-ins (`Kind::kStandIn`, 3861).
- **The render pass key** (`VulkanRenderTargetCache::RenderPassKey`): formats
  of the bound targets, which are bound, sample count. With
  `vulkan_dynamic_rendering` (on by default, and Turnip has it) the pipeline
  is given the formats in a `VkPipelineRenderingCreateInfo` (5887 to 5923)
  and no render pass; without it, the `VkRenderPass` of the key.
- **Stages**: vertex, or for a tessellated draw a fixed vertex shader, a
  fixed control shader and the translated guest vertex shader as the
  evaluation shader (5367 to 5419); an optional geometry shader for rectangle
  lists, quad lists and point sprites (5442), one module for each
  `GeometryShaderKey`; the pixel shader, or a fixed depth module, or none
  (5455 to 5498).
- **Modifications of translated shaders**: the module is that of a
  `VulkanShader::VulkanTranslation`, a guest shader with a modification word
  (interpolator mask, host vertex shader type, depth mode, the blend factors
  a MIN or MAX blend premultiplies by, and more). One module for each pair of
  shader and modification, made once (`GetOrCreateShaderModule`).
- **Vertex input**: none (5500). Every vertex shader fetches from the shared
  memory buffer itself.
- **The layout**: `GetGuestGraphicsPipelineLayout` (1158) asks the command
  processor for the layout of (pixel textures, pixel samplers, vertex
  textures, vertex samplers) (`VulkanCommandProcessor::GetPipelineLayout`,
  `vulkan_command_processor.cc` 3538). Four sets: shared memory and EDRAM,
  constants, the vertex stage's textures, the pixel stage's textures
  (`SpirvShaderTranslator::DescriptorSet`, `gpu/spirv_shader_translator.h`
  382). So **a layout is a property of the pair of shaders**, by their
  texture counts.
- **The stored record** (`.xpso`, `InitializeShaderStorage`, 6201): one
  `PipelineStoredDescription` for each slot a draw used; every one is made
  again at a title's start (`pipeline_storage_precreate`) through the same
  `EnsurePipelineCreated`.
- **Placeholders and stand-ins**: `MakePlaceholder` makes a slot's
  placeholder from fixed interpreter modules or a translated vertex shader;
  `vulkan_standin_precompile` replaces that by shared stand-in pipelines made
  on the creation threads (`GetSharedStandIn`, 2183). Both are monolithic
  pipelines of fixed modules and go through `EnsurePipelineCreated` with
  overrides.

### Which part of the key belongs to which library

Vulkan's four parts
(https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_graphics_pipeline_library.html):

| Library | Vulkan state | From this backend | Varies with |
|---|---|---|---|
| Vertex input interface | vertex input state, input assembly | no vertex input; topology (dynamic), primitive restart (dynamic), the topology class where the device restricts dynamic topology | nothing on Turnip: one library for the whole run |
| Pre-rasterisation shaders | the vertex-side stages, the layout, rasterisation state, viewport state, tessellation state, the render pass (or, with dynamic rendering, only `viewMask`) | the vertex translation's module (or the three tessellation modules), the geometry shader module, patch control points, `depthBiasEnable` (set when the render pass key has a depth target, 5627), and depth clamp and polygon mode where they are not dynamic | the vertex translation, the geometry shader kind, tessellation, depth target present or not |
| Fragment shader | the fragment stage, the layout, depth and stencil state, multisample state (when sample shading or a render pass is used), the render pass or `viewMask` | the pixel translation's module (or a depth module, or none), the sample count, and depth and stencil state where not dynamic | the pixel translation, the sample count |
| Fragment output interface | colour blend state, multisample state, the render pass or the attachment formats | the render pass key; blend and write masks where not dynamic | the render pass key |

- **Nothing has to become dynamic that is not already.** Every field that
  would otherwise multiply the shader libraries (cull, front face, depth,
  stencil, blend) is dynamic on Turnip today. On a device that lacks a
  feature, that field stays in the key of the library it belongs to, exactly
  as it stays in the pipeline's key now.
- **What is duplicated**: the multisample state must be given identically to
  the fragment shader and the fragment output libraries (the specification's
  valid usage 06633 to 06637), so the sample count is in both keys. The list
  of dynamic states is given whole to every library: the specification says
  dynamic states that are not part of a library's state are ignored
  (`VkGraphicsPipelineCreateInfo`, "dynamic state values ... must be ignored
  if the state they correspond to is not otherwise statically set by one of
  the state subsets used to create the pipeline").
- **One field worth making dynamic later**: `depthBiasEnable`. It is static
  and depends on whether the render pass key has a depth target, so a vertex
  shader drawn with and without one needs two libraries.
  `VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE` is core in 1.3; it needs a change in
  the command processor's dynamic state and is not done here.

### The layout

Without `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT`, every library
and the linked pipeline must have identically defined layouts (valid usage
06612, 06613). The layout here depends on both shaders' texture counts, so a
vertex library would be tied to the texture and sampler counts of the pixel
shader it was first made for, and, on Turnip, compiled again for each other
count (the layout's hash is in the key, 1b). That gives up most of the
sharing on the vertex side.

With the flag, a library's layout names only the sets its own stages use and
leaves the others null. The vertex-side library's layout is then (shared
memory, constants, vertex textures) and does not depend on the pixel shader
at all. The price in the specification: libraries with independent sets are
linked with a layout that is the union of theirs **with the flag** (valid
usage 06730; only a link with link time optimisation may drop the flag,
06729), and two layouts are only compatible for binding descriptor sets if
both or neither has the flag. So the layouts the draws bind their sets with
must have it too. In this backend all of them (the pair layouts, the
placeholders' and the stand-ins') come from the one function,
`VulkanCommandProcessor::GetPipelineLayout`, so one flag there covers every
guest draw. Turnip ignores the flag (1b); what it does to the proprietary
driver is unknown, which is why it has its own setting.

### Render passes against dynamic rendering

- **Dynamic rendering** (`vulkan_dynamic_rendering`, default on): the two
  shader libraries need only `viewMask` (0 here); the formats are given only
  to the fragment output library. So shader libraries are shared across
  render target formats. This is the case that pays.
- **Render passes**: every library but the vertex input one is created with
  the render pass and subpass, and the libraries linked together must have
  compatible render passes (06628). The shader libraries' keys then hold the
  render pass, so a shader drawn to targets of three formats has three
  libraries. It works (the key says so); it shares less. The fragment shader
  interlock path has one render pass for everything and is unaffected.

## 1d. How others use it

- **DXVK** is the reference
  (https://github.com/doitsujin/dxvk/blob/master/src/dxvk/dxvk_graphics.cpp,
  release notes https://github.com/doitsujin/dxvk/releases/tag/v2.0). It
  compiles a library for each shader when the game creates the shader, not
  when it is first drawn. At a draw it links the vertex input library, the
  two shader libraries and the fragment output library into a "base"
  pipeline (`createBasePipeline`) and draws with it at once, and queues the
  optimised pipeline, **a full monolithic compile, not a link with link time
  optimisation** (`createOptimizedPipeline`), at low priority on its worker
  threads; the draw path swaps to it when it is ready. Pipelines that cannot
  use libraries (`canCreateBasePipeline`: sample rate shading without dynamic
  multisample state, some line and polygon modes, dual-source blending, and
  so on) go the old way. What it learned, from the release notes: it needs
  `graphicsPipelineLibraryIndependentInterpolationDecoration`; games that
  create their shaders at load have "prolonged high CPU use" then; games that
  create them at draw time still stutter, less; the driver's on-disk cache
  grows a great deal (it had to tell Nvidia users to raise the limit); with
  libraries its own state cache shrinks to the pipelines that cannot use
  them. It uses independent-set layouts for the libraries.
- **Zink** (Mesa's OpenGL on Vulkan) does the same: a fast-linked pipeline
  for the first draws and an optimised one compiled in the background. Its
  author's guide for drivers (https://www.supergoodcode.com/fastlink) sets
  the bar DXVK and Zink assume: a fast link is cheap enough to do a hundred
  in a frame, and a driver must not compile or copy shader code in one.
- **Emulators**: I found none that ships it. Dolphin has an open request,
  accepted, with no work (https://bugs.dolphin-emu.org/issues/12934); its
  answer to the same problem is ubershaders, which is what this fork's
  interpreter stand-ins are. I did not find it in yuzu, Ryujinx, RPCS3,
  PCSX2, Cemu or xenia; that is a search that found nothing, not a proof.
- **On mobile drivers** I found no written experience at all beyond Turnip's
  own (it runs DXVK through Winlator and the like every day, which is the
  best evidence that its implementation is exercised).

What to take from them: the swap of a fast-linked for an optimised pipeline
is the same hot swap this backend already does for placeholders; the
optimised pipeline is a full compile and so is optional work to be done
late, if at all; libraries belong to shaders and should be made when the
shader is known, not when a pair is drawn; and independent interpolation
decoration is a requirement, not an option.

## 1e. What it would save, from the device's numbers

Assumptions, each of which the device can check with the new log line:

1. A library costs what its stage costs inside a pipeline today, and the two
   stages of a pipeline cost about the same, so a library is about half a
   pipeline. I give the range for the new stage being 30% to 70% of the
   pipeline.
2. A fast link costs well under a millisecond on the phone (Turnip's source
   and the 0.05 ms measurement elsewhere).
3. "Seen before" in the device's line is by translation (shader and
   modification), which is what a library is keyed by. A library also
   depends on the sample count (fragment), on whether there is a depth
   target (vertex side) and, without independent sets, on the partner's
   texture counts; so some "both seen" pipelines will still need a library.
   Not counted below.

| | Banjo-Kazooie | Fable II |
|---|---|---|
| Pipeline compile time measured | 237,085 ms over 782 pipelines | 32,922 ms over 264 |
| "Both seen": become links | 47,745 ms saved | 12,119 ms saved |
| "One of the two": one library instead of a pipeline | 71,569 ms saved (42,941 to 100,197) | 7,907 ms saved (4,744 to 11,069) |
| "Neither": two libraries, the same work | 0 | 0 |
| **Saved** | **about 119 s of 237 s, 50% (38% to 62%)** | **about 20 s of 33 s, 61% (51% to 70%)** |
| Distinct shader libraries, at most | 670 | 211 |

These are thread-seconds on the creation threads, measured under load and
heat, so the percentages are firmer than the seconds.

What the totals hide, and matters more for what the owner sees:

- **A third of new pipelines need no compile at all** (253 of 782 on Banjo,
  101 of 264 on Fable II). If a link is as cheap as the source says, those
  can be linked on the GPU command thread at the draw that first needs them:
  no stand-in, no skipped draw, the right pixels in the first frame. That is
  the part no amount of faster compiling gives.
- For the "one of the two" class the wait for the real pipeline is one
  stage's compile instead of two, and the "neither" class can compile its two
  libraries on two threads at once.

**What it does not save:** the first compile of each shader (the 670 and the
211 above are still compiled, each once); anything on a launch where the
driver's cache already has everything; translation time; and the run-time
cost of the stand-ins while a library compiles.

**How it combines with the stand-in path:** the stand-in covers a draw until
its real pipeline is there; libraries make that time shorter (one stage, or
none). The stand-ins themselves are pipelines of fixed modules and gain
nothing from libraries. The record (`.xpso`) needs no new format: making its
pipelines again at a title's start goes through libraries too, so each
shader in the record is compiled once and every stored combination is
linked; an imported record, or the first launch after a driver update, costs
the 670 instead of the 782 pipelines' worth. Making libraries **before any
draw needs the pair** (as soon as a shader is translated with a
modification, or for the pairs `shader_ahead` sees ahead in the command
stream) is the natural next step and is cheap to get wrong, because a
library made for nothing is one stage, not a pipeline. It is designed below
and not built.

## 1f. The design

### Settings

| Setting | Default | What |
|---|---|---|
| `vulkan_pipeline_libraries` | false | The master switch. Read at device set-up (it decides whether the two extensions are enabled) and by the pipeline cache. Needs a restart. Meant to become the default once the device runs below are good |
| `vulkan_pipeline_libraries_independent_sets` | true | Shader libraries get layouts with only their own stage's sets, and every guest pipeline layout is created with the independent sets flag. Off: libraries are created with the pair's whole layout (valid everywhere the extension is, shares less) |
| `vulkan_pipeline_libraries_inline_link` | false | A new pipeline whose libraries are all ready is linked on the GPU command thread at the draw, within a budget for the frame |
| `vulkan_pipeline_libraries_inline_link_frame_us` | 2000 | That budget: the most time links on the GPU command thread may take in one guest frame; past it, new pipelines go to the creation threads as before |

With `vulkan_pipeline_libraries` off, or on a device without the extension,
the feature or independent interpolation decoration, none of the code below
runs: no extension is enabled, no layout gets a flag, and
`EnsurePipelineCreated` makes its one `vkCreateGraphicsPipelines` call as
before.

### Objects

- One map for each of the four kinds, keyed by a plain struct of exactly
  what the driver is given for that part (table in 1c): module handles, the
  layout handle, the render pass handle or the formats, and the static state
  values. Handles, not guest hashes: a module is made once for each
  translation, so the handle is the translation.
- An entry is made once. A thread that needs one being made by another waits
  for it. A failed entry stays failed (the pipeline falls back).
- Libraries live until the pipeline cache is shut down. Their number is
  bounded by the number of translations times the few values of the static
  leftovers; the log line counts them.
- Flags: `VK_PIPELINE_CREATE_LIBRARY_BIT_KHR` and a
  `VkGraphicsPipelineLibraryCreateInfoEXT` naming the part. **No**
  `RETAIN_LINK_TIME_OPTIMIZATION_INFO`: on Turnip it keeps a copy of every
  shader's NIR for a link that is a full compile anyway (1b).
- All libraries are created against the same `VkPipelineCache` object as
  today's pipelines, so the stored driver cache keeps their binaries.

### Where it sits

In `EnsurePipelineCreated`, at the one `vkCreateGraphicsPipelines` call, for
a real pipeline only (no module or layout override, so never a placeholder
and never a stand-in): the state structures the function has just filled are
split into the four keys, each library is found or made, and the pipeline is
linked from them with no flags (a fast link) and the pair's layout. Whatever
fails (a library, the link) falls back to the monolithic call it replaces,
and is counted by reason. Everything after the call (publishing the handle,
the swap for a placeholder, the logs, the accounting by "shaders seen
before") is unchanged and sees a pipeline like any other.

So every existing caller gets libraries without knowing: the creation
threads, the record's pipelines at a title's start, speculative pipelines,
the synchronous path.

### The draw path

- **Combination ready** (the slot has its pipeline): bind it. Unchanged.
- **Libraries ready, combination new**: with
  `vulkan_pipeline_libraries_inline_link`, `ConfigurePipeline` asks for a
  link only (`PipelineCreationArguments::link_only`: `EnsurePipelineCreated`
  returns without compiling anything if a shader library is missing) when
  both shaders are translated and the frame's budget is not spent. A slot
  linked this way never has a placeholder. The time of every such link is
  measured; the budget is the bound. Without the setting, the job goes to a
  creation thread as today and is a link there.
- **Libraries not ready**: today's path, whichever is set: a stand-in or a
  placeholder, or the draw dropped, while the creation thread makes the
  missing library and links.

### The background optimised pipeline

Not built, and I would not build it before the device says a fast-linked
pipeline costs frame time. On Turnip it is a full monolithic compile for
each pipeline (1b), that is, all the work this branch removes, done later.
On a phone that is heat and battery for a gain that may be a few per cent of
GPU time, on vertex shaders that already do not write what their partner
does not read. If the device shows a cost: compile the monolithic pipeline
only for slots that have drawn in some hundreds of frames, one at a time, at
the lowest priority, only while the frame time is under its target, and swap
it in as a placeholder is swapped now. The swap machinery is there.

### Log lines

- At device set-up, always: the `pipeline libraries:` line of 1a (the
  extension, the feature, the two properties, whether it was enabled;
  `VK_EXT_shader_object` and its feature; the
  `VK_EXT_extended_dynamic_state3` flags, those the fork uses and the five
  more that would matter for a later step).
- Once a second while something moved, with `shader_profiling`, a third
  `placeholder cover:` line:

      placeholder cover: pipeline libraries: on (independent sets yes, fast linking yes); libraries created: vertex input N (ms), vertex side N (ms, slowest), fragment N (ms, slowest), fragment output N (ms); found ready: ...; pipelines linked N (ms, slowest), of those on the GPU command thread N (ms, slowest; refused: a library not ready N, over the frame's budget N, shaders not translated N); pipelines that fell back to a whole compile N (library failed N, link failed N)

- The existing line `pipeline compiles by shaders the driver had compiled
  before` keeps counting linked pipelines, with the whole time including any
  library made for them. **It is the comparison**: with the setting on, the
  "both" column's mean should fall from 120 to 190 ms to about nothing, and
  "one of the two" to about half.

### Without the extension

The start-up line says what is missing and the pipeline cache never takes
the new branch. Nothing else changes.

### Not built (in the order I would do them)

1. Libraries made ahead of need: a creation job of a new kind that makes the
   shader libraries of a translation as soon as it exists (from
   `shader_ahead`'s pairs seen ahead, and for each shader of the record
   before its pipelines), so that the first draw of a pair finds both and is
   an inline link.
2. `depthBiasEnable` dynamic, to halve the vertex-side libraries of shaders
   drawn with and without a depth target.
3. The optimised pipeline in the background, only if measured to matter.
4. Dropping the placeholder for a slot whose libraries will be ready within
   the frame.

## Device test order

To be filled in with the exact lines by the last commit.

## Upstream files changed

To be filled in by the last commit.

## For `boosted/HANDOFF.md` and `boosted/docs/ARCHITECTURE.md` (not edited here)

To be filled in by the last commit.

## What is unverified

Everything about behaviour: see the first paragraph. In particular, from the
research: that the bundled binary matches upstream Turnip's source in this
respect; the cost of a fast link on the phone; the run-time cost of
fast-linked pipelines; anything about the Qualcomm proprietary driver.

## Builds

To be filled in.

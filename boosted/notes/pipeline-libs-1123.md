# Pipeline libraries brought up to date for a phone A/B (branch `claude/pipeline-libs-1123`)

Base: `claude/integration-1116` at `816e62e`, with `claude/pipeline-libraries`
(`8c5a685`) merged in. **Nothing here has been run on the phone.** The design,
the reading of Turnip's source and the device test order of the original work
are in `boosted/notes/pipeline-libraries.md`; this note says what changed for
the merge, what a review against the specification found, whether
`VK_EXT_shader_object` is the better road, and exactly how to run the A/B.

Paths below are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/`.

## Done / in progress / next

Done:

1. The merge. One conflict, in `gpu/vulkan/vulkan_pipeline_cache.h`: the
   stand-in static dynamic state bits (`StandInStaticBits`,
   `standin_static_mask()`, from `claude/standin-device-lost`) and the two
   pipeline library queries sat in the same place. Both are kept. Everything
   else merged by itself; the two branches touch different functions of
   `vulkan_command_processor.cc` (the integration branch: the draw-time
   dynamic state check, `vulkan_dynamic_color_state_fix`; the libraries:
   `GetPipelineLayout`'s flag and library layouts).
2. The review below. No change of behaviour was needed.
3. The start-up line `pipeline libraries: in use yes|no: ...` is now printed
   whatever is set, so both runs of an A/B say which path they took.
4. A cumulative line, `pipeline compile totals:`, printed with
   `shader_profiling` whatever the model, and the existing line by shaders
   seen before now printed with `shader_profiling` whatever the model (it
   needed the placeholder model or the libraries before). Off and on runs now
   have the same lines.
5. `boosted/tools/compile-ab.py`: reads a run's `xe.log` and prints the
   numbers the A/B compares, for the first N seconds.

Next: the phone A/B below. Then, if it is good, the rest of the device test
order in `pipeline-libraries.md` (warm run for the run-time cost; inline
links).

## How the merge meets the stand-in and dynamic state work

- Libraries are made only for real pipelines (`EnsurePipelineCreated`: kind
  `kReal`, no module or layout override). Shared stand-ins, which may bake
  dynamic state in (`vulkan_standin_dynamic_state_mask`), and placeholders
  are made whole as before. A real pipeline keeps every piece of dynamic
  state (`standin_static` is 0 for it), so the dynamic state list each
  library is given is the list the whole pipeline would have had.
- The draw-time check (`vulkan_dynamic_state_check`) reads what the device
  makes dynamic, not the pipeline object, so a linked pipeline is checked
  like a whole one. A linked pipeline's colour blend state has the same
  attachment count as the whole one (the fragment output library is given
  the same `VkPipelineColorBlendStateCreateInfo`).
- Every guest pipeline layout, stand-ins' and placeholders' included, comes
  from `VulkanCommandProcessor::GetPipelineLayout`, so with
  `vulkan_pipeline_libraries_independent_sets` all of them carry the
  independent sets flag and stay compatible with each other for
  `vkCmdBindDescriptorSets` (the layout-change logic that keeps lower sets
  bound across pipelines is unchanged and still right: compatibility needs
  both or neither layout to have the flag, and all have it). The compute
  layouts (swap, FXAA, resolve) bind to the compute point and are not
  affected.

## Review of the GPL path against the specification

Read against the Vulkan specification's rules for
`VK_EXT_graphics_pipeline_library` (the valid usage of
`VkGraphicsPipelineCreateInfo` and `VkPipelineLayoutCreateInfo`), with the
code at `gpu/vulkan/vulkan_pipeline_cache.cc` (`CreatePipelineFromLibraries`,
`GetOrCreatePipelineLibrary`, `TryLinkPipelineInline`) and
`gpu/vulkan/vulkan_command_processor.cc` (`GetPipelineLayout`,
`GetLibraryPipelineLayoutLocked`).

| Point | What the code does | Verdict |
|---|---|---|
| Fast linking | The final pipeline is created with `VkPipelineLibraryCreateInfoKHR` naming the four libraries and no flags: no `LINK_TIME_OPTIMIZATION`, so the driver may not compile. Inline links on the GPU command thread are offered only when the driver reports `graphicsPipelineLibraryFastLinking` | Right. Without the property a link is still valid, only perhaps slow; the creation threads take it then |
| Library flags | Each library: `VK_PIPELINE_CREATE_LIBRARY_BIT_KHR` and a `VkGraphicsPipelineLibraryCreateInfoEXT` with one part. No `RETAIN_LINK_TIME_OPTIMIZATION_INFO` | Right, and the right choice on Turnip: with it every library keeps its NIR for an optimised link that is a full compile there (`pipeline-libraries.md`, 1b). The cost is that an optimised pipeline can never be made from these libraries; it would have to be a whole compile, which is what DXVK does anyway |
| Dynamic state | Every library is given the whole pipeline's `VkPipelineDynamicStateCreateInfo`; the link is given none | Right: a library ignores dynamic states that belong to other parts, and the linked pipeline's dynamic state is the union of its libraries'. Each piece is in exactly one part (topology and restart: vertex input; viewport, scissor, depth bias, cull, front face, depth clamp, polygon mode: pre-rasterisation; depth and stencil tests, ops, masks, reference: fragment shader; blend constants, blend enable, equation, write mask: fragment output), so there is nothing to disagree |
| Multisample state | Given to the fragment shader and fragment output libraries from the same structure | Right: both must be identical when both are given. The sample count and the 2x-as-4x sample mask are in both keys |
| Render pass and formats | Dynamic rendering: the shader libraries get a `VkPipelineRenderingCreateInfo` with only `viewMask` (0); the formats go to the fragment output library alone. Render passes: every library but vertex input gets the render pass and subpass | Right. The view masks agree; with render passes the libraries share the pass of the key |
| Interpolation | Used only with `graphicsPipelineLibraryIndependentInterpolationDecoration` | Right and necessary: the translator does not promise matching decorations between a vertex shader's outputs and a pixel shader's inputs |
| Layouts, independent sets on | Library layouts name only their own stage's texture set, the other set null, with `INDEPENDENT_SETS`; the pair layout the link and the draws use has the flag too; no push constants anywhere | Right: null set layouts need the flag; the link without link time optimisation must use a layout with the flag; the link's layout is the union of the libraries'; push constant ranges are identical (none) |
| Layouts, independent sets off | Libraries are given the pair's whole layout | Right (identically defined layouts), shares less: a vertex library is then tied to its partner's texture counts |
| A missing library layout | `GetLibraryPipelineLayout` returns the whole layout | Valid: the whole layout has the flag too. The error line says so |
| Draw without a pixel shader | A fragment shader library with no stage, holding the depth and stencil state | Allowed by the specification; Turnip has a path for it |
| Pipeline cache | Libraries and links go to the same `VkPipelineCache` as whole pipelines, and creating a shader library marks it dirty so it is saved | Right. On Turnip the stored cache keeps each library's compiled stages, so a second launch finds them. A cold run must delete `<cache>/shaders/local/TITLEID.vk.bin` (the A/B below does) |
| Thread safety | One map of libraries under a mutex; a library being made by another thread is waited for on a condition variable (never with `link_only`); entries are never erased before `Shutdown` | Right. `found ready` counts are approximate when threads link at once, as the original note says |
| Key | Module handles, layout and render pass handles, and every static value given to that part | Right as long as modules live as long as the cache, which they do |

Two things the specification allows that a driver other than Turnip might
handle badly, and which the counters would show (`failed:` per part, `fell
back to a whole compile`): the empty fragment shader library, and the
independent sets flag on every guest layout. Both were in the original note.

**Not checked by running**: everything. In particular that the bundled Turnip
binary draws the same picture from a linked pipeline as from a whole one.

## Is `VK_EXT_shader_object` a better fit on Turnip?

**No, because Turnip does not have it**, and nothing says it is coming. Mesa
`main` as of 2026-11-22 (mirror
https://github.com/sailfishos-mirror/mesa, `docs/features.txt` line 703):
`VK_EXT_shader_object DONE (anv, lvp, hk, nvk, radv)`, no `tu`;
`src/freedreno/vulkan/tu_device.cc` has no mention of it, while it has
`EXT_graphics_pipeline_library = true` and
`graphicsPipelineLibraryFastLinking = true`. The start-up line
`pipeline libraries:` prints `VK_EXT_shader_object offered`, `shaderObject`
for the device, so a run on the phone's own Qualcomm driver answers it there.

The trade-off, for the day a driver offers both:

| | Graphics pipeline libraries | Shader objects |
|---|---|---|
| What is compiled once | A shader, inside a library that also holds that part's static state (sample count, depth target present, render pass without dynamic rendering) | A shader alone; all state is dynamic |
| What a new combination costs | A fast link (Turnip: no compile; the varying routing and register state) | Nothing at bind time in the API; the driver may still compile variants behind the application's back, or pay at draw time |
| State that must be dynamic | Only what would otherwise multiply libraries; this backend already makes it all dynamic on Turnip | Everything, including vertex input, rasterisation samples, sample mask, alpha to coverage, logic op, depth clip: the `VK_EXT_extended_dynamic_state3` features this backend does not use yet, and `VK_EXT_vertex_input_dynamic_state` |
| Cross-stage optimisation | Lost in a fast link (an optimised whole pipeline can be compiled later) | Lost, unless shaders are created linked (`VK_SHADER_CREATE_LINK_STAGE_BIT_EXT`), which brings back per-pair compiles |
| Changes here | Done: inside `EnsurePipelineCreated`, nothing else sees it; the stand-ins, the record and the slots are unchanged | A second draw path: no `VkPipeline` per slot, binding of shaders and every piece of state per draw, the placeholder swap and the record rethought |
| Driver support on Android | Turnip yes; about 19% of Android devices report it (gpuinfo) | Turnip no; Qualcomm's unknown |

So libraries are the road on the bundled driver, and the only one; shader
objects would be a rewrite of the draw path for a driver that does not
exist on this phone. Not implemented.

## What the log says, and what the A/B compares

All with `shader_profiling = true` (it is in the base config).

- At start-up, twice in a row near the top:
  - `pipeline libraries: VK_EXT_graphics_pipeline_library offered yes
    (VK_KHR_pipeline_library yes), graphicsPipelineLibrary yes, fast linking
    yes, independent interpolation decoration yes; enabled on this device
    yes|no (vulkan_pipeline_libraries yes|no); VK_EXT_shader_object offered
    ...` (the device, `ui/vulkan/vulkan_device.cc`). Expected on the bundled
    Turnip: the first five `yes`, shader object `no`.
  - `pipeline libraries: in use yes|no: real pipelines are linked from
    libraries | built whole ...` (the pipeline cache). The off run must say
    `no`, the on run `yes`. If the on run says no, stop: the device line says
    what is missing.
- Once a second while something moved:
  - `first run split: T ms, N frame(s) (longest L ms), ...`: frames and the
    worst frame of that second.
  - `pipeline compile totals: real N in T ms (mean M, over 100 ms K, slowest
    S; linked from libraries N in T ms), placeholders and stand-ins N in T
    ms (over 100 ms K, slowest S); pipeline libraries on|off`: since start.
    A real pipeline's time includes making any library it needed.
  - `pipeline compiles by shaders the driver had compiled before: neither
    ..., one of the two ..., both ...`: since start.
  - With libraries on: `placeholder cover: pipeline libraries: on (...);
    libraries created: ...; pipelines linked N (...); ... pipelines that fell
    back to a whole compile N (...)`.
- `boosted/tools/compile-ab.py <xe.log> <seconds> [<xe.log> ...]` prints, for
  the first so many seconds from the first draw: the path, frames, the worst
  frame, the seconds with a frame over 100 ms and over 250 ms, the last
  totals lines in the window and any library failure, `Failed to create` or
  device loss.

## The phone A/B

Do not run it while another batch is on the phone.

**Configs** (whole files, outside the repository because they carry the
profile): `C:\Users\ctyld\Desktop\HakuX\device-logs\run148\cfg\pl-off.toml`
and `pl-on.toml`. Both are `device-logs/run148/loads-A-2/config.toml` (the
defaults run A of 2026-11-23: build 149's defaults, `shader_profiling = true`,
the placeholder model, the bundled Turnip) with three lines added at the top
of `[Vulkan]`; they differ in the first only:

    vulkan_pipeline_libraries = false        (pl-off)   / true (pl-on)
    vulkan_pipeline_libraries_independent_sets = true
    vulkan_pipeline_libraries_inline_link = false

MD5: `pl-off.toml` c41efd0aa36de9082c90bd5718216a71, `pl-on.toml`
4b6d5bb25a4385d2e6b27c4b63ce8c78. If they are lost, make them again from that
base with those lines.

**Build**: the APK of this branch's CI run (below). Install it, and nothing
else changes between the four runs.

**ISO names**: `adb shell ls /storage/emulated/0/Roms/` and take the Banjo-
Kazooie: Nuts & Bolts and Fable II files (title IDs 4D5307ED and 4D5307F1).

**Runs**, in this order, each cold (`run-one.sh ... 1` deletes the title's
record, driver cache and stand-in cache), with `COOL=35` so each starts at the
same battery temperature. Run longer than the window, because the window is
counted from the first draw, not from launch:

    COOL=35 tools/run-one.sh pl-banjo-off <...>/cfg/pl-off.toml 4D5307ED "/storage/emulated/0/Roms/<banjo>.iso" 90 1
    COOL=35 tools/run-one.sh pl-banjo-on  <...>/cfg/pl-on.toml  4D5307ED "/storage/emulated/0/Roms/<banjo>.iso" 90 1
    COOL=35 tools/run-one.sh pl-fable-off <...>/cfg/pl-off.toml 4D5307F1 "/storage/emulated/0/Roms/<fable>.iso" 150 1
    COOL=35 tools/run-one.sh pl-fable-on  <...>/cfg/pl-on.toml  4D5307F1 "/storage/emulated/0/Roms/<fable>.iso" 150 1

Hands-off: no keys, so both runs of a title see the same screens (title
screens and attract mode). If a title needs keys to reach anything drawn,
give the same `KEYS` to both of its runs.

**Read**:

    python boosted/tools/compile-ab.py device-logs/run148/pl-banjo-off/xe.log 60 device-logs/run148/pl-banjo-on/xe.log
    python boosted/tools/compile-ab.py device-logs/run148/pl-fable-off/xe.log 120 device-logs/run148/pl-fable-on/xe.log

and compare, off against on:

1. `pipeline libraries in use`: no, then yes. Errors: 0 in both. Any
   `pipeline libraries: failed`, `fell back to a whole compile` above 0, or a
   device loss in the on run is a finding to write down before anything else.
2. `pipeline compile totals:` real pipelines' total ms, `over 100 ms`, and
   `slowest`: the claim to test is that these fall (the estimate in
   `pipeline-libraries.md` 1e: about half of the compile time). Compare the
   count too: the same screens should make about the same number of real
   pipelines; if the counts differ a lot, the runs did not see the same
   things and the totals are not comparable.
3. `pipeline compiles by shaders the driver had compiled before`: in the on
   run, the `both` mean should be near 0 (a link) and `one of the two` about
   half its off value.
4. Frames: the worst frame and the seconds with a frame over 100 ms and over
   250 ms. This is what the owner sees. Compile time moves off the creation
   threads' clock, not necessarily off the frame; if totals fall and frames
   do not, the stand-ins or waits are what holds frames, not compiles.
5. The picture at `end.png`: the same scene drawn the same in both. A
   difference is a fault of the split or the driver's link.
6. From the on run's `placeholder cover: pipeline libraries:` line: the mean
   cost of a vertex side and a fragment library, and of a link (`pipelines
   linked N (... mean ..., slowest ...)`); a link's slowest above 1 ms would
   argue against `vulkan_pipeline_libraries_inline_link` later.

Nothing is claimed until these four runs are in. One cold run each is noisy
(heat, what the title showed); if the difference is under about 20% in
totals, repeat each pair once before drawing a conclusion.

## Upstream files changed in this branch's own commit

| File | Why |
|---|---|
| `gpu/vulkan/vulkan_pipeline_cache.h` | The totals since start for `pipeline compile totals:` |
| `gpu/vulkan/vulkan_pipeline_cache.cc` | The `pipeline libraries: in use` line printed always; the totals counted in `EnsurePipelineCreated` (only with `shader_profiling`); `LogStatsIfDue` prints the totals line and the line by shaders seen before with `shader_profiling` whatever the model |

The merge's own upstream files are the rows added to
`boosted/docs/ARCHITECTURE.md` for pipeline-libraries.

## Builds

Run 150 (Actions id 45234728326, `df3e94d`, 2026-11-23): "Xbox core (hakuX)" green (3 m 49 s), "APK (both cores)" green (22 m 48 s), artifact `hakuX-Boosted-debug-apk` (34.5 MB). "Translator tests" (the CPU side, untouched here) still running when this was written. This is the APK for the A/B.

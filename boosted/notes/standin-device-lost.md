# Device lost with shared stand-ins (branch `claude/standin-device-lost`)

Base: `claude/integration-1116` at `7577c87`. **Nothing here has been built or
run.** Every statement about behaviour is a reading of the code until the
phone runs it, and nothing here is a fix until it has. Paths are under
`x360/emulator-core/src/main/cpp/xenia/src/xenia/gpu/vulkan/` unless said
otherwise.

## Done / in progress / next

Done:

1. Read the run 146 logs (`device-logs/run146/xe-C.log`, `xe-D.log`,
   `logcat-C.txt`) and the shared stand-in path (below, "What the logs say"
   and "What the code does").
2. Two bisection settings for the shared stand-ins:
   `vulkan_standin_dynamic_state_mask`, `vulkan_standin_shared_forms`
   (commit `d29cd5e`).
3. A draw time check, `vulkan_dynamic_state_check`, and a fix as a setting,
   `vulkan_dynamic_color_state_fix`, for a gap in the colour blend dynamic
   state tracking that applies to every pipeline (commit `7cc75e6`).

In progress: nothing.

Next: the phone runs under "Phone runs, fewest first". Build first (Actions);
nothing here has been compiled.

## What the logs say

- `xe-C.log` (precompile on): `VK_ERROR_DEVICE_LOST` from `vkWaitForFences`
  on the paint fence of submission 482; the last completed submission is
  481, so the GPU stopped in 482 (around frame 470 to 471; submission 490 is
  frame 474). The submission list at the end (490 to 505) is what was
  recorded after, not what hung.
- Shared stand-ins were drawn without harm well before: at frame 1 (the
  same pair, VS `1E6883FCCDE1F688` / PS `A4A965C189287B99`, pixel
  interpreter, right after the title made its 4x MSAA targets) and at frame
  4 (a depth-only stand-in). So a shared stand-in pipeline is not fatal by
  itself; something of frames 468 to 471 is.
- At frame 468 the title loads (decommitted pages at 460), and in four
  frames: 13 first draws with shared stand-ins (pixel interpreter and
  no-op), 32 draws skipped "while a stand-in was being made", stand-ins for
  new render passes (4x MSAA colour and depth `0x38E`, 1x colour `0x208`),
  and real pipelines replacing stand-ins. No 2D, flat or cube fallback
  stand-in was drawn (all "first draw" lines say interpreter or no-op).
- `xe-D.log` (same build, precompile off, same title, cold cache) went
  through the same scene at frame 473 with the same pairs on per-slot
  placeholders (pixel interpreter, 4x MSAA, depth-only) and ran on for 9
  minutes. D skipped no draws before frame 1624; C skipped 24 at frame 1
  and 32 at 468 to 473.
- `logcat-C.txt`: no kernel or driver line before the abort (the log has
  no kgsl lines at all); HWUI's "Timeout (500ms) exceeded waiting for buffer
  fence" 1.6 s before it is the hang seen from the compositor.
- The device's dynamic state (the `placeholder cover: stand-ins:` line):
  extended, depth clamp, polygon mode, blend enable, blend equation, colour
  write mask and any topology all true. `turnip_debug = 'sysmem'` was set.

## What the code does (by reading)

- **The pipelines.** A shared stand-in is built by the same
  `EnsurePipelineCreated` from its recipe's description, which is the
  slot's canonical description with the shader hashes cut and the
  modifications cut to the host vertex shader type and the two premultiply
  factors. Every field `EnsurePipelineCreated` reads is kept (topology,
  restart, rasterisation, render pass key, depth and stencil, blend
  attachments, tessellation), so the shared pipeline for a slot is the same
  pipeline the per-slot path builds for it: same modules (interpreter
  vertex module, `ucode_program_ps_`, the same geometry shader key), same
  layout object for the pixel interpreter (`GetPipelineLayout(28, 14, 0,
  0)` both ways), same render pass key, same multisample state, same list
  of dynamic states. I found no difference that the GPU could see, apart
  from the layout of the no-op and float24 forms (below).
- **The dynamic state list** is the same for every guest pipeline, real or
  stand-in (`EnsurePipelineCreated`, the `dynamic_states` block), and the
  draw sets it from the slot's own `DynamicState`, worked out from the
  draw's non-canonical description in `ConfigurePipeline`, for stand-in and
  real draws alike (`UpdateDynamicState`). So "a dynamic state the
  stand-in declares but the draw never sets" has no stand-in specific path
  that I could find. The new check settles that on the phone.
- **One general gap (not specific to stand-ins), now behind a setting.**
  `UpdateDynamicState` sets colour blend enable, blend equation and write
  mask only for attachments `[0, highest used]` of the current draw, and
  decides whether to set them by comparing with remembered values. After a
  new command buffer, or an external pipeline bind, the flag forces one
  emission, but only for the current draw's count; a later draw to more
  attachments compares its higher attachments with values remembered from
  an older command buffer and, if they match, leaves them unset in this one
  (undefined by the spec). Fable's passes in the hang have one colour
  attachment, so this is unlikely to be the cause here.
  `vulkan_dynamic_color_state_fix` sets all four every time.
- **The layout of the no-op and float24 forms.** A shared depth-only
  stand-in has the minimal layout of its own (`GetPipelineLayout(0, 0, 0,
  0)`), and its draws bind descriptor sets with it (`IssueDraw`, the
  `placeholder_shared` test). The per-slot path used the slot's layout. The
  interpreter's vertex stage reads no texture, so both are valid; low
  suspicion.
- **Render pass compatibility.** The recipe's render pass key is the slot's
  normalised key (only the load operation bits are cleared, which do not
  count for compatibility), and with dynamic rendering the pipeline's
  attachment formats come from that key, as for real pipelines. The new
  check compares it with the pass each shared stand-in is drawn in.
- **Lifetime.** A shared handle is never queued for destruction when a real
  pipeline replaces it in a slot (the `placeholder_shared` test in
  `EnsurePipelineCreated`), and the race between adoption and the real
  pipeline's publication resolves either way without a destroy. No
  use-after-free found.
- **Skipped draws.** A draw whose stand-in is still compiling returns from
  `IssueDraw` after `render_target_cache_->Update`, which may have queued
  in-pass transfers and set "don't care" loads for the coming pass. The next
  `Update` performs them outside a pass (`FlushPendingDrawPassTransfers`).
  That path is old, but in this scene only the precompile run took it (D
  skipped nothing until frame 1624). Not shown to be wrong; a candidate by
  timing alone.
- **Turnip (Mesa 27.0.0-devel, git 2648b2059f on the phone).** From the
  copy of `tu_pipeline.cc` in this session's scratch folder (same major
  version, exact commit unknown): Turnip groups state into draw states and
  computes a group at draw time when any member is dynamic, from the
  command buffer's copy of the dynamic state; unset dynamic state there is
  stale values, not garbage pointers. From memory, not checked against the
  phone's build: missing dynamic state on Turnip tends to give wrong pixels
  rather than a hang. That lowers the dynamic state suspect, but does not
  rule it out (a stale value for a state that sizes something, such as the
  attachment count of the blend group, could).

## Ranked causes

1. **Something only the precompile run did in frames 468 to 471.** The D
   run is the control: same build, scene and pairs, no loss. Within that:
   - (a) the 32 skipped draws and the in-pass transfers they leave behind
     (only C skipped in this scene; frame 1's 24 skips did not hang, which
     weakens it);
   - (b) sharing one pipeline object across slots, or making it on a
     creation thread (nothing found by reading; Turnip behaviour assumed);
   - (c) dynamic state left unset at a stand-in draw (the brief's suspect;
     no stand-in specific gap found by reading, so ranked below the two
     above; the check answers it in one run);
   - (d) the other things the setting turns on (the interpreter for already
     translated vertex shaders, no placeholder from a translated vertex
     shader, depth-only draws always asynchronous): D already had
     `vulkan_placeholder_interpreter_translated_vs = true` and
     `vulkan_placeholder_depth_only_async = true`, so only the second is new.
2. **A render pass mismatch for a shared stand-in.** Not possible by
   reading; the check confirms.
3. **The colour attachment gap.** Real but general, and not exercised by
   one-attachment passes.

## Settings (all new, all default to the old behaviour)

| Setting | Default | What it does |
|---|---|---|
| `vulkan_dynamic_state_check` | 0 | 1: at each draw with a shared stand-in, 2: at every guest draw, check that every dynamic state the bound pipeline declares has been set since the command buffer began or since a pipeline with it static was bound (external pipelines make the extended state undefined; a stand-in with baked state makes that state undefined), and that a shared stand-in is drawn in the render pass it was made for. Logs the first 16 faults (`dynamic state check: frame F, submission S: VS, PS, pipeline P (a shared stand-in / the slot's own placeholder / the real pipeline), render pass K: not set ...: names`) and every 600 frames `dynamic state check: N draws checked, M with a fault` |
| `vulkan_standin_dynamic_state_mask` | 0 | Shared stand-ins bake in these pieces from the draw's own values (more distinct stand-ins), and their draws do not set them: 1 depth clamp, 2 polygon mode, 4 blend enable, 8 blend equation, 16 write mask, 32 topology and primitive restart, 64 cull, front face, depth test, write, compare, stencil test and ops. 127 all. Cut to what the device makes dynamic. Real pipelines unchanged |
| `vulkan_standin_shared_forms` | -1 | Which pixel forms use a shared stand-in; the others get a per-slot placeholder as without precompile (inline on the GPU command thread with `vulkan_placeholder_async = false`). 1 no-op, 2 debug colour, 4 float24, 8 pixel interpreter, 16 flat, 32 2D, 64 cube |
| `vulkan_dynamic_color_state_fix` | false | Colour blend enable, equation and write mask set for all four attachments |

With either stand-in setting off its default, the second `placeholder
cover: stand-ins:` line ends with `; baked into the stand-ins 0x..,
shared forms 0x..`.

## Phone runs, fewest first

Every run: run 146's config C (`device-logs/run146/config-C.toml`:
`vulkan_standin_precompile = true` and the rest), Fable II, **cold cache**
(clear the title's shader cache and its `.standin.vk.bin` record as for
run C), play to the same load (it hung at frame 480, about 54 s in), with
`shader_profiling = true` as in C. Pull `xe.log` and `logcat`.

1. **The check.** Add `vulkan_dynamic_state_check = 2`.
   - Look for `dynamic state check: frame` lines. One that names "a shared
     stand-in" just before the loss, or any "the stand-in was made for",
     is the cause: report it and stop.
   - Lines that name only "the real pipeline" and colour attachments 1 to
     3 are the general gap: note them, and repeat this run later with
     `vulkan_dynamic_color_state_fix = true` to see them go.
   - No line and the loss again: dynamic state as tracked here is ruled
     out; go to 2.
2. **No skipped draws.** Config C plus `vulkan_placeholder_async_wait_ms =
   1000` (a draw waits up to a second a frame for its stand-in instead of
   being skipped; frames will hitch at the load; only a slot's first draw
   waits, so a few later draws may still be skipped). Check that `draws
   skipped while a stand-in was being made` in the stand-ins line is 0 or
   near it, against 32 in run C. No loss: the
   skip path is the cause (then the work is in `IssueDraw`'s skip return
   after `render_target_cache_->Update`). Loss: go to 3.
3. **Baked state.** Config C plus `vulkan_standin_dynamic_state_mask =
   127`. No loss: one of the dynamic pieces; halve it in the next runs, 28
   (the colour three) then 3 or 96, to name it. Loss: go to 4.
4. **No sharing.** Config C plus `vulkan_standin_shared_forms = 0` (every
   placeholder per slot, as in run D, with the rest of precompile on). No
   loss: sharing a pipeline object or making it on a creation thread; then
   `8` (only the pixel interpreter shared) and `1` (only no-op) to name the
   form. Loss: the cause is not the shared pipelines but what precompile
   turns on besides (first suspect: no placeholder from a translated vertex
   shader).

Optional, config only, at any point (TU_DEBUG flag names from memory, not
checked against the phone's Turnip): `turnip_debug = 'sysmem,nolrz'`
(rules out low-resolution depth), `turnip_debug = 'sysmem,syncdraw'`
(waits for idle after each draw: a loss that goes away is a missing
barrier or a race, not state).

## What I could not verify

Everything about behaviour. That the code compiles (no compiler here). That
the check's model of "undefined" matches Turnip (it follows the Vulkan rule:
binding a pipeline with a state static makes that dynamic state undefined,
and setting a state that the bound pipeline has static is not allowed before
the draw). That Turnip's handling is as described above (read from a source
copy whose commit is not the phone's, and from memory). That baking state
into the stand-ins does not itself fail pipeline creation for some recipe
(a failed one is counted in `failed` of the stand-ins line, and its slots
skip draws).

## Upstream files changed (each change marked `hakuX-Boosted`)

- `gpu/vulkan/vulkan_pipeline_cache.h`: `StandInStaticBits`,
  `standin_static_mask()`, `Pipeline::placeholder_render_pass_key`, the
  source description members, `BuildStandInRecipe`'s new argument.
- `gpu/vulkan/vulkan_pipeline_cache.cc`: the two stand-in settings;
  `Initialize` (the mask); `ConfigurePipeline` (keeps the non-canonical
  description when the mask is set); `MakePlaceholder` (the form test);
  `BuildStandInRecipe` (puts back baked pieces); `TryAdoptSharedStandIn`
  (the render pass key); `EnsurePipelineCreated` (the dynamic state list of
  a stand-in); `LogStatsIfDue` (the suffix).
- `gpu/vulkan/vulkan_command_processor.h`: `UpdateDynamicState`'s new
  argument, the check's members and functions.
- `gpu/vulkan/vulkan_command_processor.cc`: the check and colour settings;
  `BindExternalGraphicsPipeline`, `SetViewport`, `SetScissor`,
  `BeginSubmission`, `UpdateDynamicState` (check bits, baked pieces, the
  colour fix); `IssueDraw` (the mask and the check); the check's functions.

## For `boosted/HANDOFF.md` (not edited here)

Under "Right now", after the run 146 device runs:

> - **`claude/standin-device-lost`** (from `claude/integration-1116` at
>   `7577c87`, not built, not run): the device loss with
>   `vulkan_standin_precompile` not found by reading; a shared stand-in is
>   the same pipeline the per-slot path builds, and its dynamic state is set
>   as for real pipelines. Settings to find it on the phone, all off:
>   `vulkan_dynamic_state_check` (1 or 2), `vulkan_standin_dynamic_state_mask`,
>   `vulkan_standin_shared_forms`, and `vulkan_dynamic_color_state_fix` for a
>   general gap in the colour blend state tracking. Phone order (each on
>   config C, Fable II, cold cache): check = 2; `vulkan_placeholder_async_wait_ms
>   = 1000`; mask = 127; shared forms = 0. Notes:
>   `boosted/notes/standin-device-lost.md`.

## For `boosted/docs/ARCHITECTURE.md` (not edited here)

Rows for the upstream files list above, with "standin-device-lost:
bisection settings and a dynamic state check for shared stand-ins".

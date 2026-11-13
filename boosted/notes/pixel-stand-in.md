# Pixel stand-in (branch `claude/pixel-stand-in`)

Base: `7648e9c` (built green as run 44). Nothing here has been run on a
device. The only check is a green GitHub Actions build.

## Status

Done:
- Read the placeholder model end to end (findings below).
- Stage 1, the stand-in pixel shader: `3810bd9`, built green as run 60
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43393570519).
  Settings `vulkan_placeholder_ps_mode` (0 as now, 1 approximation with
  alpha 1, 2 approximation with alpha and the guest alpha test),
  `vulkan_placeholder_real_vs` (true as now),
  `vulkan_placeholder_interpreter_translated_vs` (false as now).

In progress:
- Stage 2: placeholders for draws the interpreter does not cover today
  (draws without a pixel shader, rectangle and quad lists), each behind its
  own setting, and the list of what still gets none.

Next:
- Stage 3: design notes for a pixel ucode interpreter.
- The full write-up of this file (settings to flip, what to read in
  `xe.log`, upstream files changed, what the build cannot show).

## Findings from reading the code (before any change)

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/gpu/`.

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
   waiting for pipelines" in the backlog. Not changed here (it is the main
   branch's model, not this task). A fix would record the slot pointer
   again for draws whose layout was complete when recorded, so the replay
   can use a pipeline that appeared during the wait.
2. **The interpreter vertex shader outputs no interpolators.**
   `shaders/ucode_interpreter.vs.glsl` computes `xe_out_interp[16]` and
   writes only `gl_Position`. Any pixel stand-in that needs a texture
   coordinate or a vertex colour needs a variant of it that outputs them.
3. **A placeholder with the real vertex shader is compiled on the draw
   thread.** `ConfigurePipeline` calls `EnsurePipelineCreatedWithPlaceholder`
   inline when the vertex shader is already translated, which is a driver
   compile of the real vertex shader (with the no-op pixel shader) on the
   GPU command thread. Only the interpreter placeholder is cheap, and only
   because every such pipeline shares one vertex and one pixel module, so
   the driver's cache can answer. The stand-in is designed to keep that
   property.
4. In the placeholder model `vulkan_async_skip_draws = false` makes
   `EndSubmission` block for every queued pipeline although the draws were
   already recorded with their placeholders: a stall with no gain. Test the
   placeholder model with `vulkan_async_skip_draws = true` (the app's
   default).

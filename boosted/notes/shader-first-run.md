# Shader first run (branch `claude/shader-first-run`)

Base: `claude/integration-1113` at `8e03766`. **Nothing here has been run on
a device.** Work in progress: this file is rewritten as each step lands.

## Status

Done:
- The guest shader interpreter, version 2, as shaders and a program builder
  (`gpu/shaders/ucode_program*.glsl*`, `gpu/ucode_program.{h,cc}`).
- A rig that checks it against the translator on a software Vulkan device
  (`gpu/first_run_rig/`, README there): 367 vertex test shaders, none
  differs.
- A translator bug the rig found, fixed: `cube` written to `.x` or `.w`
  without `.y` produced a module with a missing id
  (`spirv_shader_translator_alu.cc`).
- The interpreter in the draw path, as a vertex shader, behind
  `vulkan_placeholder_interpreter = 2` (default 1, today's behaviour). Written
  by the 02:30 resume check, see "The interpreter in the draw path" and the
  section at the end of this file. Compiles; never drawn anything anywhere:
  the rig does not run this code and no device has.

- The interpreter in the draw path of a tessellated draw, as its tessellation
  evaluation shader, behind `vulkan_placeholder_interpreter_tessellation`
  (default false, and only with `vulkan_placeholder_interpreter = 2`). Written
  by the 04:30 resume check, see "Tessellated draws" and the section at the
  end of this file. Compiles; never drawn anything anywhere: the rig does not
  draw the tessellation forms and no device has.

In progress: nothing.

Next (none of it started): the point sprite form in the draw path (the module
exists, nothing loads it; it is only for `vulkan_avoid_geometry_shaders`, and
a point list with a geometry shader needs the point size as an output the
interpreter's vertex shader does not have yet); vertex shaders that fetch
textures (the interpreter gives such a fetch 0, so the draw's shape may be
wrong: it needs either real sampling in the vertex stages or a setting that
says wrong shape is better than no draw); the frame-time split log; the pixel
side of the interpreter; less compile work; the kept record. See the task in
`boosted/notes/OVERNIGHT-2.md`.

## Settings

| Setting | Group | Default | What |
|---|---|---|---|
| `vulkan_placeholder_interpreter` | Vulkan | 1 | New. 1: XenDroid's interpreter, as before. 2: version 2 draws the vertices of a placeholder. Read once at start-up, so it needs a restart. Does nothing without `vulkan_placeholder_pipelines` and `async_shader_vs_interpreter` |
| `vulkan_placeholder_interpreter_tessellation` | Vulkan | false | New. With `vulkan_placeholder_interpreter = 2`: a tessellated draw can have an interpreter placeholder. Read once at start-up. Does nothing with version 1 |

## The interpreter in the draw path

What `vulkan_placeholder_interpreter = 2` changes, and only with it:

- A draw's vertex shader can have an interpreter placeholder when it was
  loaded inline (`IM_LOAD_IMMEDIATE`) and when it has loops. Of the 604 draws
  skipped in Fable II on 2026-11-13 those were 11 and 4 of the shader pairs
  named as reasons. Still refused, as with version 1: tessellated draws,
  primitives expanded in the vertex shader, point lists, vertex shaders that
  fetch textures, call subroutines or export to memory. New refusal: microcode
  that is malformed or longer than 1,007 control flow and other instructions
  together (log reason `the vertex shader's microcode is malformed or longer
  than the interpreter's buffer`).
- The placeholder's vertex shader module is `ucode_program_vs`, with the
  no-op pixel shader and with a stand-in alike.
- The draw is fed the prepared microcode (`VulkanShader::GetUcodeProgram`,
  built once for a shader) behind the bool and loop constants: that buffer
  (set 1, binding 3) is 16,384 bytes for such a draw and 160 for any other,
  and is written again when the program, or the kill rule, changes.
- A stand-in pixel shader reads its two interpolators at their own locations
  (a copy of the module for each pair of locations, as with a translated
  vertex shader), and which texture coordinate it uses is decided when the
  placeholder is made, not for each draw. So placeholders of version 2 do not
  all share one pair of modules as version 1's do.
- The geometry shader of a rectangle or quad list placeholder
  (`vulkan_placeholder_interpreter_geometry`) passes on the interpolators up
  to the last one the stand-in reads.

One change that is not behind the setting: the second `UpdateBindings` of a
draw (after render target transfers inside the draw's pass) now passes the
stand-in's texture as the first does. It was left out, so a stand-in with a
texture had its descriptor set written without it on that path. It only
matters with `vulkan_placeholder_ps_mode` above 0.

Log lines to compare on the device (with `shader_profiling = true`):

- at start-up, `placeholders use the guest shader interpreter, version 2`
  with the module's size, or `Failed to create the vertex shader of the guest
  shader interpreter, version 2 - version 1 is used`;
- `shader_profiling: placeholder detail: ... interpreter 2 vertex shader, N
  bytes of SPIR-V` and the time on the line before it: what one placeholder
  of version 2 costs the driver, against version 1's 51 KB module (1.6 ms
  each in Banjo, worst 257 ms in Fable II);
- the skipped draw summary and its reasons: `loaded inline` and `has loops`
  should be gone, `malformed or longer` should be rare or absent.

What must be looked at on the device before it is trusted, in this order:

1. With `vulkan_placeholder_interpreter = 1` or unset: nothing changes (the
   merge check).
2. With 2 and `vulkan_placeholder_ps_mode = 0`: geometry appears where it did
   with version 1 (the no-op pixel shader writes depth only, so look for
   missing or misplaced occluders, and for a device loss or a validation
   error at the first placeholder draw).
3. With 2 and the stand-in on: textures on the placeholders are the right
   ones and not swimming (a wrong interpolator location would show as that).

## Tessellated draws

What `vulkan_placeholder_interpreter_tessellation = true` changes, and only
with `vulkan_placeholder_interpreter = 2`:

- At start-up eight more modules are made: the interpreter as the
  tessellation evaluation shader of triangle and of quad patches, with
  indexed control points and indexed by patch (the four
  `ucode_program_*.ds.glsl`), each as compiled (equal spacing, for the
  discrete mode) and as a copy whose spacing execution mode is changed to
  fractional even (the continuous and adaptive modes). The translator picks
  domain, spacing and winding for the translated shader by the same two
  fields of the vertex shader modification (`host_vertex_shader_type`,
  `tessellation_mode`).
- A tessellated draw (host vertex shader type a triangle or quad domain) is
  no longer refused by the interpreter for being tessellated. The other
  refusals stand (vertex texture fetch, subroutine calls, memory export, a
  program that does not fit). Its placeholder is the host's own tessellation
  vertex and control shaders, as in the real pipeline, with the interpreter's
  module where the translated guest vertex shader would be.
- Of the 604 draws skipped in Fable II on 2026-11-13, "tessellated" was the
  reason named for 10 shader pairs. How many of those are also refused for
  another reason is not known.
- Line domains have no module and stay refused.

Log lines to compare on the device:

- at start-up, `tessellated draws can have a placeholder of the guest shader
  interpreter, version 2 ... N of 8 tessellation evaluation modules made`. N
  should be 8. Under 8 means a module was refused by the driver or (4 exactly)
  the spacing execution mode was not found in the compiled modules, and the
  draws of that domain and spacing stay refused;
- `shader_profiling: placeholder detail: ... interpreter 2 tessellation
  evaluation, as the vertex shader, N bytes of SPIR-V` with the time on the
  line before it;
- in the skipped draw summary, `interpreter refused: the draw is tessellated`
  should be gone for triangle and quad patches.

What must be looked at on the device before it is trusted: with the setting
off nothing changes (the merge check; the only shared code is which module a
placeholder's vertex stage gets, and for a draw that is not tessellated that
is the module it got before). With it on, in a place with tessellated
geometry (which Fable II has; where is not recorded): the surface appears
where the real pipeline then draws it, facing the right way (a wrong winding
would show as the surface missing until the real pipeline arrives, which is
no worse than today), with no crack pattern that changes when the real
pipeline takes over (a wrong spacing would show as that), and no device loss
or validation error at the first such draw.

What the build cannot say about it: anything. The rig checks the
interpreter's core against the translator through the plain vertex shader
form only; the four tessellation forms share that core and differ in how
registers 0 and 1 are loaded, which was written to follow
`SpirvShaderTranslator` and has been compared with it by reading, not by
drawing. Extending the rig to draw patches is the way to check it off the
device.

What the build cannot say, besides everything about behaviour: whether the
Adreno driver accepts and how fast it compiles the module (the rig ran it on
lavapipe), and whether a 16 KB uniform range for each program change costs
anything that shows.

Upstream files changed (all XenDroid's, all marked `hakuX-Boosted`):

- `gpu/spirv_shader_translator_alu.cc`: the `cube` fix above.
- `gpu/vulkan/vulkan_shader.{h,cc}`: `GetUcodeProgram`.
- `gpu/vulkan/vulkan_pipeline_cache.{h,cc}`: the settings, the modules, the
  choice of module and of the stand-in's locations, the new skip reason; in
  `EnsurePipelineCreated` the tessellation evaluation stage takes the
  placeholder's module when there is one (it took the translated shader's
  always, which no placeholder reached before).
- `gpu/vulkan/vulkan_command_processor.{h,cc}`: which draws the interpreter
  may stand in for (tessellated ones included, with the setting), the program
  and its per-draw word in the bool and loop buffer, the second
  `UpdateBindings`.

## Resume check, 02:30 (2026-11-14)

The branch's last commit was at 23:15 with "putting the interpreter into the
draw path behind a setting" in progress and nothing of it committed, so its
session had stopped. `claude/guest-waits` and `claude/defaults-config` were
finished with green builds (runs 102 and 103); `claude/translator-wide`,
`claude/bench-harness` and `claude/memory-watches` did not exist. This check
carried on the one stalled branch.

Done: the step that was in progress, for the plain vertex shader form only
(above). Checked by compiling the three changed sources with the host's clang
against stub shader headers (no error in them), then by the Actions build
(its state is the last line of this section).

Deliberately not done, to keep to what a build can vouch for: loading the
point sprite and tessellation modules, vertex texture fetches, and every
item under "Next". The glslang tools could not be installed in this session,
so the rig was not rerun; the shaders it tests were not touched.

Build: run 105, commit `354899d`, success
(https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43547462886). It
proves the code compiles, that the Actions shader tools compiled
`ucode_program.vs.glsl` (the header it makes is now included, so a failure
there would have failed the build) and that the APK packs. Nothing about
behaviour. The commit that adds this paragraph changes this file only.

## Resume check, 04:30 (2026-11-14)

The branch's newest commit was 92 minutes old, nothing was in progress and
most of "Next" was untouched, so it counted as stalled.
`claude/guest-waits` (run 102) and `claude/defaults-config` (run 103) were
finished with green builds; `claude/bench-harness` and
`claude/memory-watches` had commits under 90 minutes old (their sessions were
alive or had just stopped, builds green); `claude/translator-wide` did not
exist: nothing of the translator's task has been started by anyone tonight.
This check carried on this branch alone.

Done: the tessellation forms of the interpreter in the draw path, behind
`vulkan_placeholder_interpreter_tessellation` ("Tessellated draws" above).
Chosen from "Next" because it is the one item whose shaders were already
written and whose C++ follows the path the 02:30 step laid; it changes
nothing with the setting off.

Checked by compiling `vulkan_pipeline_cache.cc` and
`vulkan_command_processor.cc` with the host's clang against stub shader
headers (no error in them), then by the Actions build (its state is the last
line of this section). The glslang tools are not in this session either, so
the rig was not rerun; the shaders it tests were not touched.

Deliberately not done:

- Vertex shaders that fetch textures (20 shader pairs in Fable II, the
  largest reason left after "not translated yet"). The interpreter's shaders
  already answer such a fetch with 0, and letting those draws through is a
  few lines, but a draw whose shape comes from a texture would then be drawn
  in the wrong place, which is the opposite of the task ("nothing drawn
  wrong"). It wants sampling in the vertex stages, checked on the rig.
- The point sprite form: see "Next".
- The frame-time split log, the pixel side, less compile work, the kept
  record: not started, as before.

Build: run 117, commit `cbe3740`, success
(https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43557597650). It
proves the code compiles, that the Actions shader tools compiled the four
`ucode_program_*.ds.glsl` (their headers are now included, so a failure
there would have failed the build) and that the APK packs. Nothing about
behaviour. The commit that adds this paragraph changes this file only.

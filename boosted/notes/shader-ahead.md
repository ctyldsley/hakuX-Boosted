# Shader ahead (branch `claude/shader-ahead`)

Base: `claude/integration-1113` at `d940365`. **Nothing here has been run on a
device.** The checks were a host syntax pass (clang, the changed files only,
with the header-only dependencies fetched) and the GitHub Actions build of the
branch. Every timing, every visual result and every hit rate is unknown until
the phone says.

Every new behaviour is behind a setting that is off by default. With the
defaults the only difference from the base is in the log: a skipped draw of
the placeholder model now also writes one `Draw skip reason:` line for each
pair of shaders, and `shader_profiling` adds a `placeholder detail:` line
after each placeholder's `pipeline create (placeholder)` line and one
`placeholder cover:` line a second.

## Status

Done:
- Part 2, cover more draws: late placeholders, placeholders created on a
  creation thread with a bounded wait, a vertex shader translated ahead of
  the queue for a draw that has nothing to make a placeholder from, a wait
  for the real pipeline of full-screen passes, and the reason for every
  skipped draw in the log. Commit `393f5ea`.
- Part 1, compile ahead: the look ahead in the command stream, translation
  ahead, speculative pipelines with their bounds and counters. Same commit.
- These notes.

In progress: nothing.

Next (not started, in this order): see "What I would do next" at the end.

## Builds

| Commit | What | Run |
|---|---|---|
| `393f5ea` | all the code | 81 (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43485706424), see the end of this file for its result |

## What the guest makes known, and when (from the code)

Paths are under `x360/emulator-core/src/main/cpp/xenia/src/xenia/gpu/`.

1. **A shader first exists for the emulator in a `PM4_IM_LOAD` packet.** It
   carries the shader's type, the guest address of its microcode and its size
   (`pm4_command_processor_implement.h`, `ExecutePacketType3_IM_LOAD`), or, as
   `PM4_IM_LOAD_IMMEDIATE`, the microcode itself inside the packet. Executing
   it hashes the microcode, finds or creates the `Shader` object
   (`VulkanPipelineCache::LoadShader`) and makes it the active vertex or
   pixel shader. Nothing earlier is visible: Direct3D is linked into the
   title, so creating a shader is the title's own code writing memory, and
   no kernel export or register write announces it.
2. **The packet is readable before it is executed.** The guest writes
   packets into the primary ring buffer and into indirect buffers that the
   primary buffer calls (`PM4_INDIRECT_BUFFER`), then moves the write
   pointer (`CommandProcessor::UpdateWritePointer`). The worker
   (`CommandProcessor::WorkerThreadMain`) then executes everything between
   its read pointer and that write pointer in one go
   (`ExecutePrimaryBuffer`). When that call starts, the whole stretch and
   every indirect buffer it calls are already in guest memory. So the
   earliest moment a shader is known is **the write pointer update that
   exposes its `IM_LOAD`**, and the time gained is however long the GPU
   command thread takes to execute from the start of that stretch to the
   shader's first draw. The code does not say how long that is: it depends
   on how often the title moves the write pointer (once a frame gives up to
   a frame, 33 ms; many small stretches give little). The log measures it
   (below).
3. **Shader constants do not identify a shader.** `PM4_SET_CONSTANT`,
   `PM4_LOAD_ALU_CONSTANT` and type 0 packets are register writes between
   the load and the draw. Among them are the registers that decide which
   *translation* of the shader a draw needs (the modification bits):
   `SQ_PROGRAM_CNTL` and `SQ_CONTEXT_MISC` (register counts, PsParamGen),
   `PA_CL_CLIP_CNTL` (clip planes, vertex kill), `RB_DEPTHCONTROL`,
   `RB_COLOR_MASK`, `RB_BLENDCONTROL0`, `RB_SURFACE_INFO`,
   `VGT_DRAW_INITIATOR` (`guest_spirv_shader_cache.cc`,
   `GetVertexShaderModification`, `GetPixelShaderModification`). They are
   only final when the draw packet is reached, and the vertex shader's
   modification also needs the pixel shader it is paired with (the
   interpolator mask) and what the primitive processor makes of the draw.
4. **The pipeline is only fully known at the draw.** `IssueDraw` works out
   the modifications, the render pass key (after the render target cache's
   update) and the description (`ConfigurePipeline`). Which states end up in
   the description depends on the device: with
   `vulkan_dynamic_pipeline_state` and the extended dynamic state features,
   cull, depth, stencil and blend state are not in it, and what is left is
   the two shaders with their modifications, the render pass key (formats
   and sample count), the geometry shader, tessellation and the topology
   class.

So a shader can be *analysed* at point 2 with certainty, *translated* at
point 2 on a prediction of its modification, and its pipeline *created* at
point 2 on a prediction of the whole description. Anything in a prediction
that turns out wrong costs a wasted translation or compile; it cannot draw a
wrong picture, because a draw finds its pipeline only by the description it
computes itself.

## Part 1: what was built

### The look ahead (`shader_ahead`)

`CommandProcessor::LookAheadPrimaryBuffer` (`command_processor.cc`) runs on
the GPU command thread just before each `ExecutePrimaryBuffer`. It walks the
stretch about to be executed and the indirect buffers it calls (four levels
deep at most, a million packets at most), reading packet headers only. It
executes nothing and writes no register. It follows:

- `IM_LOAD`: finds the shader by address and size in a small table (checked
  against the first and last microcode word), else calls the same
  `LoadShader` the execution will call;
- `IM_LOAD_IMMEDIATE`: the same from the packet's own words;
- `DRAW_INDX` and `DRAW_INDX_2`: when the pair of shaders differs from the
  previous draw's in the walk, tells the backend
  (`VulkanCommandProcessor::OnShaderPairAhead`,
  `VulkanPipelineCache::NoteShaderPairAhead`) with the primitive type from
  the packet.

Predicated packets are walked whatever the bin mask will be. A pair told
that is then not drawn costs at most a wasted prediction.

For a pair that no pipeline has been asked for yet,
`NoteShaderPairAhead`:

- analyses both shaders' microcode (`AnalyzeUcode`): work every draw needs
  first, moved earlier, not added;
- predicts the description the pair will be drawn with
  (`BuildDescriptionFromTemplate`): the state, and the state-dependent bits
  of both modifications, of a *template* pipeline, with the shader-dependent
  bits (the interpolator mask) worked out for the pair. Templates, in order:
  the newest pipelines that have the pair's vertex shader, failing that its
  pixel shader, failing that the pipeline a draw asked for last. A template
  is refused where the prediction could not be right: tessellation, point
  lists, PsParamGen, dynamic register addressing, a vertex shader type other
  than the plain one, or shaders that do not write the same outputs as the
  template's;
- queues the two translations of the likeliest prediction on the creation
  threads, ahead of queued pipelines (a new job kind that only translates).

In the default (deferred-bind) model this is what the setting buys: with
`async_shader_translate_on_draw` the GPU command thread translates a new
shader itself at its first draw, and a translation that was predicted right
is done, or under way on another thread, by then.

In the placeholder model a vertex shader translated ahead would have lost
its draw the interpreter (the interpreter is only used for a vertex shader
that is not translated, unless
`vulkan_placeholder_interpreter_translated_vs` is on). With `shader_ahead`
on, the interpreter is used for translated vertex shaders too, exactly as
that setting does.

### Speculative pipelines (`shader_ahead_speculate`)

Needs `shader_ahead` and `vulkan_async_skip_draws = true` (without the
latter the end of a submission waits for every queued pipeline). Two kinds
of prediction, counted separately:

- **A pair seen ahead** (`same VS`, `same PS`, `neighbour` in the log): one
  pipeline for each of the `shader_ahead_templates` (default 2) newest
  distinct states of the template source above.
- **Siblings** (`sibling`): when a draw asks for a pipeline of a pair in a
  state the pair had not been drawn in, and its vertex shader (or its pixel
  shader) has another pair that was drawn both in this state and in another
  one with the same shader stages and topology, the new pair is predicted
  in that other state too. That is the same objects drawn into another
  render target, as a rule. This one needs no look ahead and can run
  seconds ahead of the draw rather than a fraction of a frame.

A speculative pipeline is an ordinary slot in the pipeline map, marked, with
its creation job in a queue of its own. A creation thread takes from that
queue only when nothing real is queued, and at most `shader_ahead_threads`
(default 1) of them at a time, because a compile cannot be interrupted and
each thread on a guess is a thread a real pipeline may have to wait for.
When a draw arrives for such a slot (`OnPipelineReused`) it becomes a real
one: it is written to the pipeline storage only then (a wrong guess is not
compiled again at the next start), and if it was still queued it is queued
again as a real request and the speculative copy is dropped when its turn
comes.

Bounds, all settings:

| Setting | Default | Bound |
|---|---|---|
| `shader_ahead_max_outstanding` | 2 | speculative pipelines queued or compiling at once |
| `shader_ahead_max_unused` | 16 | speculative pipelines no draw has used yet |
| `shader_ahead_write_off_s` | 20 | seconds after which an unused one is written off as wasted (it leaves the unused count, stays in the map and can still be hit) |
| `shader_ahead_max_wasted` | 48 | written-off pipelines after which no more are made in the run (one log line says so) |
| `shader_ahead_threads` | 1 | creation threads on speculative work at once |
| `shader_ahead_templates` | 2 | predictions from one pair or one new pipeline |

At the measured 116 ms a pipeline, the worst case of the defaults is about
48 wasted compiles, 5.6 s of one low-priority thread, spread over at least
60 s by the unused cap and the write-off time.

## Part 2: which draws had no placeholder, from the code

Both lines come from `VulkanCommandProcessor::IssueDraw`.

**`Draw skipped (no interpreter placeholder, real pipeline not ready)`** is
logged when all of these hold: the placeholder model is on, the pipeline can
be asynchronous, `async_shader_skip_draws` is on, the ucode interpreter was
refused for this draw, and **this vertex shader translation is not done
yet**. The name is misleading: the draw is dropped because there is nothing
to make *any* placeholder from, and it goes on being dropped only until a
creation thread has translated the vertex shader, which it does at the
start of the pipeline's own job, whenever that job's turn comes behind
compiles of 116 ms each on four threads.

The interpreter is refused when any of these is true (the conditions of
`use_interpreter`):

| Log text | Cause |
|---|---|
| `async_shader_vs_interpreter is off` | the setting |
| `already translated and vulkan_placeholder_interpreter_translated_vs is off` | cannot produce this line (the vertex shader is then translated), can produce the next |
| `loaded inline (IM_LOAD_IMMEDIATE)` | the interpreter reads microcode from guest memory by address |
| `the device lacks full 32-bit indices` | device |
| `shared memory is split over several bindings` | device |
| `the draw is tessellated` | the vertex shader is the evaluation shader |
| `the primitive is expanded in the vertex shader` | no geometry shader on the device |
| `point list` | needs a point size the interpreter does not output |
| `rectangle or quad list and vulkan_placeholder_interpreter_geometry is off` | **full-screen passes**, unless that setting is on |
| `the vertex shader fetches textures` | interpreter has no sampling |
| `calls subroutines` / `exports to memory` / `has loops` | not implemented in the interpreter |

Run 73 had `vulkan_placeholder_pipelines` and `vulkan_placeholder_ps_mode`
only, as far as the handover says, so
`vulkan_placeholder_interpreter_geometry` was off and every rectangle list
was refused. Rectangle lists are how titles draw full-screen passes, and a
dropped full-screen pass is the whole frame wrong: this is my first
suspect for the dozen pairs and the full-screen flashes, **not confirmed**
(the device log is not in the repository). The new reason line will say.

**`Draw skipped (pipeline not ready yet)`** is logged when the slot has
neither a placeholder nor the real pipeline. A slot has no placeholder when:

- it was first drawn in the state above (interpreter refused, vertex shader
  not translated), and the placeholder is only ever made at the first draw.
  Once the vertex shader is translated the first line stops and this one
  starts, for the same draws, until the real pipeline is compiled. So the
  two lines are mostly **one population of draws seen at two moments**;
- `vulkan_placeholder_real_vs` is off and the interpreter was refused;
- the placeholder failed to create (`placeholder FAILED to create`).

A third skip was silent and is now counted: a slot whose placeholder is the
interpreter's, drawn with a vertex shader loaded inline.

## Part 2: what was built

All in the placeholder model only.

- **`vulkan_placeholder_late`**: a slot with neither placeholder nor
  pipeline gets its placeholder at a later draw, as soon as one can be made:
  the interpreter's if this draw allows it, else one from the translated
  vertex shader once it and the pixel shader are translated (the slot's
  layout must be the real one by then). Closes the second line for the
  first cause above. `ConfigurePipeline`'s placeholder block was moved
  whole into `MakePlaceholder` for this; a placeholder made late publishes
  itself only into an empty slot and is destroyed unused if the real
  pipeline got there first.
- **`vulkan_placeholder_translate_first`**: when a draw gets no placeholder
  because its vertex shader is not translated, that translation is queued
  ahead of every pipeline. With `vulkan_placeholder_late` the draw has its
  placeholder a frame or two later instead of after the queue. Closes most
  of the first line without translating on the GPU command thread.
  (`async_shader_skip_draws = false` closes all of it by translating on the
  GPU command thread, as before; `shader_ahead` aims to have it translated
  before the draw.)
- **`vulkan_placeholder_async`**, **`vulkan_placeholder_async_wait_ms`**
  (default 8): placeholders are created by a creation thread, ahead of
  everything else queued. The GPU command thread waits for them for at most
  the setting's milliseconds *per guest frame, all placeholders together*,
  then drops the draw (logged as `its placeholder is being compiled on a
  creation thread`); the placeholder is there at a later draw. This is the
  bound on the placeholder's worst case.
- **`vulkan_placeholder_wait_fullscreen_ms`** (0 = off): a full-screen pass
  (rectangle list, or an unindexed draw of at most four vertices: the test
  `vulkan_async_wait_fullscreen_draws` uses) gets no placeholder. Its
  shaders are translated on the GPU command thread, its real pipeline is
  queued ahead of everything and waited for, up to the setting, once for
  each such pipeline. This is the correct picture at the price of one hitch
  per new full-screen pipeline, for when a stand-in of a post-process pass
  is worse than a hitch. In this model a wait at the draw does save the
  draw (it is recorded with the handle), unlike the deferred-bind model's
  wait at the end of the submission (finding 1 of `pixel-stand-in.md`).
- **The reason log**: `VulkanPipelineCache::NoteDrawSkipped`, once for each
  pair of shaders and set of reasons (4,096 lines at most).

### Where a placeholder's time goes (the 271 ms)

From the code: a placeholder is one `vkCreateGraphicsPipelines` call on the
GPU command thread, inside `ConfigurePipeline`. There are two kinds, and the
old log did not say which a given time belonged to:

- **Interpreter placeholders** share two modules whatever the guest shaders
  are (the interpreter's vertex shader, 40 KB of SPIR-V, 52 KB for the
  stand-in's copy, and one of six small pixel shaders). Whether Turnip
  compiles them again for each pipeline or answers from its cache is the
  driver's business; the code only shows that the same `VkPipelineCache`
  object is passed every time.
- **Placeholders from the translated guest vertex shader** compile that
  shader. Compile time follows SPIR-V size on this driver, so these cost the
  vertex shader's share of a real compile, and they are made for every new
  pipeline of a vertex shader the interpreter was refused for, and (unless
  `vulkan_placeholder_interpreter_translated_vs` is on) for every new
  pipeline of a vertex shader that is already translated.

My expectation, **not confirmed**, is that 271 ms was of the second kind or
the first compile of an interpreter module. The new `placeholder detail:`
line after each `pipeline create (placeholder)` line names the kind, the
SPIR-V size of the vertex shader and the thread, so the existing run's
question is answered by the next one. `vulkan_placeholder_async` bounds
either kind.

## Settings to flip on the phone

Cold shader cache each time (delete `compose/cache/shaders` and the pipeline
cache), `shader_profiling = true` throughout. One step at a time.

Step 0, diagnosis only (no new behaviour): the placeholder model exactly as
in run 73. Read the `Draw skip reason:` and `placeholder detail:` lines.

```toml
[GPU]
shader_profiling = true

[Vulkan]
vulkan_placeholder_pipelines = true
vulkan_async_skip_draws = true
vulkan_placeholder_ps_mode = 2
```

Step 1, the pixel stand-in's own untested switches, which step 0's reasons
will most likely point at:

```toml
[Vulkan]
vulkan_placeholder_interpreter_geometry = true
vulkan_placeholder_interpreter_translated_vs = true
vulkan_placeholder_depth_only_interpreter = true
```

Step 2, cover what is left and bound the placeholder:

```toml
[Vulkan]
vulkan_placeholder_late = true
vulkan_placeholder_translate_first = true
vulkan_placeholder_async = true
vulkan_placeholder_async_wait_ms = 8
```

Step 3, only if full-screen passes look wrong with a stand-in:

```toml
[Vulkan]
vulkan_placeholder_wait_fullscreen_ms = 1500
```

Step 4, the look ahead, first alone in the default model (everything above
off), then with step 2:

```toml
[GPU]
shader_ahead = true
```

Step 5, speculation:

```toml
[GPU]
shader_ahead_speculate = true
```

## What to look for in `xe.log`

- `Draw skip reason: VS ..., PS ... (mask XXXXXXXX): ...`: one for each pair
  of shaders and set of reasons. **The line that names what is left.**
  Several `interpreter refused:` parts can appear together.
- `placeholder cover: draws skipped N (vertex shader not translated a, no
  placeholder b, placeholder compiling c, other d); placeholders made on the
  GPU command thread e (slowest X ms), handed to creation threads f, of all
  those late g; waits for a creation thread's placeholder h (Y ms), for a
  full-screen pass's real pipeline i (Z ms); translations done on the GPU
  command thread j (W ms)`: cumulative, once a second while it changes.
  Compare `draws skipped` with run 73's 2,956, and `slowest` with 271 ms.
- `shader_profiling: placeholder detail: VS ...: interpreter|translated guest
  vertex shader, N bytes of SPIR-V, stand-in|no-op pixel shader[, geometry
  shader], made on the GPU command thread|made late on the GPU command
  thread|made on a creation thread`: pairs with the
  `pipeline create (placeholder) ... ms` line just before it.
- `shader_profiling: draw waited X ms for the real pipeline of a full-screen
  pass, VS ..., PS ...[, gave up]`: step 3.
- `shader ahead: pairs seen ahead N (shaders new to the cache M),
  translations queued ahead T, first pipeline of a pair asked for X ms after
  the look ahead on average over K pair(s) (longest L ms); translations done
  on the GPU command thread j (W ms); speculative pipelines made A (same VS
  .., same PS .., neighbour .., sibling ..), hits H (...; ready r, compiling
  c, still queued q, after being written off w), misses S, written off as
  wasted O, not used yet U, outstanding P, compiled C in Y ms, dropped
  before compiling D, refused by the caps R`: cumulative, once a second
  while it changes.
  - `X ms after the look ahead` is the lead the look ahead gives. If it is a
    millisecond or two, the title moves the write pointer in small steps and
    the look ahead buys little: only the sibling rule is then worth
    keeping.
  - `translations done on the GPU command thread` against the same number
    with `shader_ahead` off (it is also on the `placeholder cover:` line) is
    what translation ahead saved.
  - `hits` against `made` and `written off`, by rule, is whether each
    prediction earns its compiles; `ready` hits are pipelines that were
    there before their first draw, `misses` are pipelines a draw asked for
    that no prediction had made; `compiled C in Y ms` is what the
    speculation cost.
- `shader ahead: N speculative pipelines were never used
  (shader_ahead_max_wasted): no more are made in this run`.
- The existing lines of `pixel-stand-in.md` still apply. A stand-in made
  late or handed to a creation thread says so at the end of its
  `stand-in made for` line.

## Cases still not covered, and why

With every setting above on:

1. **The first draws of a pipeline whose placeholder is on a creation
   thread**, when the frame's wait allowance is spent: dropped for a frame
   or two. That is the price of the bound.
2. **A draw the interpreter is refused for, before its vertex shader is
   translated**: dropped for the frame or two the translation takes, unless
   the look ahead had it translated in time, or
   `async_shader_skip_draws = false` translates on the GPU command thread.
3. **The interpreter's own limits are unchanged.** Loops, subroutine calls,
   vertex texture fetches, memory export, point lists, inline shaders and
   tessellation still refuse it, and such draws get the slower placeholder
   from the translated vertex shader (after translation, see 2). I did not
   widen the interpreter blind: a wrong loop draws exploded geometry, which
   is worse than a late object, and nothing yet says a title's vertex
   shaders use them. The reason line will.
4. **Operations the interpreter gets wrong but is not refused for**
   (`pixel-stand-in.md`: predicate pushes, `setp_inv/pop/clr/rstr`, the
   approximate `mulsc/addsc/subsc`, no user clip planes). Unchanged, and
   not detected.
5. **Draws without a pixel shader whose vertex shader the interpreter is
   refused for**: synchronous as before (translated and compiled on the GPU
   command thread at the first draw).
6. **Full-screen passes with a stand-in are approximations.** Step 3 trades
   that for a wait.
7. **A speculative slot hit before its real pipeline is ready, in the
   placeholder model, gets its placeholder at that draw only if one can be
   made then**; later draws need `vulkan_placeholder_late`.
8. **The fragment shader interlock path**: no stand-in (as before) and no
   speculation.

Predictions not attempted: pairs drawn as point lists, tessellated, with
PsParamGen or with dynamic register addressing; pairs for which a draw's
own register state would be needed (a pixel shader that is dropped because
colour writes are off, for example, is predicted with its pixel shader and
simply misses).

## What the build could not verify

Everything that matters:

- That any of it works when run: the look ahead's packet walk against real
  command streams, the placeholder made on another thread, the late
  publish, the wait. All reasoned from the code.
- How much lead the look ahead gives in any title.
- Whether any prediction rule hits often enough to pay for its compiles on
  a phone that is short of heat budget. The caps bound the cost, not the
  worth.
- Thread safety beyond what the code shows. The points a reviewer should
  look at first: `EnsurePipelineCreated`'s `publish_if_empty` branch and
  the early return above it (a placeholder against the real pipeline of the
  same slot from two threads), `CreationThread`'s three job kinds and the
  `creation_claimed` flag, and `OnPipelineReused`.
- That the speculative queue never delays a real pipeline by more than one
  compile on `shader_ahead_threads` threads (it cannot be interrupted).

Known weak points, by design or for lack of time:

- `VulkanPipelineCache::AwaitPipelineCompletion` (strict occlusion query
  paths) does not wait for a speculative compile in flight; a slot promoted
  while its speculative compile is running is waited for only by the
  per-slot waits.
- The look ahead tells a pair once (65,536 pairs remembered). A pair first
  seen while nothing could be predicted for it is not predicted again by
  the look ahead; the sibling rule still applies to it.
- `GetRealPipelinePriority` for a promoted slot uses the render pass's
  colour targets, not the draw's colour mask.

## Upstream files changed, and why (for `docs/ARCHITECTURE.md`)

All under `x360/emulator-core/src/main/cpp/xenia/src/xenia/gpu/`. Every
change is marked `hakuX-Boosted:`.

| File | Change |
|---|---|
| `gpu_flags.cc`, `gpu_flags.h` | `shader_ahead`, `shader_ahead_speculate`, at the end. |
| `command_processor.h`, `.cc` | `LookAheadPrimaryBuffer`, `LookAheadRange`, `LookAheadShader`, the virtual `OnShaderPairAhead`, their state; one call before `ExecutePrimaryBuffer` in the worker loop. |
| `vulkan/vulkan_command_processor.h`, `.cc` | `OnShaderPairAhead`; `IssueSwap` tells the pipeline cache the frame boundary; `IssueDraw`: `wait_for_real_pipeline`, `shader_ahead` in the interpreter's condition, the refusal reasons and a `NoteDrawSkipped` call at each skip. |
| `vulkan/vulkan_pipeline_cache.h` | New fields in `Pipeline` (not copied by its constructors) and in `PipelineCreationArguments` (job kind, flags, placeholder modules); `DrawSkipReason`; the new functions' declarations; the speculative queue, indexes and counters; `require_real` on `ConfigurePipeline`; `publish_if_empty` on `EnsurePipelineCreated`. |
| `vulkan/vulkan_pipeline_cache.cc` | Eleven settings; `ConfigurePipeline`'s placeholder block moved into `MakePlaceholder`, its two cache-hit returns call `OnPipelineReused`, a new slot is reported to `NoteRealPipeline`; `EnsurePipelineCreatedWithInterpreterPlaceholder` split (`GetInterpreterPlaceholderPixelShader`); `EnsurePipelineCreated`: the conditional publish, the detail line; `CreationThread`: the speculative queue, the placeholder and translate job kinds, the claim; `EnsureShadersTranslated` counts what the GPU command thread translated; `EndSubmission` calls `LogStatsIfDue`; the new functions in one block before `MaybeSaveVkPipelineCache`. |

Not changed: `pm4_command_processor_implement.h`, `shader.*`,
`vulkan_shader.*`, the shaders, the translator, anything under `kernel/` or
`cpu/`, `boosted/HANDOFF.md`, `boosted/docs/ARCHITECTURE.md`. The new
settings are not in the app's settings screen or `default_config.toml`;
they work from the config file.

## What I would do next

1. Read step 0's log: the reason lines decide everything below.
2. If rectangle lists are the dozen pairs: steps 1 and 2 are the answer and
   the question becomes how a stand-in full-screen pass looks (step 3 if
   badly).
3. If loops, texture fetches or point lists show up in the reasons: widen
   the stand-in's copy of the interpreter for that one, with the title's
   log to check against. Loops need a loop stack in `XeRunInterp`, the loop
   constants (already bound) and aL-relative addressing in `Operand`; point
   lists need a point size output and the point geometry shader's inputs.
4. If the look ahead's lead is short: drop the pair prediction and keep
   analysis, translation and the sibling rule; consider predicting from the
   previous *frame* (the pairs a frame drew are the best guess for the
   next), which needs no lead at all.
5. If interpreter placeholders are slow each time on Turnip (the
   `placeholder detail:` lines will show it): create one of each kind at
   start, on a creation thread.
6. The stored pipeline list already removes all of this on a second run;
   keeping it across reinstalls (the backlog) is still the largest win for
   the owner.

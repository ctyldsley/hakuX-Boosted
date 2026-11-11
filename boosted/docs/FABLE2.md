# Fable II (4D5307F1) on the 360 side

Where the title stands, what this version does for it, and what to try on a
device, in order. The starting point is XenDroid's own entry for the title
(`x360/GAME_COMPAT.md`, dated 2026-06-13); nothing here has been run by this
fork.

## Where it stands upstream

- Boots and reaches gameplay. The boot-time failures were emulator bugs,
  fixed upstream without title-specific code.
- **Open: the ground is not drawn** on the Vulkan backend. The draws are
  issued and fail the per-fragment depth test. Upstream's reading is a
  reversed-Z and 24-bit float depth precision mismatch, and notes that no
  Xenia fork has a Vulkan fix.
- **Open: an intermittent crash on loading screens** while the disc streams.
  Upstream describes a one-instruction title patch for a race in the game's
  own lock-free pool and says it ships in the repository; it is not in
  XenDroid's tree as imported (`x360/patches/` holds only the xenia-canary
  patch submodule, which does not contain it).
- Open: dialogue audio cut-offs.

So before performance, Fable II has one rendering bug and one stability bug
that decide whether it is playable at all.

## What this version does

- Ships the best-known per-game config
  (`boosted/x360-titles/config/4D5307F1.config.toml`):
  `[Vulkan] vulkan_depth_unorm24 = false`. It is written to the title's
  per-game config on first launch if you have not made one.
- Bundles xenia-canary's Fable II patches (they were already bundled by
  XenDroid). Open the title's Patches screen to turn them on. They match by
  the executable's hash; the Game of the Year / Platinum edition
  (`4145F96D2DEE2AB5`) has the fullest set.
- Moves Turnip's KGSL contexts off the ringbuffer the compositor preempts
  (`kgsl_context_priority`), which matters with the default `sysmem` mode.

## What to try, in order

Change one thing at a time and confirm in `xe.log` that it applied (search
for the cvar's name and for `Applied N game config override(s)`). Per-game
settings go in the title's per-game settings screen, or in
`Android/data/<package>/files/compose/config/4D5307F1.config.toml` under the
section named.

### 1. Baseline

Global setting `mount_cache = true` (section `[Storage]`; it cannot be set
per game). Custom Turnip driver selected. Note fps in Bowerstone Market and
whether the ground is drawn.

### 2. The ground

Upstream lists what did not help: `readback_resolve`, `readback_memexport`,
`occlusion_query`, `depth_float24_round`, `render_target_path_vulkan=fsi`,
`turnip_debug=nolrz`. One existing switch is not on that list and does what
upstream names as the remaining lead (quantising the pixel's depth to the
console's 24-bit float in the shader):

```toml
[GPU]
depth_float24_convert_in_pixel_shader = true
```

Try it with `vulkan_depth_unorm24 = false` kept, then with
`depth_float24_round = true` added. It costs fragment shader time and early
depth rejection, so if it does not fix the ground, turn it off again. If it
does, the next step is to find which draws need it and apply it only there.

### 3. Anti-aliasing off

Patches screen: "Disable MSAA". On a tile-based GPU this is likely the
largest single performance setting for the title, and it avoids the MSAA
paths behind the Adreno 830 hang described in
`x360/docs/a830-gmem-msaa-plan.md`.

### 4. Tiled rendering

With MSAA disabled by the patch, try Turnip's tiled mode:

```toml
[Vulkan]
turnip_debug = ""
```

Compare fps with step 3 over the same scene. If the GPU hangs or the picture
breaks, go back to `"sysmem"`. This is the step most likely to differ
between an Adreno 6xx, 7xx and 8xx.

### 5. Frame rate patches

"High Tick Rate" (30 Hz game logic), then "60 FPS". Only once steps 1 to 4
hold a steady 30.

### 6. Resolution

`draw_resolution_scale` above 1 turns off the `uma` readback path
(`readback_resolve`), so expect a cost beyond the extra pixels. Leave until
last.

## What to send back

For each step: device and GPU, the Turnip build, the fps before and after
(second run of each), whether the ground is drawn, and `xe.log`. With those,
the roadmap's phase 4 can be turned into code changes rather than guesses.

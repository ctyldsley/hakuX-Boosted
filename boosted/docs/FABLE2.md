# Fable II (4D5307F1) on the 360 side

Where the title stands, what this version does for it, and a test plan for
an Adreno 8xx device. The starting point is XenDroid's own entry for the
title (`x360/GAME_COMPAT.md`, 2026-06-13) and its Adreno 830 notes
(`x360/docs/a830-gmem-msaa-plan.md`, 2026-08-13). Nothing here has been run
by this fork.

## Where it stands upstream

- Boots and reaches gameplay. The boot-time failures were emulator bugs,
  fixed upstream without title-specific code.
- **Open: the ground is not drawn** on the Vulkan backend. The draws are
  issued and fail the per-fragment depth test. Upstream's reading is a
  reversed-Z and 24-bit float depth precision mismatch.
- **Open: an intermittent crash on loading screens** while the disc streams.
  Upstream describes a one-instruction title patch for a race in the game's
  own lock-free pool and says it ships in the repository; it is not in
  XenDroid's tree as imported (`x360/patches/` holds only the xenia-canary
  patch submodule, which does not contain it).
- Open: dialogue audio cut-offs.

So before performance, Fable II has one rendering bug and one stability bug
that decide whether it is playable at all.

## The performance lever on an Adreno 8xx

By default the 360 side tells Turnip to render untiled (`turnip_debug =
"sysmem"`). The settings screen itself labels that mode "untiled, slower".
It is the default because tiled rendering hangs the Adreno 830, and
upstream's notes pin down exactly when:

| Measured on Adreno 830 (`a830-gmem-msaa-plan.md`) | |
|---|---|
| Single-sample pass, tiled | Runs indefinitely |
| 2x MSAA pass, tiled | GPU fault within about 400 ms, every run |
| 2x MSAA pass, untiled | Fine |

The hang is multisampled passes in tiled mode and nothing else. Fable II
renders with MSAA, and xenia-canary's patch set (bundled) has a "Disable
MSAA" patch for it. With that patch on, every pass should be single-sample,
which is the case measured as stable in tiled mode. So for this title on
this GPU, tiled rendering may be usable, and it is the mode a tile-based GPU
is built for. That is the first thing to measure.

This version helps you check it: `xe.log` now says once whether the title
creates multisampled render targets ("This title creates multisampled render
targets"), so you can see that the patch took, and warns if MSAA and tiled
mode are on together.

## What this version does

- Ships the best-known per-game config
  (`boosted/x360-titles/config/4D5307F1.config.toml`):
  `vulkan_depth_unorm24 = false`, written to the title's per-game config on
  first launch if you have not made one.
- Bundles xenia-canary's Fable II patches (as XenDroid does). They match by
  the executable's hash; the Game of the Year / Platinum edition has the
  fullest set.
- Moves Turnip's KGSL contexts off the ringbuffer the compositor preempts
  (`kgsl_context_priority`).
- Puts every switch below in the per-game settings screen
  (`vulkan_depth_unorm24` and `kgsl_context_priority` were config-file only).

## Test plan

In the 360 library, long-press the game for **Per-game settings** and
**Game patches**. Change one thing per step, play the same scene (Bowerstone
Market is a good heavy one), and take the second run's numbers: the first
run after any change compiles shaders. A custom Turnip driver for Adreno 8xx
must be selected (360 settings, Vulkan, Custom Vulkan driver).

| Step | Change | Record |
|---|---|---|
| 0 | Global settings, Storage: **Mount cache** on. Nothing else. | fps; is the ground drawn; does `xe.log` say "multisampled render targets" |
| 1 | Game patches: **Disable MSAA** | fps; the MSAA line should be gone from `xe.log` |
| 2 | Per-game, Vulkan, **Turnip debug mode: None (GMEM)**, with step 1 kept | fps; any hang or device-lost in the first minutes |
| 3 | Per-game, GPU, **Depth float24 convert in pixel shader** on | Is the ground drawn; fps cost |
| 4 | Step 3 plus **Depth float24 round** on | Is the ground drawn |
| 5 | Game patches: **High Tick Rate**, then **60 FPS** | Only if steps 1 to 2 hold a steady 30 |

Notes on the steps:

- **Step 2 is the one that may hang.** If the picture freezes or the app
  dies, set Turnip debug mode back to sysmem. If `xe.log` still shows the
  multisampled line with the patch on, some pass is still multisampled
  (send the log: it names the sample count).
- **Step 3** is the existing switch that does what upstream names as the
  remaining lead for the ground (quantising the pixel's depth to the
  console's 24-bit float in the shader). It is not on upstream's list of
  things already tried. It costs fragment shader time and early depth
  rejection, so turn it off again if it changes nothing.
- **Resolution scale** above 1x turns off the direct readback path
  (`readback_resolve = uma`), so it costs more than the extra pixels. Leave
  it until the above is settled.

## What to send back

For each step: fps before and after, whether the ground is drawn, any hang,
and `xe.log` (360 library menu, "Open user data"; the file is in the data
folder's root, and older sessions are zipped under `logs/`). Also the device
and the Turnip build's name. With those, the next changes can be aimed at
what your device actually does.

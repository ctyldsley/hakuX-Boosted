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
- **An intermittent crash on loading screens** while the disc streams.
  Upstream wrote a one-instruction title patch for a race in the game's own
  lock-free pool and calls it required. The file is no longer in XenDroid's
  tree; it was recovered from XenDroid's history (commit f8a24a3c) and ships
  here, enabled (`boosted/x360-titles/patches`). It applies to the Game of
  the Year / Platinum executable (hash `4145F96D2DEE2AB5`) only.
- Open: dialogue audio cut-offs.

So before performance, Fable II has one rendering bug that decides whether
it is playable at all.

Those notes are from 13 to 16 June 2026 and may be out of date on the
ground: XenDroid's history has tessellation fixes from 12 June ("Fix
tessellation spacing and CP-indexed control point indices", "Fix adaptive
tessellation issues") and one from 5 July, after the notes ("Fix
tessellation vertex index endian"). That last one is a strong candidate
for having fixed this title already: its message reads "Use
host_shader_index_endian (kNone for auto draws) instead of raw
VGT_DMA_SIZE.swap_mode", and this title's ground is drawn as auto-indexed
patches, whose patch index would have been byte-swapped by a stale swap
mode before it. The fix is in the tree this fork builds. Step 0 of the plan
below says whether the ground is still missing; if it is drawn, skip steps
3 to 5.

Xenia Canary's tracker (game-compatibility issue 74) has the same history
on Windows: "Ground is entirely missing" in January 2021, reported fixed in
April 2022, the month upstream Xenia made the tessellator's patch index
follow the index offset ("Offset and clamp tessellation patch index"). So
on the reference backend the ground was a tessellation fault, not a depth
one.

## The ground is drawn by the tessellator

Xenia's source names this title's ground in two places: `gpu/xenos.h`
("4D5307F1 draws single ground patches by passing 1 as the index count",
"auto-indexed patches with a nonzero VGT_INDX_OFFSET") and
`gpu/primitive_processor.cc` ("4D5307F1 - ground - continuous", under quad
patches). So the missing ground is the one part of the scene that goes
through the console's tessellator: on Vulkan, a small vertex shader and a
tessellation control shader of the emulator's own, then the game's vertex
shader run as the tessellation evaluation shader. That path is newer than
the rest of the Vulkan backend (a "TODO: Tessellation" from before it was
written still sits beside it) and few titles use it, so it has had far less
use than the ordinary draw path. It changes where to look: a fault there
would also make draws "issued, but writing nothing".

A read of that path against the reference translator (the D3D12 one, in the
same tree, written when this title's ground was first made to work) found:

- For this title's kind of patch (quad, patch-indexed, continuous) the
  register set-up, the tessellation factors, the index handling and the
  constants match, with one difference: the Vulkan translator puts 1.0 in
  `r0.w` where the reference leaves 0. Unlikely to matter (nothing maps to
  that component), not ruled out.
- A separate fault that does not affect this title, since fixed: discrete
  and continuous patches given by control points (triangle and quad lists)
  were built with one control point per patch and the one-control-point
  shader (`vulkan_pipeline_cache.cc`, `patchControlPoints`), while the
  evaluation shader reads three or four. They now use the 3 and 4 control
  point shaders, as the reference does (`STATUS.md`).
- Upstream's depth reading is not excluded by any of this.

Reading cannot settle it, a run can. `tessellation_debug` (per-game
settings, GPU) logs each kind of tessellated draw with its state and the
game's vertex shader, and can turn off culling or the depth test for
tessellated draws only:

| Ground appears with | Then the fault is |
|---|---|
| 1 (no culling) | Winding of the tessellator's triangles: a one-line fix |
| 2 (no depth test) | The depth the ground's shader produces or is tested against |
| Neither | Position: the patches are not where they should be, or not rasterized. The logged shader says what it reads |

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
- Installs the loading-screen crash fix, enabled.
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
| 3 | Per-game, GPU, **Tessellation diagnostics**: 1, then 2, then 3 (a launch each), then back to 0 | For each: is the ground drawn. Send `xe.log` from the run at 3 |
| 4 | Per-game, GPU, **Depth float24 convert in pixel shader** on | Is the ground drawn; fps cost |
| 5 | Step 4 plus **Depth float24 round** on | Is the ground drawn |
| 6 | Game patches: **High Tick Rate**, then **60 FPS** | Only if steps 1 to 2 hold a steady 30 |

Notes on the steps:

- **Step 2 is the one that may hang.** If the picture freezes or the app
  dies, set Turnip debug mode back to sysmem. If `xe.log` still shows the
  multisampled line with the patch on, some pass is still multisampled
  (send the log: it names the sample count).
- **Step 3** is the most useful one for the ground: its three answers and
  the logged shader decide what gets fixed next (the table above). With the
  depth test off the ground will draw over things in front of it; that is
  expected.
- **Step 4** is the existing switch that does what upstream names as the
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

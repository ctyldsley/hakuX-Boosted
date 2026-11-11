# Status

What is known about this version and how. Updated with each change.

## Verified

| What | How |
|---|---|
| hakuX's core builds from this tree on Linux with NDK 29.0.14206865 | GitHub Actions, job "Xbox core (hakuX)" |
| The native dependency revisions in `boosted/tools/fetch-native-deps.sh` are the ones hakuX's sources build against | The same job; and hakuX's host tests (`tests/hle`) configured against them |
| The whole app builds into one debug APK: hakuX's core, the 360 core (libe.so), the Kotlin of both sides under hakuX's toolchain (Kotlin 2.2, AGP 8.13, R8 on), one merged manifest | GitHub Actions, job "APK (both cores)", first passed at d043094. The job's "What the APK holds" step lists the libraries and activities in the log |
| hakuX's adrenotools patch applies unchanged to the 360 side's adrenotools | `git apply --check`; the files' blob hashes are the patch's own base |

## Upstreams

- hakuX: `rfandango/hakuX` master at a7cdb5a (v1.3.0 and 14 commits),
  merged without a conflict.
- XenDroid: `rfandango/XenDroid` main at 779680a.

## First device run (2026-11-11)

Run 13's APK on an Adreno 840 phone, Fable II: the app, the 360 library and
the game start and reach play. It ran on Qualcomm's driver (no Turnip was
installed), drew with flicker, skipped 2,253 draws and stalled at frame
9210. Details in `boosted/HANDOFF.md`. Nothing has run on Turnip yet.

## Not verified: nothing below has run on a device

No build of this fork has been installed or run. In particular:

- **The combined app.** That the 360 library opens from hakuX's library,
  that a 360 image picked in hakuX's library boots in the 360 core, that
  both sides' settings and data survive side by side.
- **`kgsl_context_priority`.** hakuX runs the same hook on the same driver
  stack, but through its own loader code. Check `xe.log` for `Custom Vulkan
  driver's kgsl draw contexts at priority 7` and logcat for
  `hook_ioctl: kgsl draw context ... created at priority 7`. To rule it out
  when chasing a problem: `[Vulkan] kgsl_context_priority = 0`.
- **Per-title defaults.** Check that
  `Android/data/<package>/files/compose/config/4D5307F1.config.toml` exists
  after Fable II's first launch and that `xe.log` reports the override.
- **The MSAA log line and `tessellation_debug`.** Built, not run. They
  change nothing unless multisampled targets are created or the setting is
  not 0.
- **Control points of tessellated triangle and quad lists.** Discrete and
  continuous patches given by control points were built with one control
  point per patch on Vulkan, where the evaluation shader (the game's vertex
  shader) reads three or four: each index was a patch of its own and the
  other corners were read out of bounds. They now get 3 / 4 and the 3cp / 4cp
  control shader, as the D3D12 backend does
  (`gpu/vulkan/vulkan_pipeline_cache.cc`). Patch-indexed and adaptive draws
  are as they were. The kind of draw is read from the vertex shader
  modification the pipeline description already holds, so the description's
  layout and the pipeline storage version are unchanged: a stored pipeline
  of this kind is rebuilt correctly when it is loaded. Checked by reading
  and by compiling the control shaders (3 / 4 output vertices, one index
  passed through per control point); built (Actions, job "APK (both
  cores)", at 5d7f29e, which is also the first build of
  `tessellation_debug`); not run. Known user: title 58410823,
  the retro screen and beams in the main menu (quad list, continuous);
  `tessellation_debug` logs host vertex shader type 3 (triangle) or 5 (quad)
  for such draws. Fable II's ground is patch-indexed (type 6): not a test
  for this.
- **Performance.** No change in this version has been measured. The only
  one expected to affect frame delivery is `kgsl_context_priority`, and its
  expected effect is fewer GPU hangs and stalls under GPU composition, not
  a higher average frame rate.

## First run: what to check

1. Install the debug APK from the Actions run. hakuX's own games should
   behave exactly as in hakuX 1.3.0 (upstream master at a7cdb5a): nothing in
   its core changed.
2. Library, "Xbox 360": the 360 library opens. Pick its games folder there
   (it keeps its own).
3. Launch a 360 title from the 360 library; then put a 360 ISO in hakuX's
   games folder and launch it from hakuX's library (it should go to the 360
   core).
4. With a custom Turnip driver selected on the 360 side, look for the two
   log lines above.
5. Fable II: `boosted/docs/FABLE2.md`.

## Known gaps

- hakuX's host tests (`tests/hle`) do not all build with GCC on Linux
  (`-Werror` on a missing initialiser in `test_pusher.c`, `libm` not linked
  for `test_apu_hrtf`). Upstream's issue, untouched; the tests are not part
  of the Actions build.
- The two sides have separate libraries, settings and driver managers
  (`ROADMAP.md`, phase 5).
- The debug APK is signed with the build machine's debug key, so one run's
  APK will not install over another's without uninstalling first. Set up
  `android/key.properties` (see `android/README.md`) for a stable key.

## Licence question to settle before distributing builds

Xenia is BSD 3-Clause and hakuX is GPL-3.0-or-later; those combine. But
XenDroid's own Android code has no stated licence for most files: 29 files
carry `SPDX-License-Identifier: WTFPL`, 76 of the 79 Kotlin frontend files
carry nothing, and the repository has no top-level licence file (its README
defers to per-file headers). hakuX and XenDroid are published by the same
account, so asking upstream to state XenDroid's licence should be simple.
Until it does, treat redistribution of APKs containing the XenDroid frontend
as not cleared. Building and running it yourself is not affected.

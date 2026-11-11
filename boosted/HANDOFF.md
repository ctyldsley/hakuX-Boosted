# Handover: hakuX-Boosted

The one file to read first in any new session, cloud or desktop. It says
where the work stands, what is waiting on whom, and how to work in this
repository. **Update it before a session ends** (the "Right now" section
and a line in the log at the bottom), and commit it with the work.

Last updated: 2026-11-11 (after the first device run).

## Right now

- **Branch:** `claude/xbox360-emulator-architecture-eac8l7`. All work is on
  it. `master` is still upstream hakuX 1.2.0; nothing has been merged to it
  and there is no pull request.
- **First device run done** (2026-11-11, run 13's APK, Fable II GOTY, a
  Snapdragon 8 Elite Gen 5 phone: SM8850, Adreno 840, 16 GB, Android 17).
  The app, the 360 library and the game start; the game reached play, drew
  with flicker and glitches, then froze. What the logs say is under
  "First device run" below.
- **The bundled Turnip driver is the change waiting to be tried.** That run
  was on Qualcomm's stock driver: no Turnip package had been installed, so
  none of the Turnip-side work was in effect. The app now ships StevenMXZ's
  Turnip (v39) and uses it for the 360 core by default. Built on Actions:
  run 18, commit `8a8618a`
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43050816108),
  the APK is its artifact `hakuX-Boosted-debug-apk`, now signed with one
  fixed debug key so later builds install over it. Not run on a device yet.
- **To check on the next run: did the "Disable MSAA" patch apply?** Run 13
  was meant to turn it on for Fable II by itself, yet that run's log says
  the title creates 4x multisampled targets. Either the ISO's executable is
  not one the bundled patch files match by hash, or the seeding did not
  happen. Look for `Patcher: Applying patch for` in `xe.log` and for the
  title's files in `Android/data/<package>/files/compose/patches/`.
- **Asked for by the owner, not started:** one library for both consoles
  instead of the separate "Xbox 360" section, on-screen controls and
  in-game settings (graphics, emulation, controls) for 360 games in the
  same style as hakuX's. A 360 image opened from hakuX's library already
  goes to the 360 core (`boosted/X360.kt`); the screens are still XenDroid's
  own.

## First device run (2026-11-11): what the logs say

The logs are the 360 side's session ZIP
(`Android/data/<package>/files/compose/logs/`) and logcat.

- **Driver: Qualcomm's, not Turnip.** `xe.log`: `driverID:
  QualcommProprietary`, `vulkan_lib_path` empty. `TU_DEBUG=sysmem` and the
  KGSL priority hook do nothing on it. Judge rendering only on Turnip.
- **The freeze is the guest stalling, not a crash.** At frame 9210, about
  525 s in, `GuestScheduler: no guest frame presented in 2000 watchdog
  ticks`: '3D Engine' running at `lr=82B9BF98`, 'Texture Streamer' at
  `82CC207C`, 'XThread000F' (the XMA thread, `F8000070`) not switching
  fibers at `82C45A48` with `XmaContext: kick still unserviced` before it.
  The same pair of warnings appeared and cleared at frame 4154. Not
  explained yet; see whether it survives the driver change first.
- **2,253 draws were skipped**, all one kind: `UpdateBindings: pipeline
  layout counts (VS 0t/0s, PS 0t/0s) do not match the draw's binding counts
  (VS 6t/3s, PS 0t/0s)`. A vertex shader with six textures drawn with a
  pipeline whose layout has none ("the minimal layout from untranslated
  shaders", `vulkan_command_processor.cc`). Missing geometry for certain;
  a candidate for Fable II's ground (vertex texture fetch is how a
  heightfield is displaced). Independent of the driver.
- **The "reboot" was Android killing the app.** The phone did not restart
  (kernel uptime unbroken). When the frozen game was left, both processes
  were killed (`critical_mem_pressure`) and the system restarted the
  emulator activity, which booted the game again. The phone was short of
  memory for the whole session (65 background apps killed in 8 minutes);
  what the emulator process itself held was not captured.
- **No touch controls at first: fixed.** The phone's built-in
  `uinput-xiaomi` device reports gamepad keys, so the 360 side took it for a
  controller and started with the touch controls hidden. It now uses hakuX's
  test of a real controller (`settings/TouchOverlayDefault.kt`).
- **The title is 4x MSAA** (the log line this fork added), so tiled mode
  needs the MSAA patch first, as expected.

## What to do next, by what the device test says

| Result | Next |
|---|---|
| (now) Fable II on the bundled Turnip | Check `xe.log` says `driverID: MesaTurnip` (not `QualcommProprietary`); then whether the flicker, the skipped draws and the frame 9210 stall are still there |
| The skipped draws remain | Find why the pipeline layout is the minimal one for a vertex shader with textures (`UpdateBindings`, `vulkan_command_processor.cc`); suspect the tessellated-draw path |
| The stall remains | The XMA thread that stops yielding (`lr=82C45A48`) and the 3D Engine's wait (`lr=82B9BF98`): disassemble both guest addresses |
| The app or the 360 library does not open | Logcat first. Suspects: `Application.ensure_init` (the 360 side's start-up inside hakuX's app), R8 on the 360 Kotlin, the manifest merge (`boosted/android/x360-ui`) |
| Fable II boots, ground drawn | Go to performance: tiled mode (step 1 of `FABLE2.md`), then the frame rate patches |
| Ground missing | `tessellation_debug` 1, 2, 3 and the logged vertex shader decide it (table in `FABLE2.md`) |
| Tiled mode holds with MSAA off | Ship it as the title's default (`boosted/x360-titles/config`), then repeat the recipe on the next titles |
| Tiled mode hangs | Read `xe.log` for the multisample line (a pass still multisampled); stay on sysmem; look at pass count and the JIT instead (`docs/ROADMAP.md` phase 4) |
| GPU hangs with the compositor active | Check the `kgsl draw contexts at priority 7` log line; `kgsl_context_priority = 0` rules the hook out |

## What this fork is, in five lines

- hakuX (original Xbox, upstream `rfandango/hakuX`) with XenDroid (Xbox
  360, upstream `rfandango/XenDroid`, a Xenia Edge fork) under `x360/` as a
  git subtree. One Android app, two cores, each in its own process.
- The goal is not the merge itself: it is to make Xbox 360 on Snapdragon
  as good as hakuX made the original Xbox. Fable II is the first target.
- hakuX's side is left alone so upstream merges stay trivial (the 1.3.0
  merge had no conflicts).
- Everything of this fork's own is in `boosted/`, `.github/workflows/` and
  the `hakuX-Boosted:` comments in the two upstream trees.
- Read `docs/STATUS.md` for what is verified, `docs/ARCHITECTURE.md` for
  the layout and merge procedures, `docs/CROSSOVER.md` for what does and
  does not carry over from hakuX, `docs/ROADMAP.md` for the plan.

## Findings that should not be rediscovered

- **There is no hakuX-style leap to copy.** hakuX's jump was low-level to
  high-level emulation; Xenia has been high-level from the start, and both
  projects share an author, so most small tricks are already in XenDroid.
  Gains have to be measured on a device, one at a time.
- **Tiled rendering is the first lever.** XenDroid defaults Turnip to
  `sysmem` (untiled, slower) because tiled mode hangs the Adreno 830, and
  its own notes show the hang is on multisampled passes only. A title with
  MSAA patched off may run tiled.
- **Fable II's ground is tessellated** (continuous quad patches), not a
  depth problem per se, and may already be fixed: XenDroid's fix to the
  patch index of auto-indexed tessellated draws (2fb3c036, 5 July) is newer
  than its notes on the missing ground.
- **Zero-copy guest memory is not a quick win.** It needs hakuX's
  first-write barrier to be correct (the GPU reads at submit time), and its
  gain is unproven on GPU-bound titles. `docs/ROADMAP.md` phase 3.
- **Xenia Canary's patch set is already bundled** (501 files, the
  `x360/patches/xenia-canary` submodule). `python3
  boosted/tools/title-info.py <title>` pools what is known about a title.
- **XDK documentation is not used.** The copies in circulation are leaks;
  Xenia's source, Canary's data and Mesa's freedreno cover the same ground.
- **XenDroid's Kotlin frontend has no stated licence** (`docs/STATUS.md`).
  Settle it with upstream before distributing builds.

## How to work here

**Building.** Only GitHub Actions builds the app (`.github/workflows/
boosted.yml`): every push to a `claude/**` or `boosted/**` branch builds
hakuX's core, then the APK with both cores, and checks what the APK holds.
A warm run takes about 12 minutes. By hand on a desktop:
`boosted/README.md`.

**Installing a build:** debug APKs are signed with the fork's own debug key
(`boosted/android/signing`) from the build after run 13, so each installs
over the last (`adb install -r`). Run 13's APK was signed with a runner's
throwaway key and has to be uninstalled once.

**In a cloud session:**
- The container cannot reach Google's Android SDK host, so there is no
  local NDK or Gradle build. Push and read the Actions run.
- It cannot download Actions artifacts either. The job's "What the APK
  holds" step is the evidence of what was built.
- `gh` is for search only (`gh api -X GET search/commits`, `search/issues`).
  Searching `repo:rfandango/XenDroid` commits is how the Fable II patch was
  found; `git fetch <sha>` from a public repository works.
- A newer push cancels the run before it.

**On a desktop:** the same branch. With the Android SDK installed you can
build locally and use adb; the device etiquette in upstream's own
`HANDOFF.md` (repository root, hakuX's) applies to hakuX's devices.

**Rules of this fork:**
- Never claim a speed-up that was not measured on a device. `STATUS.md`
  separates built from run; keep it true.
- Changes to upstream files are small, marked `hakuX-Boosted:`, and listed
  in `docs/ARCHITECTURE.md`. hakuX's core is not modified.
- What a title is known to need goes in `boosted/x360-titles` so it applies
  without the user doing anything: `config/` (per-game settings), `patches/`
  (our own patches, shipped enabled), `enable/` (names of bundled Canary
  patches on by default). Anything that can hang or that changes how a game
  plays stays a choice.
- Commit messages follow upstream's style: `area: what changed and why`,
  with the reasoning in the body and "Not built or run yet" where true.
- UK English, no em dashes.

**Taking upstream in:**
- hakuX: `git fetch https://github.com/rfandango/hakuX.git master && git
  merge FETCH_HEAD`; if `android/README.md`'s dependency table changed,
  update `boosted/tools/fetch-native-deps.sh` (it checks commits).
- XenDroid: `git subtree pull --prefix=x360
  https://github.com/rfandango/XenDroid main --squash`, then
  `python3 boosted/tools/sync-x360-gitmodules.py`, then compare XenDroid's
  Gradle files and manifest with `boosted/android/` (`docs/ARCHITECTURE.md`).
  Currently at hakuX `a7cdb5a` (v1.3.0 + 14) and XenDroid `779680a`.

## Settings this fork added (360 side)

| Setting | Section | Default | What |
|---|---|---|---|
| `kgsl_context_priority` | Vulkan | 7 | Turnip's KGSL contexts on the compositor's ringbuffer (hakuX's hook); 0 leaves the driver's |
| `tessellation_debug` | GPU | 0 | Logs tessellated draws and their vertex shader; 1 no culling, 2 no depth test, 3 both |
| (log line) | | | "This title creates multisampled render targets", a warning when tiled mode is on |

## The bundled Turnip driver

- `boosted/drivers/turnip.lock.json` pins a release of
  `StevenMXZ/Adreno-Tools-Drivers` and each package's SHA-256: the plain
  upstream-Mesa builds, one for Adreno 8xx and one for 7xx (not the
  "Patched" builds, not the One UI ones).
- `boosted/tools/fetch-turnip.py` downloads them for the build (checked
  against the lock); Gradle packs them as `assets/gpu-drivers`.
- `x360/app/.../core/BundledDriver.kt` unpacks the one for the phone's GPU
  and sets `vulkan_lib_path` to it, once. A driver the user installed, or
  the system driver once they chose it, is left alone. hakuX's core keeps
  its own choice of driver; the package is only added to its driver list.
- `.github/workflows/turnip-sync.yml` moves the lock to the newest release,
  commits it and starts a build. Its daily schedule only runs once the file
  is on the repository's default branch; until then run it by hand
  (Actions, "Turnip sync"). Every build also notes when a newer release is
  out.
- The packages are a third party's prebuilt binaries. Run a new one on a
  device before passing a build on.

## Session log

Newest first. One or two lines each: what changed, what was verified.

- **2026-11-11 (cloud, later).** Checked what the desktop session pushed:
  all five commits are on the main branch, none on a side branch, and run
  18 built them (after three runs lost to the runner's package mirror).
  Noted the MSAA patch question above. No code changed.

- **2026-11-11 (desktop, phone on adb).** Read the first device run's logs
  (section above). Bundled StevenMXZ's Turnip v39 as the 360 core's default
  driver on Adreno 7xx and 8xx, with the lock file, the fetch script and the
  sync workflow. Verified: the lock's hashes against the downloads. Not
  verified: the build, and anything on the device.

- **2026-11-11 (cloud).** Created the fork's structure: XenDroid imported
  as the `x360/` subtree, the two Gradle modules and the bridge that put
  both cores in one app, Actions build. Ported hakuX's KGSL priority hook.
  Merged hakuX 1.3.0+14. Recovered Fable II's streaming crash patch from
  XenDroid's history; per-title defaults (config, patches, enabled Canary
  patches) with Fable II's. Added the MSAA log line and
  `tessellation_debug`. Merged the control point tessellation fix from a
  second session. Verified: builds on Actions (run 13, `8345f8d`). Not
  verified: anything on a device.

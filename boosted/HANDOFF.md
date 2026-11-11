# Handover: hakuX-Boosted

The one file to read first in any new session, cloud or desktop. It says
where the work stands, what is waiting on whom, and how to work in this
repository. **Update it before a session ends** (the "Right now" section
and a line in the log at the bottom), and commit it with the work.

Last updated: 2026-11-11.

## Right now

- **Branch:** `claude/xbox360-emulator-architecture-eac8l7`. All work is on
  it. `master` is still upstream hakuX 1.2.0; nothing has been merged to it
  and there is no pull request.
- **Last verified build:** commit `8345f8d`, GitHub Actions run 13
  (https://github.com/ctyldsley/hakuX-Boosted/actions/runs/43024961723):
  both jobs passed, the debug APK is its artifact
  `hakuX-Boosted-debug-apk` (kept 14 days).
- **Waiting on the owner:** the first run on a device. Nothing in this fork
  has ever run. The test is Fable II (GOTY ISO) on an Adreno 8xx with a
  custom Turnip driver; the steps and what to send back are in
  `docs/FABLE2.md`.
- **Nothing is in flight.** No uncommitted work, no other open branch
  (`claude/busy-hamilton-xxgrqy`, the control point tessellation fix, is
  merged and can be deleted).

## What to do next, by what the device test says

| Result | Next |
|---|---|
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

**In a cloud session:**
- The container cannot reach Google's Android SDK host, so there is no
  local NDK or Gradle build. Push and read the Actions run.
- It cannot download Actions artifacts either. The job's "What the APK
  holds" step is the evidence of what was built.
- `gh` is for search only (`gh api -X GET search/commits`, `search/issues`).
  Searching `repo:rfandango/XenDroid` commits is how the Fable II patch was
  found; `git fetch <sha>` from a public repository works.
- Pushes queue builds; a newer push replaces a queued one but does not
  cancel a running one.

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

## Session log

Newest first. One or two lines each: what changed, what was verified.

- **2026-11-11 (cloud).** Created the fork's structure: XenDroid imported
  as the `x360/` subtree, the two Gradle modules and the bridge that put
  both cores in one app, Actions build. Ported hakuX's KGSL priority hook.
  Merged hakuX 1.3.0+14. Recovered Fable II's streaming crash patch from
  XenDroid's history; per-title defaults (config, patches, enabled Canary
  patches) with Fable II's. Added the MSAA log line and
  `tessellation_debug`. Merged the control point tessellation fix from a
  second session. Verified: builds on Actions (run 13, `8345f8d`). Not
  verified: anything on a device.

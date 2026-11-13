# `claude/defaults-config`: defaults, a config that cannot be lost, the settings screens, the look

Cut from `claude/integration-1113` at `8e03766`.

**Nothing here was seen on a device.** A cloud session cannot run the app.
Every statement about behaviour below is what the code is meant to do. The
only check is the GitHub Actions build of this branch.

## Where it stands

- Done: step 1 (defaults), step 2 (the config store), step 3 (settings
  screens), step 4 (the look), each as written code.
- In progress: getting the build green with all four.
- Next, for whoever has the phone: the checks listed under each step. For a
  later session: the items under "Not done".

## Step 1: defaults

### What the evidence in the repository does and does not hold

The brief expected a list of tuned values in `boosted/HANDOFF.md`. There is
none. The tuned config itself is on the desktop PC
(`device-logs/restore-config.toml`, and the `CONFIG DUMP` at the top of any
`xe.log`), not in the repository. What the handoff does say about the
phone's config, entry by entry:

- it was made from the bundled `default_config.toml` at the first install
  (the 360 side copies it in) and edited from there;
- "the phone's config is back to the baseline (all switches off,
  `host_perf_log` on)" (run 73), and the baseline runs are "every new
  switch off";
- `kgsl_context_priority = 0` (since run 33; also the code's default now);
- the diagnostics `host_perf_log`, `guest_scheduler_stats` and
  `shader_profiling` were on for measurement runs;
- the last two days' switches (placeholder model, look ahead, skew bound,
  translator, parking), which stay off;
- `defaults_date = 2026072714`, the bundled file's;
- a signed-in profile.

So, by the handoff, the tuned config is the bundled file plus a signed-in
profile, the bundled Turnip driver's path and diagnostics. No resolution,
filtering or frame-rate value of the owner's is recorded anywhere in the
repository. **If the owner changed such values in the settings screen, they
are in the `CONFIG DUMP` and nowhere I can read: the desktop session should
diff `device-logs/restore-config.toml` against
`x360/emulator-core/src/main/assets/config/default_config.toml` and add
what is proven to the table below.** The command, on the PC:

```bash
python3 - <<'EOF'
import tomllib
a = tomllib.load(open('device-logs/restore-config.toml', 'rb'))
b = tomllib.load(open('x360/emulator-core/src/main/assets/config/default_config.toml', 'rb'))
for s in sorted(set(a) | set(b)):
    for k in sorted(set(a.get(s, {})) | set(b.get(s, {}))):
        x, y = a.get(s, {}).get(k, '<absent>'), b.get(s, {}).get(k, '<absent>')
        if x != y: print(f'[{s}] {k}: phone {x!r}, bundled {y!r}')
EOF
```

### Why the run on defaults was so much worse (read from the code)

Not, as far as the code shows, because of setting defaults. Three things
the app writes into the config once per install were not written again
when the config was rebuilt from the bundled file:

| What | Written by | Why it was not rewritten | Effect on that run |
|---|---|---|---|
| `Vulkan.vulkan_lib_path` (the bundled Turnip) | `BundledDriver.install` | its "offered once" flag was already set, and an empty path with the flag set is read as the player choosing the system driver | **the title ran on Qualcomm's stock driver**, which the first device run (2026-11-11) already showed to draw wrongly and slowly; the 360 core's Turnip settings do nothing on it |
| `Profiles.logged_profile_slot_0_xuid` | `ProfileBootstrap` | it only signs in when no profile exists at all | nobody signed in: no saves shown, cannot save |
| `HID.show_touch_overlay` | `seedTouchOverlayDefault` | its "seeded" flag was already set | the core's own default instead of the per-phone choice |

This is a reading of `BundledDriver.kt`, `ProfileBootstrap.kt` and
`TouchOverlayDefault.kt`, not an observation. **To confirm on the PC:**
`device-logs/run90-all-on/xe.log` should say `driverID:
QualcommProprietary` and an empty `vulkan_lib_path` in its config dump. If
it says `MesaTurnip`, this reading is wrong and the cause is in values I
cannot see.

Step 2 fixes all three: a config rebuilt from defaults gets the profile,
the driver and the touch choice put back.

### Defaults changed

| Setting | Where | Old | New | Evidence |
|---|---|---|---|---|
| `vulkan_async_skip_draws` | the default literal, `gpu/vulkan/vulkan_pipeline_cache.cc` (its own line; `claude/shader-first-run` owns the file) | `false` | `true` | The bundled config has shipped `true` since the import, so every device run but one ran with it; the one run with `false` (run 37b) gave 56 waited frames, 35 over 100 ms. The literal disagreeing with the shipped value already misled one session (handoff, run 37: "the bullet below wrongly assumed false"). Now a config that lacks the key behaves as the shipped one. |
| `vulkan_in_pass_resolve` (what the settings row shows when the key is absent) | `SettingsSchema.kt` | `true` | `false` | The core's literal is `false` and the bundled config does not list the key, so every device run had it off while the row showed On. The row now shows what runs. The core's behaviour is unchanged. |
| `framerate_limit` (same) | `SettingsSchema.kt` | `60` | `0` | The bundled config has `0` and so has the phone; only the row's fallback differed. |

`default_config.toml` itself is unchanged: for every key it lists it
already held the value the phone ran with.

### Flagged, not changed

| Setting | Bundled / literal | Why flagged |
|---|---|---|
| `defaults_date` | bundled `2026072714`, the newest update in the code is `2026080113` | With the older date, three settings are put back to their new default at every load if the player sets the old one: `guest_scheduler = false`, `clear_memory_page_state = true`, `resolve_check_number_format = false` (the handoff met the first: "cannot be tried from the config"). So the "Guest scheduler" row cannot switch it off. `2026080113` in the bundled file would fix it for new installs. Not changed: the phone runs with the old date and the new one is not proven; it is one line when wanted. |
| `host_perf_log`, `guest_scheduler_stats`, `shader_profiling` | off | On for the phone's measurement runs. Diagnostics, and they cost log volume: left off. |
| `vulkan_pipeline_creation_threads` | bundled `4`, literal `-1` (three quarters of the cores) | Every run used 4 from the bundled file. The literal is left as automatic for phones with fewer cores. |
| `headless`, `present_safe_area_x/y`, `vulkan_log_debug_messages`, `framerate_limit` literals | differ from the bundled values | The bundled file supplies the Android values; the literals are upstream's desktop defaults and nothing reads them on a phone unless the key is deleted from the config. Left alone. |
| `vulkan_in_pass_resolve` | literal `false` | XenDroid's schema said `true`, which suggests its author meant it on. It has never run on the phone. Worth one run on its own. |
| `readback_memexport` | no such setting in the core | The row writes a key nothing reads. Removed from the screen in step 3. |
| The last two days' switches | off | As instructed: the placeholder model, the look ahead, the skew bound, the translator's settings, the parking settings. |

## Step 2: a config that cannot be lost

### The one path

`xendroid.compose.ConfigFiles` (new, `x360/emulator-core/src/main/java`):
the text is written to a file beside the target, synced, and renamed over
it; the file it replaces is kept as `<name>.bak` (an empty file never
replaces a backup). Plain Java, no Android types.

`Emulator.Config.close_config_file()` (the Java side of the config handle)
now serialises the table to text and writes it through `ConfigFiles`. It
used to call the native writer, which opened the file with `std::ofstream`
(truncating it) and wrote in place. Every Kotlin writer closes its handle
through that method, so every one of them is on the path without being
changed.

`xendroid.compose.settings.ConfigGuard` (new, `x360/app`): what happens
when the config is missing, empty or does not parse. `ConfigStore.openLive`
and `openLiveSnapshot` call it before every open, and so does
`EmulatorHostActivity` before the core starts. In order:

1. The file is there, holds settings and parses: use it. If it changed
   outside the app (pushed over adb, written by the core) the backup is
   refreshed from it, unless it holds under four fifths of the settings the
   backup holds (a config cut short can still parse; it must not replace
   the fuller copy).
2. Else the backup parses: it is put back. Notice to the player.
3. Else the bundled defaults, and in the same write: the profile found on
   disc signed in, `vulkan_lib_path` pointed at the bundled Turnip again if
   it is unpacked, `show_touch_overlay` chosen again from whether a
   controller is attached. Notice to the player (not on a first install).

A file that held text and did not parse is kept as `<name>.unreadable`, so
a hand edit with a mistake in it is not lost.

**Profile.** Whenever the config is checked and
`logged_profile_slot_0_xuid` is empty, a profile on disc
(`content/<XUID>/FFFE07D1/00010000/<XUID>`) is signed in: the last one the
guard saw signed in if it is still there, else the one whose content was
written most recently. Not when the player removed the active profile in
the profiles screen (that choice is remembered). This also repairs a phone
already left signed out by an earlier build. `ProfileBootstrap` asks for
the check when profiles exist, where it used to return.

**New settings.** When the bundled `default_config.toml` changes (by its
text's hash) or a backup was restored, every setting it lists and the
config lacks is added with the bundled value. A setting the config has is
never changed. `defaults_date` is not added: giving one to a config
without it would let the core's own updates of old defaults rewrite the
player's values at the next load. Settings that exist only as a literal in
the core need no merge: the core uses the literal for an absent key.

**Telling the player.** `ConfigGuard.Notice`: a toast once when the next
game starts, and a card at the top of the Xbox 360 settings tab until it
is dismissed. Texts: "Xbox 360 settings were restored", "Xbox 360 settings
were reset", "Profile signed in again", and the same two for one game's
settings. The guard's own memory (the last profile, the notice, whether a
config has ever existed) is `config_guard.properties` in the app's
internal data, not beside the config, and is read from disc each time
because the 360 side runs in two processes.

### Every writer found, and what protects it now

| Writer | File | Before | Now |
|---|---|---|---|
| `ConfigStore.openLive`, missing file | global | copied the bundled defaults in, silently | the guard: backup first, defaults last, profile kept, notice |
| `ConfigStore.openLive`, parse error | global | overwrote the file with the bundled defaults, silently | the same; the unreadable file is kept aside |
| `ConfigStore.openLiveSnapshot` | global | as the first row | the same guard |
| `ConfigStore.openGameConfig`, parse error | per game | overwrote the file with nothing | its backup is put back if it parses, else an empty file; the unreadable file kept aside; notice |
| `ConfigHandle.closeFile` (settings screens, `SettingsRepository.flushAndClose` and `persistDriverPath`, `ProfileBootstrap`, `ProfileManagerViewModel.writeActiveXuid`, `BundledDriver`, `seedTouchOverlayDefault`, `EmulatorHostActivity.toggleTouchOverlay`, `InGameSettings`) | global and per game | native `ofstream`, truncate and write in place | `ConfigFiles.writeAtomic` with a backup |
| `GameSettingsRepository.flush` (rebuild) | per game | blanked the file in place, then rewrote it | built in memory, written whole with a backup |
| `GameSettingsRepository.flush` (no overrides left) | per game | deletes the file | unchanged: a deliberate reset; a missing per-game config is normal and is not restored |
| `TitleDefaults` (the bundled per-title configs and patches) | per game | `writeBytes` in place, only when absent or empty | written whole, same condition |
| `Application.ensure_init` | `default_config.toml` (the cached copy of the bundled defaults, not the config) | written in place | written whole; and the guard no longer reads this copy, it reads the asset |
| The core, `config::SaveConfig` (called from `kernel/xam/profile_manager.cc:652`, `gpu/graphics_system.cc:110` and `:322`, `ui/imgui_audio_dialog.cc:87`) | global | `fopen(..., "wb")`, in place | **not changed: not my file.** See below |
| The core, `config::SaveGameConfig` (from `gpu/command_processor.cc:485` and `:531`, and `SaveGameConfigSetting`) | per game | `std::ofstream`, in place | **not changed: not my file.** See below |
| The native JNI writer `j_close_config_file` (`x360/emulator-core/src/main/cpp/emulator.cpp`) | any | `std::ofstream`, in place | no longer called from Java; the function is still there |
| `DocumentsProvider` (a file manager, adb) | any | can delete, rename or overwrite anything in the app's data | cannot be prevented; the guard restores at the next open |

`SetupConfig` does not rewrite the config at boot on Android: the host
passes `--config=<file>`, and with the file present the core reads it and
returns before its `SaveConfig()`.

### For the desktop session to apply at merge (files outside my row)

`x360/emulator-core/src/main/cpp/xenia/src/xenia/config.cc`, both writers.
In `SaveConfig`, replace the block from `auto handle =
xe::filesystem::OpenFile(config_path, "wb");` to its `fclose` with: open
`config_path` with `.tmp` appended, `fwrite`, `fflush`, `fsync(fileno(handle))`,
`fclose`, then `std::filesystem::rename(tmp, config_path)`. In
`SaveGameConfig`, write to `game_config_path` with `.tmp` appended, flush,
close, rename. Until then a process killed inside either write leaves a
truncated file; the guard then restores the backup (which is at most one
change behind) and says so.

### What the build cannot verify

Everything. In particular, to try on the phone, each with `adb` and a look
at the Xbox 360 settings tab afterwards:

1. Delete `xenia-canary.config.toml`, open the settings: the card says
   "restored", the values are the tuned ones, the profile is signed in.
2. Delete it and `xenia-canary.config.toml.bak`: the card says "reset" and
   that the profile was signed in again; the driver row says "Turnip,
   bundled with the app"; Fable II shows its saves.
3. Put a line of rubbish in the file: restored, and
   `xenia-canary.config.toml.unreadable` holds the rubbish.
4. `logcat -s ConfigGuard ConfigFiles` shows what the guard did in each
   case.
5. Change a setting, leave the settings: the `.bak` holds the value before
   the change and no `.tmp` file is left.

Risks I can name: the config text now crosses JNI as a Java string on
every save (`NewStringUTF`), which the read-only snapshot path already
did; a value with characters outside the Basic Multilingual Plane (an
emoji in a path) has never been through it. The unit tests added
(`ConfigFilesTest`) are not compiled or run by the Actions build; the Java
helper itself was compiled and exercised on a desktop JVM in this session.

## Step 3: settings screens

### Whether a tab can leave another tab's rows drawn (read from the code)

`SettingsTabsFrame` (`android/app/.../boosted/SettingsTabs.kt`) wraps the
tab's content in `key(shown) { holder.SaveableStateProvider(shown.name) {
... } }`. My reading:

- `SaveableStateProvider` is built on `ReusableContent(key)`. When its key
  changes, Compose keeps the layout nodes and composes the new content into
  them. Global and Xbox are the same `Column` call in hakuX's
  `SettingsScreen.kt`, so before the fix one tab's nodes were handed to the
  other; the Xbox 360 tab is a different composable, which is why going via
  it redrew. That matches what run 78 showed.
- `key(shown)` outside it makes each tab a group of its own: on a switch the
  old group is removed with its nodes and the new one is composed from
  nothing. No node is reused, so there is nothing left to stay drawn. I
  believe the fix is sound. **It has not been seen on a screen.**
- Scroll position: each tab's `rememberScrollState` (Global, Xbox) and
  `rememberLazyListState` plus the open sections (Xbox 360) are saved by
  the holder when the tab's group is disposed and restored when it comes
  back, because the holder is outside the `key`. The holder itself and the
  chosen tab are `rememberSaveable`, so both survive a rotation and the
  process being killed.
- Returning from another screen: the 360 side's screens (profiles, install
  content, controls, about) are another activity, so the settings screen is
  not recomposed from nothing; hakuX's own (hard disk, controllers) pop the
  navigation stack, which restores saved state the same way a rotation
  does.
- One fault found and fixed: while the 360 core's settings load, the pane
  shows a short list, and it was given the same saved list state as the
  full list. A restored scroll position would have been cut down to that
  short list, so after the process was killed the Xbox 360 tab would come
  back at the top. The short list now has a state of its own
  (`HostSettingsPane.kt`).
- Left as it is: if the saved tab were Xbox 360 on a phone where the 360
  core is not available, the frame shows Global's key while the content
  lambda still tests the saved tab. It cannot be chosen there, so it can
  only arise from a backup restored to an older phone.

To check on the phone, with screenshots and not the text tree: Global to
Xbox and back several times, quickly; scroll Xbox halfway, go to Global,
come back; rotate on each tab; open Profiles from Xbox 360 and return.

### Every row, by the owner's rule

| Tab | Rows |
|---|---|
| Global | Games folders (each folder, Add a folder); Appearance: App theme; Updates: Check for updates; Logs: Export logs |
| Xbox | Controls (On-screen controls, Controllers); Display (uncapped, aspect, show frame rate, stutter monitor in debug builds); Console (hard disk, migrate, language, clock, console timing in debug builds); Network; Console video; Graphics; GPU driver |
| Xbox 360 | the notice card and the sign-in row when there is something to say; Console (Profiles, Install content); Files (All files access); Controls (On-screen controls, Controller buttons); GPU driver; Emulation settings (16 sections, 137 rows); Shader caches; About |

Found and fixed: **the Xbox 360 GPU driver was on its tab twice**, once as
the host's row under "GPU driver" and once inside the "Graphics driver
(Vulkan)" section (`Vulkan|vulkan_lib_path`). The section no longer lists
it (`HostSettingsPane`'s new `omit`, passed by `HostApp.Settings`).

One row that is on one tab but not wholly true to it: Global's "Export
logs" exports hakuX's logs only; the 360 core's are exported by "Export
session logs to Downloads" under Xbox 360, Logging. The owner named logs
as Global, so it stays; making the Global row export both is a small job
in `data/Logs.kt` (upstream) or a second step in `boosted/`.

### Added

- **A sign-in row**, at the top of the Xbox 360 tab, only while
  `logged_profile_slot_0_xuid` is empty: "No profile is signed in", why it
  matters, and a tap opens Profiles. With step 2 this shows only when no
  profile exists on disc or the player removed the active one. Read again
  on every return to the screen.
- **Shader caches**, at the foot of the Xbox 360 tab: one row per title
  that has shader files, largest first, with the title's name where the
  library has seen it, its size, and a tap to clear after a confirmation
  that says what clearing costs. Files counted: everything under
  `<cache root>/shaders` whose name starts with the title's ID
  (`shareable/<ID>.xsh`, `shareable/<ID>.<fsi|fbo>.vk.xpso`,
  `local/<ID>.vk.bin`). `core/ShaderCaches.kt`.
- **Descriptions**: every one of the schema's settings already had one
  (checked by script: 139 names, 139 descriptions, none empty), so none
  was added. One row was titled by its raw key
  (`value_convert_7e3_8888_reuse`) and is now "Decode reused HDR tiles
  before blending".
- **Removed**: "Readback memexport". The core has no setting of that name,
  so the row wrote a key nothing reads. The schema is 138 settings now;
  `SettingsSchemaTest`'s counts follow (not run by the build).

## Step 4: the look

The palette, shapes, type weights and the focus ring were already defined
once (`x360/app/.../ui/theme/HostTheme.kt`, `SharedLook`) and worn by both
sides. What still differed, and what was changed:

| What | Before | Now |
|---|---|---|
| Dimming behind in-game panels (pause menu, in-game settings, disc swap, message box, keyboard) | `Color.Black` at 60% in five places | `GameOverlayLook.Scrim`, one value: near-black with a trace of the accent's hue, 66% |
| The frame rate counter, Xbox 360 games | white at 70% on black at 28%, 10 sp, 4 dp corner | `GameOverlayLook`: white at 90% on black at 45%, 11 sp monospace, 6 dp corner, 6 by 2 dp padding |
| The frame rate counter, the stutter monitor and the "building shaders" line, Xbox games | white with a drop shadow on a square black box, padding in pixels | the same `GameOverlayLook` values through `boosted/Look.kt` `wearReadoutLook` (the monitor at 10 sp, the shader line at 14 sp) |
| The highlighted option of an in-game panel | a solid accent button with a pale border | a tonal fill with a 2 dp ring in the accent, which is what focus looks like everywhere else |
| Rows on the 360 side's own screens (per-game settings, profiles, key map, installed content, its own settings) | a divider line under every row | none, as hakuX's lists have none; the patches screen keeps its line between files |
| "Changed from the default" on the Xbox 360 tab | the accent | amber (`tertiary`), as a changed value is marked on the 360 side's other screens; the accent stays for what can be acted on |
| Spacing | literals | `LookSpacing` (gutter 16, section 24/4, gap 8, panel 20/12) used by the Xbox 360 tab's titles, notes and cards and the in-game panels |

No image, icon, font, logo or name was added. The words on screen are
"Xbox" and "Xbox 360" as before.

### Not done, said plainly

- The 360 side's screens keep XenDroid's structure: per-game settings is
  still an index of sections that open a second page, with XenDroid's row
  components (`SettingRows.kt`), not the rows of the Xbox 360 tab. Giving
  it the tab's rows means teaching `HostSettingRow` the override switch;
  not attempted blind.
- The type scale is Material's with heavier titles and labels; no sizes
  were changed. A larger scale for handheld distance is a judgement to
  make with the phone in hand.
- Dialog buttons and other Material components show focus as a 24% wash,
  not the ring. `Modifier.focusRing()` exists for them and is on the tabs
  only.
- "Large tiles" in the library and the Cocoon-style home were not touched:
  the library is upstream's screen and was not in this task.
- New strings are English only and in code, not resources.

### To see on a device before any of this is called done

1. The counter in an Xbox game and in an Xbox 360 game, side by side: same
   size, same pill, both clear of the corner. Over a bright scene and a
   dark one.
2. The pause menu of a 360 game with a controller: the ring moves with the
   D-pad and is unmistakable; the dimming does not look green.
3. Xbox 360 tab: the notice card (after test 1 of step 2), the sign-in row
   (remove the active profile), the shader cache rows with real sizes
   (the handoff measured 46 MB of pipeline cache, 1.2 MB of shaders and 52 KB of pipeline states on disc after the Banjo run), clearing
   one and the size going.
4. Profiles, per-game settings, key map without their divider lines: does
   the list still read as rows? If not, the lines go back in one place
   each.
5. Light theme: the amber "changed" label on white, the cards'
   containers.

## Upstream files touched, and why

hakuX's (under `android/app`):

- `NativeCoreActivity.kt`: three marked lines, one after each readout is
  made (`wearReadoutLook`), for the shared counter look.

XenDroid's (under `x360/`, all marked `hakuX-Boosted:`):

- `emulator-core/src/main/java/xendroid/emulator/Emulator.java`:
  `Config.close_config_file` writes through `ConfigFiles`.
- `emulator-core/src/main/java/xendroid/compose/Application.java`: the
  cached copy of the bundled defaults is written whole.
- `app/.../settings/ConfigStore.kt`: opens go through `ConfigGuard`; the
  two silent fallbacks to defaults are gone.
- `app/.../settings/GameSettingsRepository.kt`: the rebuild writes whole.
- `app/.../settings/SettingsSchema.kt`, `SettingDescriptions.kt`,
  `TouchOverlayDefault.kt`: two fallbacks, one row removed, one renamed,
  one accessor.
- `app/.../core/ProfileBootstrap.kt`, `TitleDefaults.kt`, `HostApp.kt`:
  the profile check, whole-file writes, the driver row once.
- `app/.../ui/profile/ProfileManagerViewModel.kt`: tells the guard the
  player's choice.
- `app/.../EmulatorHostActivity.kt`: the guard before the core starts, the
  toast.
- `app/.../data/GameMetadataCache.kt`: names by title ID.
- `app/.../FpsOverlay.kt`, `ui/pause/PauseMenuPanel.kt`,
  `ui/disc/DiscSwapPanel.kt`, `ui/messagebox/GuestMessageBoxPanel.kt`,
  `ui/keyboard/GuestKeyboardPanel.kt`, `ui/panel/GuestPanelOption.kt`: the
  shared in-game look.
- `app/.../ui/keymap/KeymapScreen.kt`, `ui/profile/ProfilesScreen.kt`,
  `ui/content/ContentManagerScreen.kt`,
  `ui/settings/PerGameSettingsScreen.kt`, `ui/settings/SettingsScreen.kt`:
  divider lines removed.
- The core, one line: `gpu/vulkan/vulkan_pipeline_cache.cc`, the default
  literal of `vulkan_async_skip_draws`.

New files (the fork's own): `emulator-core/.../compose/ConfigFiles.java`,
`app/.../settings/ConfigGuard.kt`, `app/.../core/ShaderCaches.kt`,
`app/src/test/.../ConfigFilesTest.kt`. Changed fork files:
`app/.../ui/settings/HostSettingsPane.kt`, `ui/theme/HostTheme.kt`,
`android/app/.../boosted/Look.kt`.

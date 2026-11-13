# `claude/defaults-config`: defaults, a config that cannot be lost, the settings screens, the look

Cut from `claude/integration-1113` at `8e03766`.

**Nothing here was seen on a device.** A cloud session cannot run the app.
Every statement about behaviour below is what the code is meant to do. The
only check is the GitHub Actions build of this branch.

## Where it stands

- Done: step 1 (defaults), step 2 (the config store).
- In progress: step 3 (settings screens).
- Next: step 4 (the look).

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

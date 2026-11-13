# `claude/defaults-config`: defaults, a config that cannot be lost, the settings screens, the look

Cut from `claude/integration-1113` at `8e03766`.

**Nothing here was seen on a device.** A cloud session cannot run the app.
Every statement about behaviour below is what the code is meant to do. The
only check is the GitHub Actions build of this branch.

## Where it stands

- Done: step 1 (defaults).
- In progress: step 2 (the config store).
- Next: step 3 (settings screens), step 4 (the look).

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

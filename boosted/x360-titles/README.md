# Per-title defaults for the Xbox 360 side

What a title needs to run at its best, shipped with the app instead of left
for each user to find. The 360 side's counterpart of hakuX's `mods/`.

- `config/<TITLEID>.config.toml`: a per-game config (the core's
  `config::LoadGameConfig` format: keys under their cvar's category, e.g.
  `[GPU]`, `[Vulkan]`). Packed as `assets/title-defaults/` and written to
  `<storage_root>/config/` when the title has no per-game config of the
  user's (`x360/app/src/main/java/xendroid/compose/core/TitleDefaults.kt`).
  Only settings that are right for the title on every supported device belong
  here; say in the file where each comes from.
- `patches/<TITLEID> - <name>.patch.toml`: title patches in xenia's patch
  format, packed next to xenia-canary's (`x360/patches/xenia-canary`) as
  `assets/game-patches/`, so the title's patches screen lists them. A patch
  here with `is_enabled = true` is also installed to
  `<storage_root>/patches/` before the title first boots (the bundled
  xenia-canary patches all ship disabled and are installed only when the
  user turns one on). Only fixes belong here enabled: a patch that changes
  how the game looks or plays ships disabled, for the user to choose.

`python3 boosted/tools/title-info.py <TITLEID or name>` prints what is
already known about a title: its entry in Xenia's compatibility database,
xenia-canary's patches for it, and what is shipped here.

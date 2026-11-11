#!/usr/bin/env python3
"""What is already known about an Xbox 360 title, from what the tree holds.

    python3 boosted/tools/title-info.py 4D5307F1
    python3 boosted/tools/title-info.py "fable"

Pools, for each title matching the ID or the name:
- Xenia's compatibility database as XenDroid vendors it (state and labels;
  x360/.../assets/game-compatibility/master.json);
- xenia-canary's patches for it (x360/patches/xenia-canary, a submodule:
  `git submodule update --init x360/patches/xenia-canary`), by executable
  hash, with each patch's name;
- what hakuX-Boosted ships for it (boosted/x360-titles);
- XenDroid's own notes (x360/GAME_COMPAT.md), if they name it.

The starting point for a title's entry in boosted/x360-titles: labels such
as requires_render_target_rov_fsi or gpu-drawing-missing say what to expect,
and a "Disable MSAA" patch is the first thing to try for Turnip's tiled
rendering (boosted/docs/FABLE2.md).
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMPAT = ROOT / "x360/emulator-core/src/main/cpp/xenia/assets/game-compatibility/master.json"
PATCH_DIRS = [
    ROOT / "x360/patches/xenia-canary/patches",
    ROOT / "boosted/x360-titles/patches",
]
DEFAULTS = ROOT / "boosted/x360-titles/config"
NOTES = ROOT / "x360/GAME_COMPAT.md"


def patch_files(title_id):
    for directory in PATCH_DIRS:
        if directory.is_dir():
            for path in sorted(directory.glob("*.patch.toml")):
                if path.name[:8].upper() == title_id:
                    yield path


def describe_patch_file(path):
    text = path.read_text(errors="replace")
    hashes = re.findall(r'(?m)^hash\s*=\s*(.+)$', text)
    print(f"  {path.relative_to(ROOT)}")
    if hashes:
        print(f"    executable hash: {hashes[0].split('#')[0].strip()}")
    for block in re.split(r"(?m)^\[\[patch\]\]", text)[1:]:
        name = re.search(r'(?m)^\s*name\s*=\s*"([^"]*)"', block)
        enabled = re.search(r"(?m)^\s*is_enabled\s*=\s*(\w+)", block)
        if name:
            on = " (ships enabled)" if enabled and enabled.group(1) == "true" else ""
            print(f"    - {name.group(1)}{on}")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    query = sys.argv[1].strip()
    entries = json.loads(COMPAT.read_text())
    if re.fullmatch(r"[0-9A-Fa-f]{8}", query):
        ids = {query.upper()}
    else:
        ids = {e["id"].upper() for e in entries if query.lower() in e.get("title", "").lower()}
        # titles the database lacks but a patch file names
        for directory in PATCH_DIRS:
            if directory.is_dir():
                for path in directory.glob("*.patch.toml"):
                    if query.lower() in path.name.lower():
                        ids.add(path.name[:8].upper())
    if not ids:
        sys.exit(f"nothing known about {query!r}")
    notes = NOTES.read_text(errors="replace") if NOTES.is_file() else ""
    for title_id in sorted(ids):
        print(f"== {title_id}")
        found = [e for e in entries if e["id"].upper() == title_id]
        for e in found:
            labels = e["labels"]["state"] + e["labels"]["others"]
            print(f"  compatibility: {e['title']}: {e['state']}"
                  f"{' [' + ', '.join(labels) + ']' if labels else ''}")
            print(f"    {e['url']} (updated {e['updated'][:10]})")
        if not found:
            print("  compatibility: not in the vendored database")
        files = list(patch_files(title_id))
        print(f"  patches: {len(files)} file(s)" if files else "  patches: none bundled")
        for path in files:
            describe_patch_file(path)
        default = DEFAULTS / f"{title_id}.config.toml"
        if default.is_file():
            print(f"  shipped config: {default.relative_to(ROOT)}")
        if title_id in notes:
            print("  x360/GAME_COMPAT.md has notes on this title")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Rewrites the x360 part of the root .gitmodules from x360/.gitmodules.

XenDroid (the x360/ subtree) lists its third-party submodules in its own
.gitmodules, with paths relative to its root. Git only reads the root
.gitmodules, so the same entries are kept there with the x360/ prefix. Run
this after `git subtree pull --prefix=x360 ...` when XenDroid's submodule
list changed:

    python3 boosted/tools/sync-x360-gitmodules.py

hakuX's own entries (everything above the marker line) are left as they are.
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
MARKER = "# The Xbox 360 side (x360/, XenDroid): its submodules"


def main() -> None:
    source = (ROOT / "x360" / ".gitmodules").read_text()
    entries = []
    for block in re.split(r"(?m)^(?=\[submodule )", source):
        if not block.strip():
            continue
        name = re.match(r'\[submodule "([^"]+)"\]', block).group(1)
        block = block.replace(f'[submodule "{name}"]', f'[submodule "x360/{name}"]')
        block = re.sub(
            r"(?m)^(\s*path\s*=\s*)(.+)$",
            lambda m: m.group(1) + "x360/" + m.group(2).strip(),
            block,
        )
        entries.append(block.rstrip() + "\n")

    root_file = ROOT / ".gitmodules"
    own = root_file.read_text().split(MARKER)[0].rstrip() + "\n"
    header = (
        f"\n{MARKER}, as x360/.gitmodules\n"
        "# lists them, under x360/. Regenerate with boosted/tools/sync-x360-gitmodules.py\n"
        "# after a subtree pull.\n"
    )
    root_file.write_text(own + header + "".join(entries))
    print(f"{len(entries)} x360 submodules written to .gitmodules")


if __name__ == "__main__":
    main()

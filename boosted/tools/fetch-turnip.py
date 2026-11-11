#!/usr/bin/env python3
"""hakuX-Boosted: the Turnip packages the app ships with.

The app bundles a Turnip (Mesa's open Adreno Vulkan driver) package and uses
it for the Xbox 360 core on Snapdragon phones unless the user chooses another
driver (x360/app/.../core/BundledDriver.kt). The packages are StevenMXZ's
adrenotools builds (https://github.com/StevenMXZ/Adreno-Tools-Drivers); which
release, and each file's SHA-256, is pinned in boosted/drivers/turnip.lock.json.

  fetch-turnip.py            download what the lock names into
                             boosted/drivers/cache (checked against the lock),
                             for the Gradle build to pack
  fetch-turnip.py --check    say whether upstream has a newer Turnip release
  fetch-turnip.py --update   move the lock to upstream's newest Turnip release
                             (the Turnip sync workflow runs this)

Only the standard library. A package is taken only if it is what the app can
load: a ZIP with a meta.json that names a library the ZIP holds.
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys
import urllib.request
import zipfile

SOURCE = "StevenMXZ/Adreno-Tools-Drivers"
HERE = os.path.dirname(os.path.abspath(__file__))
DRIVERS = os.path.normpath(os.path.join(HERE, "..", "drivers"))
LOCK = os.path.join(DRIVERS, "turnip.lock.json")
CACHE = os.path.join(DRIVERS, "cache")

# One package per Adreno generation (the first digit of the GPU's model number),
# by the release asset's name. The plain upstream-Mesa builds: not the
# "Patched" ones (unmerged patches, "use unpatched if you have issues") and not
# the One UI ones.
PACKAGES = [
    ("8", re.compile(r"^Turnip_Gen8_V\d+\.zip$")),
    ("7", re.compile(r"^Turnip_v[\d.]+-R\d+\.zip$")),
]
MAX_SIZE = 64 * 1024 * 1024


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "hakuX-Boosted-build"})
    token = os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read(MAX_SIZE + 1)
    if len(data) > MAX_SIZE:
        raise SystemExit(f"{url}: larger than {MAX_SIZE} bytes")
    return data


def inspect(data, what):
    """The package's meta.json, once it is one the app can load."""
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = z.namelist()
            meta = json.loads(z.read("meta.json"))
    except (zipfile.BadZipFile, KeyError, ValueError) as e:
        raise SystemExit(f"{what}: not an adrenotools package ({e})")
    lib = meta.get("libraryName")
    if not lib or lib not in names or "/" in lib or not lib.endswith(".so"):
        raise SystemExit(f"{what}: meta.json names no library of the package")
    return meta


def newest_release():
    """Upstream's newest release that has every package (its Qualcomm driver
    releases have none)."""
    releases = json.loads(get(f"https://api.github.com/repos/{SOURCE}/releases?per_page=30"))
    for rel in releases:
        if rel.get("draft") or rel.get("prerelease"):
            continue
        found = []
        for adreno, pattern in PACKAGES:
            asset = next((a for a in rel["assets"] if pattern.match(a["name"])), None)
            if asset:
                found.append((adreno, asset))
        if len(found) == len(PACKAGES):
            return rel, found
    raise SystemExit(f"no release of {SOURCE} has all of the Turnip packages")


def read_lock():
    with open(LOCK, encoding="utf-8") as f:
        return json.load(f)


def update():
    rel, found = newest_release()
    old = read_lock() if os.path.exists(LOCK) else {}
    if old.get("tag") == rel["tag_name"]:
        print(f"up to date: {rel['tag_name']}")
        return
    packages = []
    for adreno, asset in found:
        data = get(asset["browser_download_url"])
        meta = inspect(data, asset["name"])
        packages.append({
            "adreno": adreno,
            "file": asset["name"],
            "url": asset["browser_download_url"],
            "sha256": hashlib.sha256(data).hexdigest(),
            "name": meta.get("name", asset["name"]),
            "driverVersion": meta.get("driverVersion", ""),
            "library": meta["libraryName"],
        })
    lock = {
        "source": SOURCE,
        "tag": rel["tag_name"],
        "title": rel.get("name") or rel["tag_name"],
        "published": rel.get("published_at", ""),
        "packages": packages,
    }
    os.makedirs(DRIVERS, exist_ok=True)
    with open(LOCK, "w", encoding="utf-8", newline="\n") as f:
        json.dump(lock, f, indent=2)
        f.write("\n")
    print(f"{old.get('tag', 'nothing')} -> {rel['tag_name']} ({lock['title']})")


def check():
    rel, _ = newest_release()
    lock = read_lock()
    if lock["tag"] == rel["tag_name"]:
        print(f"up to date: {lock['tag']}")
    else:
        # a GitHub Actions annotation, and readable by hand
        print(f"::notice::Turnip {rel['tag_name']} ({rel.get('name')}) is out, the app bundles "
              f"{lock['tag']}: run the Turnip sync workflow, or fetch-turnip.py --update")


def fetch():
    lock = read_lock()
    os.makedirs(CACHE, exist_ok=True)
    wanted = set()
    for p in lock["packages"]:
        if os.path.basename(p["file"]) != p["file"] or not p["file"].endswith(".zip"):
            raise SystemExit(f"{p['file']}: not a file name")
        wanted.add(p["file"])
        path = os.path.join(CACHE, p["file"])
        data = None
        if os.path.exists(path):
            with open(path, "rb") as f:
                data = f.read()
            if hashlib.sha256(data).hexdigest() != p["sha256"]:
                data = None
        if data is None:
            data = get(p["url"])
            digest = hashlib.sha256(data).hexdigest()
            if digest != p["sha256"]:
                raise SystemExit(f"{p['file']}: SHA-256 {digest}, the lock says {p['sha256']}")
            inspect(data, p["file"])
            with open(path, "wb") as f:
                f.write(data)
        print(f"{p['file']}: {p['name']} ({len(data)} bytes)")
    for name in os.listdir(CACHE):
        if name.endswith(".zip") and name not in wanted:
            os.remove(os.path.join(CACHE, name))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true")
    g.add_argument("--update", action="store_true")
    args = ap.parse_args()
    if args.update:
        update()
    elif args.check:
        check()
    else:
        fetch()


if __name__ == "__main__":
    sys.exit(main())

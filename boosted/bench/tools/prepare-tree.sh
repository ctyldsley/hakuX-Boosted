#!/usr/bin/env bash
# hakuX-Boosted bench: lays out the sources of one branch for the bench to
# build against, without a second checkout.
#
#   prepare-tree.sh <git ref> <destination directory>
#
# Run from a checkout whose submodules in tools/submodules.txt are present
# (the harness's). The destination gets the ref's own copy of
# x360/emulator-core/src/main/cpp, and each of those submodules as a link to
# the checkout's: so a branch is measured with its own core sources and the
# harness's third-party libraries. A branch that moves one of those
# submodules is therefore measured with the wrong version of it; the script
# says so when it sees a different commit recorded.
set -euo pipefail
ref="${1:?git ref}"
dest="${2:?destination}"
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
core="x360/emulator-core/src/main/cpp"
tp="$core/xenia/third_party"

rm -rf "$dest"
mkdir -p "$dest"
git -C "$repo" archive "$ref" "$core" | tar -x -C "$dest"

while read -r sub; do
  case "$sub" in ''|\#*) continue ;; esac
  if [ ! -e "$repo/$tp/$sub/.git" ]; then
    echo "prepare-tree: submodule $sub is not checked out in $repo" >&2
    exit 1
  fi
  mine="$(git -C "$repo" rev-parse "HEAD:$tp/$sub" 2>/dev/null || true)"
  theirs="$(git -C "$repo" rev-parse "$ref:$tp/$sub" 2>/dev/null || true)"
  if [ -n "$theirs" ] && [ "$mine" != "$theirs" ]; then
    echo "::warning::$ref records another commit of $sub ($theirs) than the harness's ($mine); measured with the harness's"
  fi
  rm -rf "$dest/$tp/$sub"
  mkdir -p "$(dirname "$dest/$tp/$sub")"
  ln -s "$repo/$tp/$sub" "$dest/$tp/$sub"
done < "$here/submodules.txt"
echo "prepare-tree: $ref at $(git -C "$repo" rev-parse --short "$ref") in $dest"

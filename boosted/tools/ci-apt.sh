#!/usr/bin/env bash
# hakuX-Boosted: installs the build's Ubuntu packages on a GitHub runner,
# without depending on the runner's own package mirror.
#
# The runner images point apt at azure.archive.ubuntu.com. On 2026-11-11 that
# mirror stopped answering: `apt-get update` failed (exit code 100) and, with
# retries, an install took 47 minutes to give up. Mirrors that do not answer
# are skipped up front, and each attempt is short (20 s a connection, 4
# minutes in all).
#
#   ci-apt.sh <package> ...
set -u
if [ "$#" -eq 0 ]; then echo "usage: ci-apt.sh <package> ..." >&2; exit 2; fi

opts=(-o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20 -o Acquire::Retries=1 -o DPkg::Lock::Timeout=60)
release=$(. /etc/os-release && echo "$VERSION_CODENAME")

# The mirrors in order of preference, the runner's own first. Each is asked
# for its release file before apt is pointed at it (8 s): a mirror that does
# not answer costs seconds, not the 5 minutes apt takes to give up on it
# (the two fallbacks used to be reached only after that, 14 minutes a job).
own=$(grep -rhoE 'https?://[a-z0-9.-]*archive\.ubuntu\.com/ubuntu' /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list /etc/apt/apt-mirrors.txt 2>/dev/null | head -1)
mirrors=()
for mirror in "$own" "http://archive.ubuntu.com/ubuntu" "http://mirrors.edge.kernel.org/ubuntu"; do
  [ -n "$mirror" ] || continue
  if curl -fsS -m 8 -o /dev/null "$mirror/dists/$release/InRelease"; then
    mirrors+=("$mirror")
  else
    echo "::warning::the package mirror $mirror does not answer, skipped"
  fi
done

for mirror in "${mirrors[@]}"; do
  if [ "$mirror" != "$own" ]; then
    for f in /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list /etc/apt/apt-mirrors.txt; do
      [ -f "$f" ] && sudo sed -i -E "s#https?://[a-z0-9.-]*archive\.ubuntu\.com/ubuntu/?#$mirror/#g; s#https?://mirrors\.edge\.kernel\.org/ubuntu/?#$mirror/#g" "$f"
    done
  fi
  # the image's other sources (Microsoft's, GitHub's) may fail without harm:
  # only the install decides
  sudo timeout 240 apt-get "${opts[@]}" update || true
  if sudo timeout 240 apt-get "${opts[@]}" install -y --no-install-recommends "$@"; then
    exit 0
  fi
  echo "::warning::installing from $mirror failed"
done
echo "::error::no package mirror served: $*" >&2
exit 1

#!/usr/bin/env bash
# hakuX-Boosted: installs the build's Ubuntu packages on a GitHub runner,
# without depending on the runner's own package mirror.
#
# The runner images point apt at azure.archive.ubuntu.com. On 2026-11-11 that
# mirror stopped answering: `apt-get update` failed (exit code 100) and, with
# retries, an install took 47 minutes to give up. Each attempt here is short
# (20 s a connection, 5 minutes in all), and a failed one moves on to the
# next mirror.
#
#   ci-apt.sh <package> ...
set -u
if [ "$#" -eq 0 ]; then echo "usage: ci-apt.sh <package> ..." >&2; exit 2; fi

opts=(-o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20 -o Acquire::Retries=1 -o DPkg::Lock::Timeout=60)
mirrors=("" "http://archive.ubuntu.com/ubuntu" "http://mirrors.edge.kernel.org/ubuntu")

for mirror in "${mirrors[@]}"; do
  if [ -n "$mirror" ]; then
    echo "::warning::the package mirror failed, trying $mirror"
    for f in /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list /etc/apt/apt-mirrors.txt; do
      [ -f "$f" ] && sudo sed -i -E "s#https?://[a-z0-9.-]*archive\.ubuntu\.com/ubuntu/?#$mirror/#g; s#https?://mirrors\.edge\.kernel\.org/ubuntu/?#$mirror/#g" "$f"
    done
  fi
  # the image's other sources (Microsoft's, GitHub's) may fail without harm:
  # only the install decides
  sudo timeout 300 apt-get "${opts[@]}" update || true
  if sudo timeout 300 apt-get "${opts[@]}" install -y --no-install-recommends "$@"; then
    exit 0
  fi
done
echo "::error::no package mirror served: $*" >&2
exit 1

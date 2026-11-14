#!/usr/bin/env bash
# hakuX-Boosted: runs the instruction tests once with every translator
# setting at its default (the reference), then again for each line of
# matrix.txt, with that line's settings. Each later run must pass the tests'
# own assertions, must print no self-check line, and must leave every test in
# the state the reference run left it in.
#
#   run-matrix.sh <runner binary> <directory of .bin/.map files>
#
# Environment: JOBS and WRAPPER as for run-tests.sh; MATRIX to name another
# matrix file; ONLY, a regular expression over the labels in it.
set -uo pipefail
runner=$1
bins=$2
here=$(cd "$(dirname "$0")" && pwd)
matrix=${MATRIX:-$here/matrix.txt}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=()

echo "::group::reference: every setting at its default"
if ! LABEL=reference STATE_OUT="$work/reference.state" \
    "$here/run-tests.sh" "$runner" "$bins"; then
  failed+=("reference")
fi
echo "::endgroup::"

# The reference a second time: what differs between two runs with the same
# settings is not the translator's doing, and must be listed in
# nondeterministic.txt for the comparisons below to mean anything.
echo "::group::reference again (the comparison must be stable)"
if ! LABEL=reference-again STATE_REF="$work/reference.state" SUITES="${STABLE_SUITES:-^(instr_m|seq_)}" \
    "$here/run-tests.sh" "$runner" "$bins"; then
  failed+=("reference-again")
fi
echo "::endgroup::"

while IFS=';' read -r label suites flags; do
  case $label in ''|'#'*) continue ;; esac
  label=$(echo "$label" | xargs)
  suites=$(echo "$suites" | xargs)
  if [ -n "${ONLY:-}" ] && ! echo "$label" | grep -qE "$ONLY"; then continue; fi
  echo "::group::$label"
  # A line whose flags end in "#ignore=<field>" leaves that field of the
  # state out of the comparison (see STATE_IGNORE in run-tests.sh).
  ignore=""
  case "$flags" in *'#ignore='*) ignore=${flags##*#ignore=}; flags=${flags%%#ignore=*} ;; esac
  # shellcheck disable=SC2086
  if ! LABEL="$label" STATE_REF="$work/reference.state" SUITES="$suites" STATE_IGNORE="$ignore" \
      "$here/run-tests.sh" "$runner" "$bins" $flags; then
    failed+=("$label")
  fi
  echo "::endgroup::"
done < "$matrix"

if [ "${#failed[@]}" -gt 0 ]; then
  echo "::error::failed: ${failed[*]}"
  exit 1
fi
echo "every configuration passed"

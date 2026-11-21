#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
exe=${1:-"$root/build/default/mam"}
json=${2:-"$root/tests/fixtures/scheduler-parity.json"}
dur=${3:-2}
sr=${4:-48000}
seed=${5:-42}

# Each invocation owns its scratch directory; never overwrite caller audio.
work=$(mktemp -d "${TMPDIR:-/tmp}/mam-parity.XXXXXX")
trap 'rm -rf "$work"' EXIT

run() {
  local mode=$1
  shift
  local wav="$work/$mode.wav" log="$work/$mode.log" hash
  if ! "$exe" --rack "$json" --wav "$wav" --sr "$sr" --duration "$dur" \
      "$@" --sha1 --random-seed "$seed" >"$log" 2>&1; then
    cat "$log" >&2
    echo "ERROR: $mode render failed" >&2
    return 1
  fi
  # Hashes arrive on stderr. Missing, duplicate or malformed hashes must fail.
  hash=$(sed -n 's/^SHA1(samples): //p' "$log")
  if [[ ! "$hash" =~ ^[0-9a-f]{40}$ ]]; then
    cat "$log" >&2
    echo "ERROR: $mode did not emit exactly one valid sample hash" >&2
    return 1
  fi
  if [[ ! -s "$wav" ]]; then
    echo "ERROR: $mode did not create a nonempty audio file" >&2
    return 1
  fi
  printf '%s\n' "$hash"
}

sha_ref=$(run baseline --offline-scheduler baseline --offline-block 256)
sha_base=$(run topo-serial --topo-scheduler topo --topo-threads 0 --topo-offline-blocks 256 --cpu-stats --cpu-stats-per-node)
sha_par=$(run topo-parallel --topo-scheduler topo --topo-threads 4 --topo-offline-blocks 256 --topo-trace "$work/trace.json" --cpu-stats)

echo "REF: $sha_ref"
echo "SER: $sha_base"
echo "PAR: $sha_par"

if [[ "$sha_ref" == "$sha_base" && "$sha_ref" == "$sha_par" ]]; then
  echo "OK: parity confirmed"
else
  echo "ERROR: parity mismatch" >&2; exit 1
fi

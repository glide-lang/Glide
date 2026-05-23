#!/usr/bin/env bash
# Concurrency micro-bench runner. Builds each bench with the current
# glide, runs RUNS times back-to-back, captures stdout. Use this BEFORE
# starting an SM sprint to record a baseline, then AFTER to compare.
#
# Output is markdown so it can be pasted into the relevant memory file
# or PR description. Each bench section shows all RUNS lines so the
# reader can eyeball variance vs the stripped median.

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

GLIDE="${GLIDE:-./glide.exe}"
RUNS="${RUNS:-5}"

BENCHES=(
  spawn_only_glide
  spawn_1m
  send_only_glide
  chan_pure_glide
  throughput_glide
  park_unpark
)

cat <<EOF
# Concurrency micro-bench

- Runs: $RUNS per bench, in order, no rebuild between runs.
- Glide: \`$GLIDE\` ($(file "$GLIDE" 2>/dev/null | cut -d: -f2 | sed 's/^ //'))
- Host: $(uname -mrs 2>/dev/null || ver 2>/dev/null || echo unknown)
- Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)

EOF

for name in "${BENCHES[@]}"; do
  src="bench/$name.glide"
  exe="bench/$name.exe"
  if [[ ! -f "$src" ]]; then
    echo "## $name"; echo "_missing source: $src_"; echo; continue
  fi
  "$GLIDE" build "$src" -o "$exe" >/dev/null 2>&1 || {
    echo "## $name"; echo "_build failed_"; echo; continue
  }
  echo "## $name"
  echo
  printf '```\n'
  for i in $(seq 1 "$RUNS"); do
    "./$exe" 2>&1 || true
  done
  printf '```\n\n'
done

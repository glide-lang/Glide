#!/usr/bin/env bash
# Concurrency micro-bench runner with median + p99 + range. Builds each
# bench with the current glide, runs RUNS times back-to-back, captures
# the relevant numeric token on each line as the metric. Reports the
# raw lines + median + p99 + range for each bench.
#
# Use BEFORE starting an SM sprint to record a baseline, then AFTER to
# compare. Differences smaller than the IQR shown here are noise.
#
# Knobs:
#   GLIDE       - which glide to use (default ./glide.exe)
#   RUNS        - per-bench run count (default 10)
#   PIN         - if 1, set GLIDE_PIN_WORKERS=1 in the bench env so
#                 worker threads pin to their CPU. Eliminates the OS
#                 scheduler migration noise. Benches that are sensitive
#                 to bimodal results should be re-run with PIN=1 to
#                 compare.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

GLIDE="${GLIDE:-./glide.exe}"
RUNS="${RUNS:-10}"
PIN="${PIN:-0}"

# Benches that DON'T need a clean shutdown (no Defender false positives
# on multi-spawn pattern). massive_concurrency is intentionally left
# out of the default list because Windows Defender flags the
# "spawn 100k concurrents on a single port" shape and corrupts the
# link output. Run massive separately on Linux for the bytes-per-task
# number.
BENCHES=(
  spawn_only_glide
  spawn_1m
  send_only_glide
  chan_pure_glide
  throughput_glide
  park_unpark
  park_unpark_coro
  park_unpark_coro_clean
)

# Compute median + p99 + min + max of one-number-per-line stdin.
# Output: "<median> <p99> <min> <max>", "- - - -" on empty input.
stats() {
  awk '
    { vals[NR] = $1 }
    END {
      n = NR
      if (n == 0) { print "- - - -"; exit }
      for (i = 2; i <= n; i++) {
        key = vals[i]; j = i - 1
        while (j >= 1 && vals[j] > key) { vals[j+1] = vals[j]; j-- }
        vals[j+1] = key
      }
      mid = int((n + 1) / 2)
      p99idx = int(n * 0.99 + 0.5)
      if (p99idx < 1) p99idx = 1
      if (p99idx > n) p99idx = n
      printf "%s %s %s %s\n", vals[mid], vals[p99idx], vals[1], vals[n]
    }
  '
}

# Extract the numeric metric on each line. Glide benches print things
# like "X N: ... avg_ns/cycle: 57" or "spawn+drain 1000000 : 98 ms".
# Prefer the second-to-last token if it's numeric (handles trailing
# unit suffixes), fall back to last token. Skip lines with no number.
extract() {
  awk '{
    n = NF
    if (n == 0) next
    if (n >= 2 && $(n-1) ~ /^-?[0-9]+(\.[0-9]+)?$/) print $(n-1)
    else if ($n ~ /^-?[0-9]+(\.[0-9]+)?$/) print $n
  }'
}

env_prefix=""
if [[ "$PIN" == "1" ]]; then
  env_prefix="GLIDE_PIN_WORKERS=1 "
fi

cat <<EOF
# Concurrency micro-bench

- Runs: $RUNS per bench, in order, no rebuild between runs.
- Pin workers: $([ "$PIN" = 1 ] && echo "yes (GLIDE_PIN_WORKERS=1)" || echo "no")
- Glide: \`$GLIDE\` ($(file "$GLIDE" 2>/dev/null | cut -d: -f2 | sed 's/^ //'))
- Host: $(uname -mrs 2>/dev/null || ver 2>/dev/null || echo unknown)
- Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)

| Bench | median | p99 | min | max |
| --- | --- | --- | --- | --- |
EOF

# Collect everything first, then emit the summary table and raw runs.
declare -A out
declare -A row
for name in "${BENCHES[@]}"; do
  src="bench/$name.glide"
  exe="bench/$name.exe"
  if [[ ! -f "$src" ]]; then
    out[$name]="_missing source_"
    row[$name]="- | - | - | -"
    continue
  fi
  buildlog=$(mktemp)
  "$GLIDE" build "$src" -o "$exe" >"$buildlog" 2>&1
  rc=$?
  rm -f "$buildlog"
  if [[ $rc -ne 0 ]]; then
    out[$name]="_build failed (rc=$rc)_"
    row[$name]="- | - | - | -"
    continue
  fi
  results=""
  for i in $(seq 1 "$RUNS"); do
    line=$(env $env_prefix "./$exe" 2>&1 || true)
    results+="$line"$'\n'
  done
  out[$name]="$results"
  row[$name]=$(echo "$results" | extract | stats | awk '{ printf "%s | %s | %s | %s", $1, $2, $3, $4 }')
  echo "| $name | ${row[$name]} |"
done

echo ""
echo "## Raw runs"
echo ""
for name in "${BENCHES[@]}"; do
  echo "### $name"
  echo ""
  printf '```\n'
  echo "${out[$name]}"
  printf '```\n\n'
done

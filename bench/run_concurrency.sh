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
  # glide.exe writes diagnostics + spinner to stdout during build,
  # then internally `__glide_redirect_to(tmp_c)` re-points fd 1 at
  # the emitted .c file. If our parent shell hands glide.exe a closed
  # / null stdout, the spinner status text gets spliced into the .c
  # file. Pipe build output through a buffer that gets discarded only
  # after build returns, so glide.exe sees a writable fd 1 during the
  # emit phase.
  buildlog=$(mktemp)
  "$GLIDE" build "$src" -o "$exe" >"$buildlog" 2>&1
  rc=$?
  rm -f "$buildlog"
  if [[ $rc -ne 0 ]]; then
    echo "## $name"; echo "_build failed (rc=$rc)_"; echo; continue
  fi
  echo "## $name"
  echo
  printf '```\n'
  for i in $(seq 1 "$RUNS"); do
    "./$exe" 2>&1 || true
  done
  printf '```\n\n'
done

#!/usr/bin/env bash
# Full Glide regression suite, one entry point:
#   1. unit tests        -> glide test tests        (compiler / stdlib, *_test.glide)
#   2. LSP smoke         -> python tools/lsp_smoke.py
#   3. end-to-end smokes -> tools/smoke/*.sh         (build + run real programs)
#
# Uses $GLIDE if set, else ~/.glide/bin/glide.exe. Run from anywhere:
#   bash tools/test_all.sh
set -u
cd "$(cd "$(dirname "$0")/.." && pwd)"
GLIDE="${GLIDE:-$HOME/.glide/bin/glide.exe}"
export GLIDE
PY="$(command -v python3 || command -v python || true)"
[ -n "$PY" ] || { echo "python not found on PATH" >&2; exit 1; }

fails=0
run() {
    local name="$1"; shift
    echo; echo "=================== $name ==================="
    if "$@"; then echo ">> ok"; else echo ">> FAILED ($name)"; fails=$((fails + 1)); fi
}

run "unit tests (glide test tests)"  "$GLIDE" test tests
run "LSP smoke (lsp_smoke.py)"        "$PY" tools/lsp_smoke.py
for s in tools/smoke/*.sh; do
    run "smoke: $(basename "$s")" bash "$s"
done

echo
if [ "$fails" -eq 0 ]; then
    echo "ALL SUITES GREEN"
else
    echo "$fails SUITE(S) FAILED"
fi
exit "$fails"

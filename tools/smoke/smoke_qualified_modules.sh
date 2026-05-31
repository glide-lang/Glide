#!/usr/bin/env bash
# Smoke: module-qualified types across a multi-file project (sibling modules +
# a submodule), run as a real `glide test`. Guards the typer canonicalization
# (qualified return / param / field / container / match-arm / submodule) for
# PROJECT-LOCAL modules — coverage the stdlib-based tests/qualified_type_test
# can't give, since a unit test can't define its own importable module.
set -u
GLIDE="${GLIDE:-$HOME/.glide/bin/glide.exe}"
DIR="$(cd "$(dirname "$0")" && pwd)"
FIX="$DIR/fixtures/qualified"
fail() { echo "FAIL: $*" >&2; exit 1; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/src/a" "$T/tests"
printf 'let manifest: Package = Package { name:"t", version:"0.1.0", bin:"src/main.glide", deps: vec_of() };\n' > "$T/glide.glide"
cp "$FIX/src/foo.glide" "$T/src/foo.glide"
cp "$FIX/src/a/b.glide" "$T/src/a/b.glide"
cp "$FIX/tests/"*.glide "$T/tests/"

echo "[smoke_qualified_modules] glide test on sibling-module qualified-type tests"
OUT="$(cd "$T" && "$GLIDE" test tests 2>&1)" || { echo "$OUT"; fail "glide test exited non-zero"; }
echo "$OUT" | grep -q "files passed" || { echo "$OUT"; fail "not all test files passed"; }
echo "PASS: qualified-type sibling-module tests all passed"
exit 0

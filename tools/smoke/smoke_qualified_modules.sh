#!/usr/bin/env bash
# Smoke: module-qualified types across a sibling module (foo) and a submodule
# (a::b, referenced by its leaf b::) in every position the typer canonicalization
# touches — qualified return / *ptr param / struct field / Vector element /
# match-arm pattern / submodule type. Built as a plain `bin` and RUN to a known
# sum (26). Using a bin instead of `glide test` keeps it OS-robust: a synthesized
# test main lives under build/ and resolves `stdlib::testing` differently across
# filesystems (it failed only on macOS/APFS), which has nothing to do with the
# qualified-type behaviour this guards.
set -u
GLIDE="${GLIDE:-$HOME/.glide/bin/glide.exe}"
DIR="$(cd "$(dirname "$0")" && pwd)"
FIX="$DIR/fixtures/qualified"
fail() { echo "FAIL: $*" >&2; exit 1; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/src/a"
printf 'let manifest: Package = Package { name:"t", version:"0.1.0", bin:"src/main.glide", deps: vec_of() };\n' > "$T/glide.glide"
cp "$FIX/src/foo.glide"  "$T/src/foo.glide"
cp "$FIX/src/a/b.glide"  "$T/src/a/b.glide"
cp "$FIX/src/main.glide" "$T/src/main.glide"

echo "[smoke_qualified_modules] building a program that uses qualified sibling + submodule types"
"$GLIDE" build "$T/src/main.glide" -o "$T/q" >/dev/null 2>&1 || fail "build failed"
OUT="$("$T/q" 2>&1)" || fail "exe exited non-zero"
echo "$OUT" | grep -q "qualified-sum: 26" || fail "wrong output: [$OUT]"
echo "PASS: qualified sibling + submodule types built and ran ($OUT)"
exit 0

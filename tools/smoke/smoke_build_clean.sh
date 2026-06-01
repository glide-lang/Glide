#!/usr/bin/env bash
# Smoke: a program that links heavy libs (`import stdlib::http`, which pulls
# in TLS / net / compress) builds with NO `tar:` / `gzip:` / `Cannot connect
# to C` noise on stderr.
#
# Those strings have leaked onto stderr in the past when the build shelled
# out to fetch/extract a sysroot or when a link step couldn't reach a C
# toolchain. A clean http build must be silent of that noise. We do NOT
# assert stderr is empty â€” the normal pipeline ("parsing", "compiling",
# "built") is expected â€” we assert only that the known-bad markers are absent.
set -u

GLIDE="${GLIDE:-$HOME/.glide/bin/glide.exe}"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$GLIDE" ] || fail "glide.exe not found at $GLIDE"

T="$(mktemp -d)" || fail "mktemp failed"
trap 'rm -rf "$T"' EXIT

cat > "$T/main.glide" <<'EOF'
import stdlib::http::*;

// Touch the heavy surface so the linker is forced to pull it in.
fn main() -> i32 {
    let r: HttpResponse = HttpResponse::ok().body("link-me");
    println!(r.status);
    return 0;
}
EOF

echo "[smoke_build_clean] building an 'import stdlib::http' program"
"$GLIDE" build "$T/main.glide" -o "$T/app.exe" >"$T/out.txt" 2>"$T/err.txt"

# Build must actually succeed (rc is unreliable; assert the exe).
[ -f "$T/app.exe" ] || { cat "$T/out.txt" "$T/err.txt" >&2; fail "program did not build"; }

# Scan BOTH streams for the known-bad markers.
if grep -nE 'tar:|gzip:|Cannot connect to C' "$T/err.txt" "$T/out.txt"; then
  echo "---- stderr ----" >&2; cat "$T/err.txt" >&2
  echo "---- stdout ----" >&2; cat "$T/out.txt" >&2
  fail "build emitted tar:/gzip:/Cannot-connect-to-C noise"
fi

echo "PASS: 'import stdlib::http' built cleanly (no tar/gzip/Cannot-connect noise)"
exit 0

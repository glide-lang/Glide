#!/usr/bin/env bash
# Smoke: a program whose handler RETURNS a module-qualified type
# (`fn f() -> http::HttpResponse`) builds AND the resulting exe runs and
# prints the expected line.
#
# Guards the typer canonicalization that strips an imported-module prefix
# from a type annotation (`http::HttpResponse` names the same global type as
# the bare `HttpResponse`). The .glide unit test covers type-checking; this
# smoke goes one step further and confirms codegen produces a working exe â€”
# i.e. the qualified return type wasn't silently miscompiled to `void`.
set -u

GLIDE="C:/Users/bye45/.glide/bin/glide.exe"
EXPECT="qualified-handler-ok"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$GLIDE" ] || fail "glide.exe not found at $GLIDE"

T="$(mktemp -d)" || fail "mktemp failed"
trap 'rm -rf "$T"' EXIT

cat > "$T/main.glide" <<EOF
import stdlib::http;

// module-qualified RETURN type + qualified Type::method chain in the body
fn make() -> http::HttpResponse {
    return http::HttpResponse::ok().body("$EXPECT");
}

// module-qualified POINTER param + field access through it
fn read_body(r: *http::HttpResponse) -> string {
    return r.body;
}

fn main() -> i32 {
    let r: http::HttpResponse = make();
    println!(read_body(&r));
    return 0;
}
EOF

echo "[smoke_qualified_build] building module-qualified-type program"
"$GLIDE" build "$T/main.glide" -o "$T/app.exe" >"$T/out.txt" 2>"$T/err.txt"
[ -f "$T/app.exe" ] || { cat "$T/out.txt" "$T/err.txt" >&2; fail "qualified-type program did not build"; }

echo "[smoke_qualified_build] running the exe"
OUT="$("$T/app.exe")" || fail "exe exited non-zero"

[ "$OUT" = "$EXPECT" ] || fail "unexpected output: got [$OUT], want [$EXPECT]"

echo "PASS: http::HttpResponse handler built and ran, printed [$OUT]"
exit 0

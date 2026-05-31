#!/usr/bin/env bash
# Smoke: an http_listen server actually RESPONDS to a request.
#
# Guards the Windows reactor-hang regression where http_listen would bind
# but never service a connection (the listen loop / coroutine reactor would
# stall). A pure .glide unit test can't catch this â€” it needs a real built
# exe, a real socket, and a real client doing a round trip. We build a
# trivial server, run it on a high port, and assert the response body.
set -u

GLIDE="C:/Users/bye45/.glide/bin/glide.exe"
PORT=38731
EXPECT="smoke-http-server-ok"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$GLIDE" ] || fail "glide.exe not found at $GLIDE"

T="$(mktemp -d)" || fail "mktemp failed"
SRV_PID=""
cleanup() {
  if [ -n "$SRV_PID" ]; then
    kill "$SRV_PID" 2>/dev/null
    # Windows-native process: kill may not reap a non-job child; force it.
    taskkill //PID "$SRV_PID" //F 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
  fi
  rm -rf "$T"
}
trap cleanup EXIT

cat > "$T/main.glide" <<EOF
import stdlib::http::*;

fn root(_req: *HttpRequest) -> HttpResponse {
    return HttpResponse::ok().body("$EXPECT");
}

fn main() -> i32 {
    let _ = http_listen($PORT, root);
    return 0;
}
EOF

echo "[smoke_http_server] building server on port $PORT"
"$GLIDE" build "$T/main.glide" -o "$T/srv.exe" >"$T/out.txt" 2>"$T/err.txt"
# `glide build` can exit 0 even on a compile error, so assert the exe exists.
[ -f "$T/srv.exe" ] || { cat "$T/out.txt" "$T/err.txt" >&2; fail "server did not build"; }

echo "[smoke_http_server] starting server"
"$T/srv.exe" >"$T/srv_out.txt" 2>"$T/srv_err.txt" &
SRV_PID=$!
sleep 2

# Confirm the process is still alive (a reactor hang at bind would have it
# alive but unresponsive â€” caught below; an early crash is caught here).
kill -0 "$SRV_PID" 2>/dev/null || { cat "$T/srv_err.txt" >&2; fail "server exited before serving"; }

echo "[smoke_http_server] curling http://127.0.0.1:$PORT/hi"
BODY="$(curl -s --max-time 8 "http://127.0.0.1:$PORT/hi")" || fail "curl failed (server unresponsive â€” reactor hang?)"

[ "$BODY" = "$EXPECT" ] || fail "unexpected body: got [$BODY], want [$EXPECT]"

echo "PASS: http_listen server responded with [$BODY] on port $PORT"
exit 0

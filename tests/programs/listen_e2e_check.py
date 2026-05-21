import http.client, os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GLIDE = os.path.join(REPO, "glide.exe" if os.name == "nt" else "glide")
SRC = os.path.join(REPO, "tests", "programs", "listen_e2e.glide")
BIN = os.path.join(REPO, "tests", "programs", "listen_e2e.exe")
PORT = 18080

def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)

def wait_port(port, deadline_s=10):
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False

def http_call(method, path, body=None):
    # Send the whole request in one buffer so the Glide HTTP parser's
    # single-read path sees headers + body together.
    body = body or b""
    req = (f"{method} {path} HTTP/1.1\r\n"
           f"Host: 127.0.0.1:{PORT}\r\n"
           f"Content-Length: {len(body)}\r\n"
           f"Connection: close\r\n\r\n").encode("ascii") + body
    s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
    s.sendall(req)
    raw = b""
    while True:
        chunk = s.recv(4096)
        if not chunk: break
        raw += chunk
    s.close()
    head, _, body_b = raw.partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n", 1)[0]
    status = int(status_line.split(b" ")[1])
    return status, body_b

print(f"[build] {SRC}")
build = subprocess.run([GLIDE, "build", SRC, "-o", BIN], capture_output=True, text=True)
if build.returncode != 0:
    print(build.stdout); print(build.stderr, file=sys.stderr)
    fail("build failed")

print(f"[spawn] {BIN}")
proc = subprocess.Popen([BIN], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    if not wait_port(PORT):
        fail("server never bound to port")

    status, body = http_call("GET", "/health")
    assert status == 200, f"GET /health: status {status}"
    assert body == b"ok", f"GET /health: body {body!r}"
    print("  GET /health -> 200 ok")

    status, body = http_call("GET", "/users/42")
    assert status == 200, f"GET /users/42: status {status}"
    assert body == b"user:42", f"GET /users/42: body {body!r}"
    print("  GET /users/42 -> 200 user:42")

    status, body = http_call("POST", "/echo", body=b"ping")
    assert status == 200, f"POST /echo: status {status}"
    assert body == b"ping", f"POST /echo: body {body!r}"
    print("  POST /echo ping -> 200 ping")

    status, body = http_call("GET", "/nope")
    assert status == 404, f"GET /nope: status {status} (expected 404)"
    print("  GET /nope -> 404")

    print("all checks passed")
finally:
    proc.terminate()
    try: proc.wait(timeout=3)
    except subprocess.TimeoutExpired: proc.kill()

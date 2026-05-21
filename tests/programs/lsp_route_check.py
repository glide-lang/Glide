import json, os, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GLIDE = os.path.join(REPO, "glide.exe")
DOC = os.path.join(REPO, "tests", "programs", "route_lsp_smoke.glide")

def frame(obj):
    payload = json.dumps(obj).encode("utf-8")
    return f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii") + payload

def read_msg(p):
    headers = b""
    while b"\r\n\r\n" not in headers:
        ch = p.stdout.read(1)
        if not ch: return None
        headers += ch
    parts = headers.decode("ascii").split("\r\n")
    n = int([h for h in parts if h.lower().startswith("content-length")][0].split(":")[1].strip())
    body = p.stdout.read(n)
    return json.loads(body)

p = subprocess.Popen([GLIDE, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

p.stdin.write(frame({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}))
p.stdin.flush()
init_resp = read_msg(p)
print("[initialize] capabilities ok=", "result" in init_resp)

p.stdin.write(frame({"jsonrpc":"2.0","method":"initialized","params":{}}))

with open(DOC, "r", encoding="utf-8") as f:
    text = f.read()
uri = "file:///" + DOC.replace("\\", "/")
p.stdin.write(frame({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
    "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":text}}}))
p.stdin.flush()

p.stdin.write(frame({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{
    "textDocument":{"uri":uri}}}))
p.stdin.flush()
ds = read_msg(p)
while ds.get("id") != 2:
    ds = read_msg(p)
names = [s["name"] for s in ds.get("result", [])]
print("[documentSymbol] names:")
for n in names: print("  ", n)

p.stdin.write(frame({"jsonrpc":"2.0","id":3,"method":"glide/routeList","params":{}}))
p.stdin.flush()
rl = read_msg(p)
while rl.get("id") != 3:
    rl = read_msg(p)
print("[routeList] routes:")
for r in rl.get("result", {}).get("routes", []):
    print(f"  {r['method']:6} {r['path']:20} -> {r['handler']} ({r['line']}:{r['column']})")

p.stdin.write(frame({"jsonrpc":"2.0","id":4,"method":"shutdown","params":None}))
p.stdin.flush()
p.stdin.write(frame({"jsonrpc":"2.0","method":"exit","params":None}))
p.stdin.flush()
p.wait(timeout=5)

#!/usr/bin/env python3
# Standalone copy of the lsp_smoke.py case_* helpers so a single batch of cases
# can be verified in isolation against ~/.glide/bin/glide.exe. Append your
# case_*(...) calls below the helpers, end with _finish(), and run it.
import json, os, re, subprocess, sys, tempfile, shutil
GLIDE = os.path.expanduser("~/.glide/bin/glide.exe")

def frame(m):
    b = json.dumps(m).encode()
    return b"Content-Length: " + str(len(b)).encode() + b"\r\n\r\n" + b

def run_session(msgs):
    r = subprocess.run([GLIDE, "lsp"], input=b"".join(frame(m) for m in msgs),
                       capture_output=True, timeout=120)
    out = []; buf = r.stdout; i = 0
    while True:
        m = re.search(rb"Content-Length: (\d+)\r\n\r\n", buf[i:])
        if not m: break
        n = int(m.group(1)); s = i + m.end()
        try: out.append(json.loads(buf[s:s+n].decode("utf-8")))
        except Exception: pass
        i = s + n
    return out

def wtmp(name, body):
    safe = re.sub(r"[^A-Za-z0-9._-]", "_", name)
    p = os.path.join(tempfile.gettempdir(), safe).replace(os.sep, "/")
    open(p, "w", encoding="utf-8").write(body)
    return p, "file:///" + p

results = []
def check(n, ok, detail=""):
    print(("PASS" if ok else "FAIL"), n, (f"({detail})" if detail else ""))
    results.append((n, ok))

def _proj_uri(d, rel=""):
    p = ((d + "/" + rel) if rel else d).rstrip("/").replace(os.sep, "/")
    if len(p) >= 2 and p[1] == ":":
        p = p[0].lower() + "%3A" + p[2:]
    return "file:///" + p

PROJ_MANIFEST = ('let manifest: Package = Package { name:"t", version:"0.1.0", '
                 'bin:"src/main.glide", deps: vec_of() };\n')

def _items(rs):
    t = next((r for r in rs if r.get("id") == 2), None)
    res = (t or {}).get("result", []) or []
    return res.get("items", []) if isinstance(res, dict) else res

def case_completion_has(label, body, pos, expect):
    _, uri = wtmp(label, body)
    rs = run_session([{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
      {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
      {"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":uri},"position":pos}},
      {"jsonrpc":"2.0","method":"exit","params":None}])
    labs = set(it.get("label") for it in _items(rs))
    for l in expect:
        check(f"{label}: offers {l}", l in labs, f"got {sorted(x for x in labs if x)[:12]}")

def case_completion_absent(label, body, pos, absent):
    _, uri = wtmp(label, body)
    rs = run_session([{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
      {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
      {"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":uri},"position":pos}},
      {"jsonrpc":"2.0","method":"exit","params":None}])
    labs = set(it.get("label") for it in _items(rs))
    for l in absent:
        check(f"{label}: hides {l}", l not in labs, "leaked")

def case_completion_project(label, files, open_rel, pos, present=None, absent=None, import_edit=None):
    d = tempfile.mkdtemp().replace(os.sep, "/")
    for rel, c in files.items():
        fp = os.path.join(d, *rel.split("/"))
        os.makedirs(os.path.dirname(fp), exist_ok=True)
        open(fp, "w", encoding="utf-8", newline="").write(c)
    uri = _proj_uri(d, open_rel)
    rs = run_session([{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":_proj_uri(d)}},
      {"jsonrpc":"2.0","method":"initialized","params":{}},
      {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"glide","version":1,"text":files[open_rel]}}},
      {"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":uri},"position":pos}},
      {"jsonrpc":"2.0","method":"exit","params":None}])
    items = _items(rs); by = {it.get("label"): it for it in items}; labs = set(by)
    for l in (present or []):
        check(f"{label}: offers {l}", l in labs, f"got {sorted(x for x in labs if x)[:12]}")
    for l in (absent or []):
        check(f"{label}: hides {l}", l not in labs, "leaked")
    if import_edit:
        l, txt = import_edit
        eds = (by.get(l) or {}).get("additionalTextEdits") or []
        check(f"{label}: {l} edit {txt!r}", txt in [e.get("newText") for e in eds],
              f"got {[e.get('newText') for e in eds]}")
    shutil.rmtree(d, ignore_errors=True)

def _posreq(method, body, pos):
    _, uri = wtmp("h", body)
    return run_session([{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
      {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
      {"jsonrpc":"2.0","id":2,"method":method,"params":{"textDocument":{"uri":uri},"position":pos}},
      {"jsonrpc":"2.0","method":"exit","params":None}])

def case_hover_has(label, body, pos, sub):
    rs = _posreq("textDocument/hover", body, pos)
    t = next((r for r in rs if r.get("id") == 2), None)
    res = (t or {}).get("result") or {}
    c = res.get("contents") if isinstance(res, dict) else None
    txt = c.get("value", "") if isinstance(c, dict) else (c or "")
    if isinstance(txt, list): txt = " ".join(str(x) for x in txt)
    check(f"{label}: hover ~ {sub!r}", sub.lower() in str(txt).lower(), f"got {str(txt)[:80]!r}")

def case_definition_line(label, body, pos, expect_line):
    rs = _posreq("textDocument/definition", body, pos)
    t = next((r for r in rs if r.get("id") == 2), None)
    res = (t or {}).get("result")
    if isinstance(res, list): res = res[0] if res else None
    if expect_line is None:
        check(f"{label}: no def", not res, f"got {res}"); return
    ln = (res or {}).get("range", {}).get("start", {}).get("line") if res else None
    check(f"{label}: def@{expect_line}", ln == expect_line, f"got {ln}")

def _finish():
    p = sum(1 for _, ok in results if ok)
    print(f"\n{p}/{len(results)} green")
    sys.exit(0 if p == len(results) else 1)

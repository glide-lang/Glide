#!/usr/bin/env python3
"""End-to-end smoke test for the Glide LSP.

Spawns `glide lsp` as a subprocess, drives it through Content-Length-framed
JSON-RPC messages, and checks diagnostics + position-based features
(hover, definition, references, rename, completion, documentSymbol,
documentHighlight, formatting).
"""
import json, os, re, subprocess, sys, tempfile

GLIDE = os.path.abspath("./glide.exe") if os.name == "nt" else "./glide"
ROOT  = os.path.abspath(".").replace(os.sep, "/")

def frame(msg: dict) -> bytes:
    body = json.dumps(msg).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body

def parse_responses(buf: bytes):
    out = []
    text = buf.decode("utf-8", "replace")
    pattern = re.compile(r"Content-Length: (\d+)\r\n\r\n")
    i = 0
    while i < len(text):
        m = pattern.search(text, i)
        if not m: break
        n = int(m.group(1))
        start = m.end()
        body = text[start:start+n]
        try:
            out.append(json.loads(body))
        except json.JSONDecodeError:
            pass
        i = start + n
    return out

def run_session(messages):
    payload = b"".join(frame(m) for m in messages)
    r = subprocess.run([GLIDE, "lsp"], input=payload, capture_output=True, timeout=120)
    return parse_responses(r.stdout)

def write_tmp(name: str, body: str) -> str:
    path = os.path.join(tempfile.gettempdir(), name).replace(os.sep, "/")
    with open(path, "w", encoding="utf-8") as f:
        f.write(body)
    return path, "file:///" + path

PASS, FAIL = "PASS", "FAIL"
results = []

def check(name, ok, detail=""):
    tag = PASS if ok else FAIL
    print(f"  {tag} {name}" + (f"  ({detail})" if detail else ""))
    results.append((name, ok))

def case_diagnostics(label, body, expect_codes_present=None, expect_codes_absent=None):
    print(f"\n[diagnostics] {label}")
    path, uri = write_tmp(label.replace(" ", "_") + ".glide", body)
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
        {"jsonrpc":"2.0","method":"exit","params":None},
    ]
    rs = run_session(msgs)
    diags = []
    for r in rs:
        if r.get("method") == "textDocument/publishDiagnostics":
            diags = r["params"]["diagnostics"]
            break
    codes = sorted(set(d.get("code","") for d in diags if d.get("code")))
    msgs = [d.get("message","") for d in diags]
    for code in (expect_codes_present or []):
        check(f"emits `{code}`", code in codes, f"got: {codes}")
    for code in (expect_codes_absent or []):
        check(f"does NOT emit `{code}`", code not in codes, f"got: {codes}")

def case_feature(label, body, request, expect_validator):
    print(f"\n[feature] {label}")
    path, uri = write_tmp(label.replace(" ", "_") + ".glide", body)
    request["params"]["textDocument"] = {"uri": uri}
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
        request,
        {"jsonrpc":"2.0","method":"exit","params":None},
    ]
    rs = run_session(msgs)
    target = next((r for r in rs if r.get("id") == request["id"]), None)
    expect_validator(target)

# ---- diagnostic battery ----

case_diagnostics("clean program",
    'fn main() -> int { let x: int = 7; return x; }',
    expect_codes_absent=["unused-var","unused-fn","unnecessary-mut","missing-return"])

case_diagnostics("unused var",
    'fn main() -> int { let x: int = 5; return 0; }',
    expect_codes_present=["unused-var"])

case_diagnostics("unused fn",
    'fn helper() -> int { return 1; }\nfn main() -> int { return 0; }',
    expect_codes_present=["unused-fn"])

case_diagnostics("unnecessary mut",
    'fn main() -> int { let mut x: int = 5; return x; }',
    expect_codes_present=["unnecessary-mut"])

case_diagnostics("missing return",
    'fn need() -> int { let _x: int = 5; }\nfn main() -> int { return need(); }',
    expect_codes_present=["missing-return"])

case_diagnostics("dead code",
    'fn main() -> int { return 0; let _x: int = 1; }',
    expect_codes_present=["dead-code"])

case_diagnostics("borrow in field",
    'struct Bad { r: &int }\nfn main() -> int { return 0; }',
    expect_codes_present=["borrow-in-field"])

case_diagnostics("null borrow",
    'fn main() -> int { let r: &int = null; return 0; }',
    expect_codes_present=["null-borrow"])

case_diagnostics("addr of temporary",
    'struct V2 { x: int, y: int }\n'
    'fn make() -> *V2 { return &V2 { x: 1, y: 2 }; }\n'
    'fn main() -> int { return 0; }',
    expect_codes_present=["addr-of-temporary"])

case_diagnostics("arena not freed",
    'fn main() -> int { let arena: *Arena = Arena::new(1024); return 0; }',
    expect_codes_present=["arena-not-freed"])

case_diagnostics("arena freed via defer",
    'fn main() -> int {\n'
    '    let arena: *Arena = Arena::new(1024);\n'
    '    defer arena.free();\n'
    '    return 0;\n'
    '}',
    expect_codes_absent=["arena-not-freed"])

case_diagnostics("large return",
    'struct Big { a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int,\n'
    '              i: int, j: int, k: int, l: int, m: int, n: int, o: int, p: int }\n'
    'fn make() -> Big { return Big { a:0, b:0, c:0, d:0, e:0, f:0, g:0, h:0,\n'
    '                                i:0, j:0, k:0, l:0, m:0, n:0, o:0, p:0 }; }\n'
    'fn main() -> int { make(); return 0; }',
    expect_codes_present=["large-return"])

case_diagnostics("overlap borrow",
    'fn main() -> int {\n'
    '    let mut x: int = 5;\n'
    '    let r1: &mut int = &mut x;\n'
    '    let r2: &mut int = &mut x;\n'
    '    return *r1 + *r2;\n'
    '}',
    expect_codes_present=["overlap-borrow"])

# ---- position features ----

def expect_hover(label, has_text):
    return lambda r: check(label,
        r and "result" in r and r["result"] and has_text in r["result"].get("contents", {}).get("value",""),
        f"got: {(r or {}).get('result')}")

case_feature("hover on fn",
    'fn add(a: int, b: int) -> int { return a + b; }\n'
    'fn main() -> int { return add(1, 2); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/hover",
     "params":{"position":{"line":1,"character":27}}},  # 'add' inside main (chars 26-28)
    expect_hover("hover returns fn signature", "fn add(a: int, b: int) -> int"))

case_feature("documentSymbol",
    'struct Point { x: int, y: int }\n'
    'fn area() -> int { return 0; }\n'
    'fn main() -> int { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{}},
    lambda r: check("doc symbols list 3 entries",
        r and "result" in r and len(r["result"]) == 3,
        f"got {len(r['result']) if r and 'result' in r else 0}"))

case_feature("goto definition",
    'fn helper() -> int { return 1; }\n'
    'fn main() -> int { return helper(); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
     "params":{"position":{"line":1,"character":27}}},  # 'helper' in main
    lambda r: check("definition resolves to line 0",
        r and "result" in r and r["result"]
            and r["result"].get("range",{}).get("start",{}).get("line") == 0,
        f"got: {(r or {}).get('result')}"))

case_feature("completion suggests locals + top-level",
    'fn add(a: int, b: int) -> int { return a + b; }\n'
    'fn main() -> int {\n'
    '    let x = 7;\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":3,"character":11}}},  # right before return value
    lambda r: (
        check("completion includes `add`",
            r and any(it.get("label")=="add" for it in r.get("result",[]))),
        check("completion includes local `x`",
            r and any(it.get("label")=="x" for it in r.get("result",[]))),
    ))

case_feature("references finds 2 sites",
    'fn helper() -> int { return 1; }\n'
    'fn main() -> int { return helper(); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/references",
     "params":{"position":{"line":0,"character":4},"context":{"includeDeclaration":True}}},
    lambda r: check("references lists 2 sites",
        r and "result" in r and len(r["result"]) == 2,
        f"got {len(r['result']) if r and 'result' in r else 0}"))

case_feature("rename produces edits",
    'fn helper() -> int { return 1; }\n'
    'fn main() -> int { return helper(); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/rename",
     "params":{"position":{"line":0,"character":4},"newName":"do_thing"}},
    lambda r: check("rename returns 2 edits",
        r and "result" in r and r["result"] and
        len(list(r["result"].get("changes",{}).values())[0]) == 2,
        f"got: {(r or {}).get('result')}"))

case_feature("documentHighlight",
    'fn main() -> int {\n'
    '    let x: int = 1;\n'
    '    let y: int = x;\n'
    '    return x + y;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/documentHighlight",
     "params":{"position":{"line":1,"character":8}}},  # 'x' on let line
    lambda r: check("highlight finds 3 occurrences of `x`",
        r and "result" in r and len(r["result"]) == 3,
        f"got {len(r['result']) if r and 'result' in r else 0}"))

case_feature("formatting normalizes whitespace",
    'fn   add(  a:int,b   :int  )->int{return    a+b ;}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/formatting",
     "params":{"options":{"tabSize":4,"insertSpaces":True}}},
    lambda r: check("formatter produces clean text",
        r and "result" in r and r["result"]
            and "fn add(a: int, b: int) -> int" in r["result"][0].get("newText",""),
        f"got: {(r or {}).get('result')}"))

# ---- summary ----
print()
passed = sum(1 for _, ok in results if ok)
total = len(results)
print(f"{passed}/{total} {'all green' if passed == total else 'failures'}")
sys.exit(0 if passed == total else 1)

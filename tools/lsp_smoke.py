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
    'fn main() -> i32 { let x: i32 = 7; return x; }',
    expect_codes_absent=["unused-var","unused-fn","unnecessary-mut","missing-return"])

case_diagnostics("unused var",
    'fn main() -> i32 { let x: i32 = 5; return 0; }',
    expect_codes_present=["unused-var"])

case_diagnostics("unused fn",
    'fn helper() -> i32 { return 1; }\nfn main() -> i32 { return 0; }',
    expect_codes_present=["unused-fn"])

case_diagnostics("unnecessary mut",
    'fn main() -> i32 { let mut x: i32 = 5; return x; }',
    expect_codes_present=["unnecessary-mut"])

case_diagnostics("missing return",
    'fn need() -> i32 { let _x: i32 = 5; }\nfn main() -> i32 { return need(); }',
    expect_codes_present=["missing-return"])

case_diagnostics("dead code",
    'fn main() -> i32 { return 0; let _x: i32 = 1; }',
    expect_codes_present=["dead-code"])

case_diagnostics("borrow in field",
    'struct Bad { r: &i32 }\nfn main() -> i32 { return 0; }',
    expect_codes_present=["borrow-in-field"])

case_diagnostics("null borrow",
    'fn main() -> i32 { let r: &i32 = null; return 0; }',
    expect_codes_present=["null-borrow"])

case_diagnostics("addr of temporary",
    'struct V2 { x: i32, y: i32 }\n'
    'fn make() -> *V2 { return &V2 { x: 1, y: 2 }; }\n'
    'fn main() -> i32 { return 0; }',
    expect_codes_present=["addr-of-temporary"])

case_diagnostics("arena not freed",
    'fn main() -> i32 { let arena: *Arena = Arena::new(1024); return 0; }',
    expect_codes_present=["arena-not-freed"])

case_diagnostics("arena freed via defer",
    'fn main() -> i32 {\n'
    '    let arena: *Arena = Arena::new(1024);\n'
    '    defer arena.free();\n'
    '    return 0;\n'
    '}',
    expect_codes_absent=["arena-not-freed"])

case_diagnostics("large return",
    'struct Big { a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32, h: i32,\n'
    '              i: i32, j: i32, k: i32, l: i32, m: i32, n: i32, o: i32, p: i32 }\n'
    'fn make() -> Big { return Big { a:0, b:0, c:0, d:0, e:0, f:0, g:0, h:0,\n'
    '                                i:0, j:0, k:0, l:0, m:0, n:0, o:0, p:0 }; }\n'
    'fn main() -> i32 { make(); return 0; }',
    expect_codes_present=["large-return"])

case_diagnostics("overlap borrow",
    'fn main() -> i32 {\n'
    '    let mut x: i32 = 5;\n'
    '    let r1: &mut i32 = &mut x;\n'
    '    let r2: &mut i32 = &mut x;\n'
    '    return *r1 + *r2;\n'
    '}',
    expect_codes_present=["overlap-borrow"])

# ---- position features ----

def expect_hover(label, has_text):
    return lambda r: check(label,
        r and "result" in r and r["result"] and has_text in r["result"].get("contents", {}).get("value",""),
        f"got: {(r or {}).get('result')}")

case_feature("hover on fn",
    'fn add(a: i32, b: i32) -> i32 { return a + b; }\n'
    'fn main() -> i32 { return add(1, 2); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/hover",
     "params":{"position":{"line":1,"character":27}}},  # 'add' inside main (chars 26-28)
    expect_hover("hover returns fn signature", "fn add(a: i32, b: i32) -> i32"))

case_feature("documentSymbol",
    'struct Point { x: i32, y: i32 }\n'
    'fn area() -> i32 { return 0; }\n'
    'fn main() -> i32 { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{}},
    lambda r: check("doc symbols list 3 entries",
        r and "result" in r and len(r["result"]) == 3,
        f"got {len(r['result']) if r and 'result' in r else 0}"))

case_feature("goto definition",
    'fn helper() -> i32 { return 1; }\n'
    'fn main() -> i32 { return helper(); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
     "params":{"position":{"line":1,"character":27}}},  # 'helper' in main
    lambda r: check("definition resolves to line 0",
        r and "result" in r and r["result"]
            and r["result"].get("range",{}).get("start",{}).get("line") == 0,
        f"got: {(r or {}).get('result')}"))

case_feature("completion suggests locals + top-level",
    'fn add(a: i32, b: i32) -> i32 { return a + b; }\n'
    'fn main() -> i32 {\n'
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
    'fn helper() -> i32 { return 1; }\n'
    'fn main() -> i32 { return helper(); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/references",
     "params":{"position":{"line":0,"character":4},"context":{"includeDeclaration":True}}},
    lambda r: check("references lists 2 sites",
        r and "result" in r and len(r["result"]) == 2,
        f"got {len(r['result']) if r and 'result' in r else 0}"))

case_feature("rename produces edits",
    'fn helper() -> i32 { return 1; }\n'
    'fn main() -> i32 { return helper(); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/rename",
     "params":{"position":{"line":0,"character":4},"newName":"do_thing"}},
    lambda r: check("rename returns 2 edits",
        r and "result" in r and r["result"] and
        len(list(r["result"].get("changes",{}).values())[0]) == 2,
        f"got: {(r or {}).get('result')}"))

case_feature("documentHighlight",
    'fn main() -> i32 {\n'
    '    let x: i32 = 1;\n'
    '    let y: i32 = x;\n'
    '    return x + y;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/documentHighlight",
     "params":{"position":{"line":1,"character":8}}},  # 'x' on let line
    lambda r: check("highlight finds 3 occurrences of `x`",
        r and "result" in r and len(r["result"]) == 3,
        f"got {len(r['result']) if r and 'result' in r else 0}"))

case_feature("formatting normalizes whitespace",
    'fn   add(  a:i32,b   :i32  )->i32{return    a+b ;}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/formatting",
     "params":{"options":{"tabSize":4,"insertSpaces":True}}},
    lambda r: check("formatter produces clean text",
        r and "result" in r and r["result"]
            and "fn add(a: i32, b: i32) -> i32" in r["result"][0].get("newText",""),
        f"got: {(r or {}).get('result')}"))

# ---- signatureHelp ----

case_feature("signatureHelp free fn tracks active param",
    'fn add(a: i32, b: i32) -> i32 { return a + b; }\n'
    'fn main() -> i32 { let z = add(1, ); return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/signatureHelp",
     "params":{"position":{"line":1,"character":33}}},  # after `add(1, `
    lambda r: (
        check("sig label is add(a: i32, b: i32) -> i32",
            r and r.get("result") and
            r["result"]["signatures"][0]["label"] == "fn add(a: i32, b: i32) -> i32",
            f"got: {(r or {}).get('result')}"),
        check("active parameter is 1",
            r and r.get("result") and r["result"].get("activeParameter") == 1,
            f"got: {(r or {}).get('result')}"),
    ))

case_feature("signatureHelp on method hides self receiver",
    'struct Counter { n: i32 }\n'
    'impl Counter { fn bump_by(self: *Counter, delta: i32) -> i32 { return self.n + delta; } }\n'
    'fn main() -> i32 {\n'
    '    let c = Counter { n: 0 };\n'
    '    return c.bump_by(5);\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/signatureHelp",
     "params":{"position":{"line":4,"character":21}}},  # inside c.bump_by(
    lambda r: check("method sig drops self -> bump_by(delta: i32) -> i32",
        r and r.get("result") and
        r["result"]["signatures"][0]["label"] == "fn bump_by(delta: i32) -> i32",
        f"got: {(r or {}).get('result')}"))

case_feature("signatureHelp resolves Type-static via qualifier",
    'struct Vec2 { x: i32, y: i32 }\n'
    'impl Vec2 { fn make(x: i32, y: i32) -> Vec2 { return Vec2 { x: x, y: y }; } }\n'
    'fn main() -> i32 {\n'
    '    let v = Vec2::make(1, 2);\n'
    '    return v.x;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/signatureHelp",
     "params":{"position":{"line":3,"character":25}}},  # after `Vec2::make(1, `
    lambda r: (
        check("static sig label is make(x: i32, y: i32) -> Vec2",
            r and r.get("result") and
            r["result"]["signatures"][0]["label"] == "fn make(x: i32, y: i32) -> Vec2",
            f"got: {(r or {}).get('result')}"),
        check("static active parameter is 1",
            r and r.get("result") and r["result"].get("activeParameter") == 1,
            f"got: {(r or {}).get('result')}"),
    ))

case_feature("signatureHelp suppressed outside a call",
    'fn add(a: i32, b: i32) -> i32 { return a + b; }\n'
    'fn main() -> i32 { let z = add(1, 2); return z; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/signatureHelp",
     "params":{"position":{"line":1,"character":45}}},  # on `z` after the call closed
    lambda r: check("no signature past the closing paren",
        r and r.get("result") is None, f"got: {(r or {}).get('result')}"))

# ---- semanticTokens ----
# legend: 0 keyword 1 function 2 method 3 type 4 parameter
#         5 variable 6 property 7 macro 8 namespace

def decode_semtokens(data):
    """Undo the LSP delta encoding -> {(line, col): tokenType}."""
    toks, line, col = {}, 0, 0
    for k in range(0, len(data) - 4, 5):
        dl, dc, _len, ttype, _mods = data[k:k+5]
        if dl == 0: col += dc
        else: line += dl; col = dc
        toks[(line, col)] = ttype
    return toks

def semtok_map(r):
    if not (r and r.get("result") and "data" in r["result"]):
        return {}
    return decode_semtokens(r["result"]["data"])

case_feature("semanticTokens fn type parameter keyword",
    'fn add(a: i32, b: i32) -> i32 { return a + b; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{}},
    lambda r: (
        check("`add` -> function(1)", semtok_map(r).get((0,3)) == 1, f"got {semtok_map(r).get((0,3))}"),
        check("`i32` -> type(3)",     semtok_map(r).get((0,10)) == 3, f"got {semtok_map(r).get((0,10))}"),
        check("`a` -> parameter(4)",  semtok_map(r).get((0,7)) == 4, f"got {semtok_map(r).get((0,7))}"),
        check("`fn` -> keyword(0)",   semtok_map(r).get((0,0)) == 0, f"got {semtok_map(r).get((0,0))}"),
    ))

case_feature("semanticTokens type method property variable",
    'struct Point { x: i32, y: i32 }\n'
    'impl Point { fn mag(self: *Point) -> i32 { return self.x; } }\n'
    'fn main() -> i32 { let p = Point { x: 1, y: 2 }; return p.mag(); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{}},
    lambda r: (
        check("`Point` ctor -> type(3)",   semtok_map(r).get((2,27)) == 3, f"got {semtok_map(r).get((2,27))}"),
        check("`p.mag()` -> method(2)",    semtok_map(r).get((2,58)) == 2, f"got {semtok_map(r).get((2,58))}"),
        check("`self.x` -> property(6)",   semtok_map(r).get((1,55)) == 6, f"got {semtok_map(r).get((1,55))}"),
        check("receiver `p` -> variable(5)", semtok_map(r).get((2,56)) == 5, f"got {semtok_map(r).get((2,56))}"),
    ))

case_feature("semanticTokens module path namespace",
    'import stdlib::time;\n'
    'fn main() -> i32 { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{}},
    lambda r: check("`stdlib` before :: -> namespace(8)",
        semtok_map(r).get((0,7)) == 8, f"got {semtok_map(r).get((0,7))}"))

# ---- inlayHint ----

def inlay_map(r):
    """InlayHint[] -> {(line, char): label}."""
    res = (r or {}).get("result") or []
    return {(h.get("position",{}).get("line"), h.get("position",{}).get("character")): h.get("label")
            for h in res}

_FULL_RANGE = {"range":{"start":{"line":0,"character":0},"end":{"line":1000,"character":0}}}

case_feature("inlayHint infers literal let types",
    'fn main() -> i32 {\n'
    '    let n = 42;\n'
    '    let s = "hi";\n'
    '    let b = true;\n'
    '    let x: i32 = 5;\n'
    '    return n;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: (
        check("`n` hint -> ': i32'",    inlay_map(r).get((1,9)) == ": i32",    f"got {inlay_map(r).get((1,9))}"),
        check("`s` hint -> ': string'", inlay_map(r).get((2,9)) == ": string", f"got {inlay_map(r).get((2,9))}"),
        check("`b` hint -> ': bool'",   inlay_map(r).get((3,9)) == ": bool",   f"got {inlay_map(r).get((3,9))}"),
        check("annotated `x` gets no hint", all(k[0] != 4 for k in inlay_map(r)), f"got {inlay_map(r)}"),
    ))

case_feature("inlayHint infers fn return type",
    'fn mk() -> i32 { return 9; }\n'
    'fn main() -> i32 {\n'
    '    let q = mk();\n'
    '    return q;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: check("`q = mk()` hint -> ': i32'",
        inlay_map(r).get((2,9)) == ": i32", f"got {inlay_map(r).get((2,9))}"))

case_feature("inlayHint resolves Vector generic param",
    'fn main() -> i32 {\n'
    '    let v = Vector::new();\n'
    '    v.push(7);\n'
    '    return v.len();\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: check("`v = Vector::new()` hint mentions Vector",
        (inlay_map(r).get((1,9)) or "").find("Vector") >= 0, f"got {inlay_map(r).get((1,9))}"))

# ---- workspace/symbol ----

def ws_names(r):
    return [it.get("name") for it in (r or {}).get("result", []) or []]

case_feature("workspace-symbol finds stdlib HashMap",
    'fn main() -> i32 { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"workspace/symbol","params":{"query":"HashMap"}},
    lambda r: (
        check("HashMap is in results", "HashMap" in ws_names(r), f"got {ws_names(r)[:12]}"),
        check("HashMap has a .glide location", any(
            it.get("name")=="HashMap" and it.get("location",{}).get("uri","").endswith(".glide")
            for it in (r or {}).get("result",[])), f"got {(r or {}).get('result',[])[:2]}"),
        check("HashMap kind is Struct(23)", any(
            it.get("name")=="HashMap" and it.get("kind")==23
            for it in (r or {}).get("result",[]))),
    ))

case_feature("workspace-symbol is case-insensitive substring",
    'fn main() -> i32 { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"workspace/symbol","params":{"query":"hashm"}},
    lambda r: check("lowercase 'hashm' still matches HashMap",
        "HashMap" in ws_names(r), f"got {ws_names(r)[:12]}"))

# ---- canonical numeric types (retired legacy spellings + 256-bit) ----

def case_diag_message(label, body, expect_substr):
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
            diags = r["params"]["diagnostics"]; break
    allmsg = " | ".join(d.get("message","") for d in diags)
    check(f"diagnostic contains {expect_substr!r}", expect_substr in allmsg, f"got: {allmsg[:140]}")

case_diag_message("retired `int` type errors with hint",
    'fn main() -> i32 { let x: int = 5; return 0; }',
    "unknown type `int`; use `i32`")

case_diag_message("retired `float` type errors with hint",
    'fn f(x: float) -> i32 { return 0; }',
    "use `f64`")

def case_completion_has(label, body, pos, expect_labels):
    print(f"\n[feature] {label}")
    path, uri = write_tmp(label.replace(" ", "_") + ".glide", body)
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
        {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
         "params":{"textDocument":{"uri":uri},"position":pos}},
        {"jsonrpc":"2.0","method":"exit","params":None},
    ]
    rs = run_session(msgs)
    target = next((r for r in rs if r.get("id") == 2), None)
    items = (target or {}).get("result", []) or []
    labels = set(it.get("label") for it in items)
    for lbl in expect_labels:
        check(f"type-position completion offers `{lbl}`", lbl in labels,
              f"got {sorted(l for l in labels if l)[:14]}")

# Cursor sits right after `let x: ` so completion is in type position.
case_completion_has("type position offers wide primitives",
    'fn main() -> i32 {\n    let x: \n    return 0;\n}',
    {"line":1,"character":11},
    ["i32","i128","u128","i256","u256"])

# Member completion on a bare integer literal: `43.` -> int methods.
case_completion_has("int literal offers methods",
    'fn main() -> i32 {\n    let a = 43.\n    return 0;\n}',
    {"line":1,"character":15},
    ["abs","to_string"])

# Path completion on a 256-bit type: `u256::` -> `from` + `MAX`/`MIN`.
case_completion_has("u256 path offers from + limits",
    'fn main() -> i32 {\n    let a: u256 = u256::from(1);\n    return 0;\n}',
    {"line":1,"character":24},
    ["from","MAX","MIN"])

# `i32::` -> the MAX / MIN associated constants.
case_completion_has("int type path offers limits",
    'fn main() -> i32 {\n    let a: i32 = i32::MAX;\n    return 0;\n}',
    {"line":1,"character":22},
    ["MAX","MIN"])

# Bare-identifier position offers primitive type names too (editor filters
# `i3` -> `i32`); regression for completion going silent on a partial type.
case_completion_has("identifier position offers primitives",
    'fn main() -> i32 {\n    let n = 1;\n    i3\n    return 0;\n}',
    {"line":2,"character":6},
    ["i32"])

# ---- summary ----
print()
passed = sum(1 for _, ok in results if ok)
total = len(results)
print(f"{passed}/{total} {'all green' if passed == total else 'failures'}")
sys.exit(0 if passed == total else 1)

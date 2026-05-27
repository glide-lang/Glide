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
    # Operate on bytes: Content-Length is a BYTE count, so slicing a decoded
    # str by it desyncs the moment any message carries multi-byte UTF-8 (an
    # em-dash in a doc comment, say), silently dropping every later response.
    out = []
    pattern = re.compile(rb"Content-Length: (\d+)\r\n\r\n")
    i = 0
    while i < len(buf):
        m = pattern.search(buf, i)
        if not m: break
        n = int(m.group(1))
        start = m.end()
        body = buf[start:start+n]
        try:
            out.append(json.loads(body.decode("utf-8")))
        except (json.JSONDecodeError, UnicodeDecodeError):
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

case_diagnostics("for-in over a scalar (bare int var)",
    'fn main() -> i32 {\n    let n: i32 = 5;\n    for i in n { println!(i); }\n    return 0;\n}',
    expect_codes_present=["for-in-not-iterable"])

case_diagnostics("for-in over a .len() result",
    'fn main() -> i32 {\n    let v: *Vector<i32> = Vector::new();\n'
    '    for i in v.len() { println!(i); }\n    return 0;\n}',
    expect_codes_present=["for-in-not-iterable"])

case_diagnostics("for-in over a Vector is fine",
    'fn main() -> i32 {\n    let v: *Vector<i32> = Vector::new();\n'
    '    for x in v { println!(x); }\n    return 0;\n}',
    expect_codes_absent=["for-in-not-iterable"])

case_diagnostics("for-in over a range is fine",
    'fn main() -> i32 {\n    for i in 0..10 { println!(i); }\n    return 0;\n}',
    expect_codes_absent=["for-in-not-iterable"])

case_diagnostics("ignored result is flagged (must-use)",
    'fn mk() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 { mk(); return 0; }',
    expect_codes_present=["unused-result"])

case_diagnostics("handled result is not flagged",
    'fn mk() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 { let r = mk(); if r.ok { return r.val; } return 0; }',
    expect_codes_absent=["unused-result"])

case_diagnostics("void call is not flagged as unused-result",
    'fn do_it() { return; }\n'
    'fn main() -> i32 { do_it(); return 0; }',
    expect_codes_absent=["unused-result"])

case_diagnostics("reading .val without checking .ok is flagged",
    'fn mk() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 { let r: !i32 = mk(); return r.val; }',
    expect_codes_present=["unchecked-result"])

case_diagnostics(".val guarded by if r.ok is not flagged",
    'fn mk() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 { let r: !i32 = mk(); if r.ok { return r.val; } return 0; }',
    expect_codes_absent=["unchecked-result"])

case_diagnostics(".val after early-exit guard is not flagged",
    'fn mk() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 { let r: !i32 = mk(); if !r.ok { return 0; } return r.val; }',
    expect_codes_absent=["unchecked-result"])

case_diagnostics("inferred result local .val unguarded is flagged",
    'fn mk() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 { let r = mk(); return r.val; }',
    expect_codes_present=["unchecked-result"])

case_diagnostics("inferred option local .val unguarded is flagged",
    'fn find() -> ?i32 { return some(1); }\n'
    'fn main() -> i32 { let m = find(); return m.val; }',
    expect_codes_present=["ignored-option"])

case_diagnostics("use-after-free: free in a returning branch is not flagged",
    'fn cond() -> bool { return true; }\n'
    'fn f() -> i32 {\n'
    '    let v: *Vector<i32> = Vector::new();\n'
    '    if cond() { v.free(); return 0; }\n'
    '    return v.len();\n'
    '}',
    expect_codes_absent=["use-after-free"])

case_diagnostics("use-after-free: unconditional free then use is flagged",
    'fn f() -> i32 {\n'
    '    let v: *Vector<i32> = Vector::new();\n'
    '    v.free();\n'
    '    return v.len();\n'
    '}',
    expect_codes_present=["use-after-free"])

case_diagnostics("use-after-free: re-let with same name after free is not flagged",
    'fn f() -> i32 {\n'
    '    let v: *Vector<i32> = Vector::new();\n'
    '    v.free();\n'
    '    let v: *Vector<i32> = Vector::new();\n'
    '    let n: i32 = v.len();\n'
    '    v.free();\n'
    '    return n;\n'
    '}',
    expect_codes_absent=["use-after-free"])

def _diag_span_test():
    # The squiggle should underline the whole `r.val`, not just its first char.
    print("\n[diagnostics] unchecked-result underlines the whole r.val")
    body = ('fn mk() -> !i32 { return ok(1); }\n'
            'fn main() -> i32 { let r = mk(); return r.val; }')
    pa, uri = write_tmp("diag_span.glide", body)
    rs = run_session([
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
        {"jsonrpc":"2.0","method":"exit","params":None},
    ])
    rng = None
    for r in rs:
        if r.get("method") == "textDocument/publishDiagnostics":
            for d in r["params"]["diagnostics"]:
                if d.get("code") == "unchecked-result":
                    rng = d["range"]
    w = (rng["end"]["character"] - rng["start"]["character"]) if rng else 0
    check("range spans the full `r.val` (5 cols), not 1", w == 5, f"got width {w}")
_diag_span_test()

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

def _goto_line(r):
    res = (r or {}).get("result")
    if not res: return None
    if isinstance(res, list): res = res[0] if res else {}
    return res.get("range", {}).get("start", {}).get("line")

case_feature("goto field on inferred struct let",
    'struct Pt { x: i32, y: i32 }\n'
    'impl Pt { fn mag(self: *Pt) -> i32 { return self.x; } }\n'
    'fn main() -> i32 {\n'
    '    let p = Pt { x: 1, y: 2 };\n'
    '    let a = p.x;\n'
    '    let b = p.mag();\n'
    '    return a + b;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
     "params":{"position":{"line":4,"character":14}}},  # `x` in `p.x`
    lambda r: check("p.x jumps to the struct decl (line 0)", _goto_line(r) == 0, f"got {_goto_line(r)}"))

case_feature("goto method on inferred struct let",
    'struct Pt { x: i32, y: i32 }\n'
    'impl Pt { fn mag(self: *Pt) -> i32 { return self.x; } }\n'
    'fn main() -> i32 {\n'
    '    let p = Pt { x: 1, y: 2 };\n'
    '    let b = p.mag();\n'
    '    return b;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
     "params":{"position":{"line":4,"character":15}}},  # `mag` in `p.mag()`
    lambda r: check("p.mag jumps to the impl method (line 1)", _goto_line(r) == 1, f"got {_goto_line(r)}"))

case_feature("goto method on fn-return-typed local",
    'struct Pt { x: i32 }\n'
    'fn mk() -> *Pt { return new Pt { x: 1 }; }\n'
    'impl Pt { fn mag(self: *Pt) -> i32 { return self.x; } }\n'
    'fn main() -> i32 {\n'
    '    let q = mk();\n'
    '    return q.mag();\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
     "params":{"position":{"line":5,"character":13}}},  # `mag` in `q.mag()`
    lambda r: check("q.mag (q = mk()) jumps to the impl method (line 2)", _goto_line(r) == 2, f"got {_goto_line(r)}"))

case_feature("goToImplementation on a trait method lists all impls",
    'trait T { fn m(self: *Self) -> i32; }\n'
    'struct A { x: i32 }\n'
    'struct B { x: i32 }\n'
    'impl T for A { fn m(self: *A) -> i32 { return self.x; } }\n'
    'impl T for B { fn m(self: *B) -> i32 { return self.x; } }\n'
    'fn main() -> i32 { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/implementation",
     "params":{"position":{"line":0,"character":13}}},  # `m` in the trait
    lambda r: check("both impl methods are found",
        isinstance((r or {}).get("result"), list) and len(r["result"]) == 2,
        f"got {(r or {}).get('result')}"))

case_feature("typeDefinition jumps to the type's declaration",
    'struct Pt { x: i32 }\n'
    'fn main() -> i32 {\n'
    '    let p = Pt { x: 1 };\n'
    '    return p.x;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/typeDefinition",
     "params":{"position":{"line":3,"character":11}}},  # `p` in `p.x`
    lambda r: check("`p` (a Pt) jumps to `struct Pt` (line 0)", _goto_line(r) == 0, f"got {_goto_line(r)}"))

case_feature("goto on chained method receiver",
    'struct Bag { items: i32 }\n'
    'impl Bag { fn tag(self: *Bag) -> i32 { return self.items; } }\n'
    'fn mk() -> *Vector<*Bag> { return Vector::new(); }\n'
    'fn main() -> i32 {\n'
    '    let v = mk();\n'
    '    return v.get(0).tag();\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
     "params":{"position":{"line":5,"character":21}}},  # `tag` in `v.get(0).tag()`
    lambda r: check("`v.get(0).tag` resolves the element type and jumps to the impl (line 1)",
        _goto_line(r) == 1, f"got {_goto_line(r)}"))

case_feature("goto on a method call inside a range bound",
    'struct Cnt { n: i32 }\n'
    'impl Cnt { fn size(self: *Cnt) -> i32 { return self.n; } }\n'
    'fn main() -> i32 {\n'
    '    let c = Cnt { n: 3 };\n'
    '    for i in 0..c.size() { let x = i; }\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
     "params":{"position":{"line":4,"character":19}}},  # `size` in `0..c.size()`
    lambda r: check("`0..c.size()` doesn't let the `..` swallow the receiver (jumps to line 1)",
        _goto_line(r) == 1, f"got {_goto_line(r)}"))

def _multidoc_goto_test():
    # Opening a second document must NOT invalidate the first doc's AST.
    # Regression for the single-shared-arena bug where any analysis nulled
    # every open doc's stmts — so goto worked exactly once, then the editor
    # opened the target file and goto in the user's file went dead.
    print("\n[feature] goto survives a second document being opened")
    a_body = ('struct Pt { x: i32 }\n'
              'impl Pt { fn tag(self: *Pt) -> i32 { return self.x; } }\n'
              'fn main() -> i32 {\n'
              '    let p = Pt { x: 1 };\n'
              '    return p.tag();\n'
              '}')
    b_body = 'fn other() -> i32 { return 7; }'
    pa, ua = write_tmp("multidoc_A.glide", a_body)
    pb, ub = write_tmp("multidoc_B.glide", b_body)
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":ua,"languageId":"glide","version":1,"text":a_body}}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":ub,"languageId":"glide","version":1,"text":b_body}}},
        {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
         "params":{"textDocument":{"uri":ua},"position":{"line":4,"character":14}}},  # `tag` in `p.tag()`
        {"jsonrpc":"2.0","method":"exit","params":None},
    ]
    rs = run_session(msgs)
    target = next((r for r in rs if r.get("id") == 2), None)
    check("goto in doc A still resolves after doc B is opened", _goto_line(target) == 1,
          f"got {_goto_line(target)}")
_multidoc_goto_test()

def _wrap_fix_test():
    # A bare value where `?T` / `!T` is expected gets a "wrap in some()/ok()"
    # quick fix, with the edit computed from the source text (so strings and
    # nested values are wrapped exactly).
    print("\n[feature] wrap-in-constructor quick fix")
    body = ('fn main() -> i32 {\n'
            '    let x: ?i32 = 5;\n'
            '    let r: !string = "hi";\n'
            '    return 0;\n'
            '}')
    pa, uri = write_tmp("wrap_fix.glide", body)
    def first_edit(diag):
        msgs = [
            {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
            {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
                "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
            {"jsonrpc":"2.0","id":2,"method":"textDocument/codeAction","params":{
                "textDocument":{"uri":uri},"range":diag["range"],
                "context":{"diagnostics":[diag]}}},
            {"jsonrpc":"2.0","method":"exit","params":None},
        ]
        rs = run_session(msgs)
        target = next((r for r in rs if r.get("id") == 2), None)
        for a in ((target or {}).get("result") or []):
            for _, edits in a.get("edit", {}).get("changes", {}).items():
                for e in edits:
                    return e.get("newText", "")
        return ""
    d_some = {"range":{"start":{"line":1,"character":18},"end":{"line":1,"character":19}},
              "severity":1,"code":"wrap-some","message":"mismatch"}
    d_ok = {"range":{"start":{"line":2,"character":21},"end":{"line":2,"character":25}},
            "severity":1,"code":"wrap-ok","message":"mismatch"}
    check("`?i32 = 5` offers `some(5)`", first_edit(d_some) == "some(5)", f"got {first_edit(d_some)!r}")
    check("`!string = \"hi\"` offers `ok(\"hi\")`", first_edit(d_ok) == 'ok("hi")', f"got {first_edit(d_ok)!r}")
_wrap_fix_test()

def _propagate_fix_test():
    # A dropped result inside a result-returning fn gets a "Propagate with `?`"
    # fix; in a non-result fn the fix is withheld (`?` would be invalid there).
    print("\n[feature] propagate-with-? quick fix")
    def action(body, retty):
        full = 'fn mk() -> !i32 { return ok(1); }\n' + body
        pa, uri = write_tmp("propagate_fix_" + retty + ".glide", full)
        diag = {"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":8}},
                "severity":2,"code":"unused-result","message":"ignored"}
        rs = run_session([
            {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
            {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
                "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":full}}},
            {"jsonrpc":"2.0","id":2,"method":"textDocument/codeAction","params":{
                "textDocument":{"uri":uri},"range":diag["range"],"context":{"diagnostics":[diag]}}},
            {"jsonrpc":"2.0","method":"exit","params":None},
        ])
        target = next((r for r in rs if r.get("id") == 2), None)
        return (target or {}).get("result") or []
    res_fn = action('fn run() -> !i32 {\n    mk();\n    return ok(0);\n}', "result")
    nt = ""
    for a in res_fn:
        for _, edits in a.get("edit", {}).get("changes", {}).items():
            for e in edits: nt = e.get("newText", "")
    check("result-returning fn offers `?` propagation", nt == "?", f"got {nt!r}")
    plain_fn = action('fn run() -> i32 {\n    mk();\n    return 0;\n}', "plain")
    check("non-result fn withholds the `?` fix", len(plain_fn) == 0, f"got {len(plain_fn)} actions")
_propagate_fix_test()

case_diagnostics("raw value into optional flags a wrap fix",
    'fn main() -> i32 {\n    let x: ?i32 = 5;\n    return 0;\n}',
    expect_codes_present=["wrap-some"])

case_diagnostics("raw value into result flags a wrap fix",
    'fn mk() -> !i32 { return 5; }\nfn main() -> i32 { return 0; }',
    expect_codes_present=["wrap-ok"])

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

case_feature("inlayHint shows parameter names at call sites",
    'fn add(a: i32, b: i32) -> i32 { return a + b; }\n'
    'struct Pt { x: i32 }\n'
    'impl Pt { fn moved(self: *Pt, dx: i32, dy: i32) -> i32 { return self.x + dx + dy; } }\n'
    'fn main() -> i32 {\n'
    '    let s = add(10, 20);\n'
    '    let p = Pt { x: 1 };\n'
    '    let m = p.moved(3, 4);\n'
    '    return s + m;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: (
        check("free-fn call shows `a:` / `b:`",
              "a:" in inlay_map(r).values() and "b:" in inlay_map(r).values(),
              f"got {list(inlay_map(r).values())}"),
        check("method call shows `dx:` / `dy:` and skips `self`",
              "dx:" in inlay_map(r).values() and "dy:" in inlay_map(r).values()
              and "self:" not in inlay_map(r).values(),
              f"got {list(inlay_map(r).values())}"),
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

case_feature("inlayHint infers struct-literal type",
    'struct Alelo { alelo: string }\n'
    'fn main() -> i32 { let a = Alelo { alelo: "x" }; return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: check("`a = Alelo{}` hint -> ': Alelo'",
        inlay_map(r).get((1,24)) == ": Alelo", f"got {inlay_map(r).get((1,24))}"))

case_feature("inlayHint resolves Vector generic param",
    'fn main() -> i32 {\n'
    '    let v = Vector::new();\n'
    '    v.push(7);\n'
    '    return v.len();\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: check("`v = Vector::new()` hint mentions Vector",
        (inlay_map(r).get((1,9)) or "").find("Vector") >= 0, f"got {inlay_map(r).get((1,9))}"))

# ---- inlayHint: ownership lifecycle ----

def inlay_labels_on_line(r, line):
    return [h.get("label") for h in (r or {}).get("result", []) or []
            if h.get("position", {}).get("line") == line]

case_feature("inlayHint lifecycle freed via defer",
    'fn main() -> i32 {\n'
    '    let arena = Arena::new(1024);\n'
    '    defer arena.free();\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: check("alloc line shows 'freed @ L3'",
        "freed @ L3" in inlay_labels_on_line(r, 1), f"got {inlay_labels_on_line(r, 1)}"))

case_feature("inlayHint lifecycle leak",
    'fn main() -> i32 {\n'
    '    let arena = Arena::new(1024);\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: check("unfreed alloc shows 'never freed'",
        "never freed" in inlay_labels_on_line(r, 1), f"got {inlay_labels_on_line(r, 1)}"))

case_feature("inlayHint lifecycle moves out on return",
    'fn mk() -> *Arena {\n'
    '    let arena = Arena::new(1024);\n'
    '    return arena;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":dict(_FULL_RANGE)},
    lambda r: check("returned alloc shows 'moves out'",
        "moves out" in inlay_labels_on_line(r, 1), f"got {inlay_labels_on_line(r, 1)}"))

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

# ---- diagnostic relatedInformation ("why") ----

def case_related(label, body, expect_code, expect_related_substr):
    print(f"\n[related] {label}")
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
    target = next((d for d in diags if d.get("code") == expect_code), None)
    check(f"emits `{expect_code}`", target is not None,
          f"codes: {[d.get('code') for d in diags]}")
    rel = (target or {}).get("relatedInformation", []) or []
    check("has relatedInformation", len(rel) > 0, f"got {(target or {}).get('relatedInformation')}")
    joined = " | ".join(ri.get("message","") for ri in rel)
    check(f"related mentions {expect_related_substr!r}",
          expect_related_substr in joined, f"got: {joined}")
    if rel:
        loc = rel[0].get("location", {})
        check("related has a uri + range",
              loc.get("uri","").endswith(".glide") and "range" in loc, f"got: {loc}")

case_related("missing trait method points at the trait",
    'trait Greeter { fn greet(self: *Self) -> i32; }\n'
    'struct Bot { x: i32 }\n'
    'impl Greeter for Bot { }\n'
    'fn main() -> i32 { return 0; }',
    "missing-trait-method", "greet")

case_related("trait method arity mismatch points at the trait",
    'trait Greeter { fn greet(self: *Self, n: i32) -> i32; }\n'
    'struct Bot { x: i32 }\n'
    'impl Greeter for Bot { fn greet(self: *Bot) -> i32 { return 0; } }\n'
    'fn main() -> i32 { return 0; }',
    "trait-method-mismatch", "greet")

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

# ---- struct hover (fields) + outline (field children) ----

def _hover_val(r):
    return (((r or {}).get("result") or {}).get("contents", {}) or {}).get("value", "")

case_feature("hover on struct lists its fields",
    'struct Point { x: i32, y: i32 }\n'
    'fn main() -> i32 { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/hover",
     "params":{"position":{"line":0,"character":8}}},  # on `Point`
    lambda r: (
        check("hover shows `x: i32`", "x: i32" in _hover_val(r), f"got: {_hover_val(r)!r}"),
        check("hover shows `y: i32`", "y: i32" in _hover_val(r), f"got: {_hover_val(r)!r}"),
    ))

case_feature("documentSymbol nests struct fields as children",
    'struct Point { x: i32, y: i32 }\n'
    'fn main() -> i32 { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{}},
    lambda r: check("Point has children x, y",
        any(s.get("name")=="Point" and [c.get("name") for c in s.get("children",[])]==["x","y"]
            for s in (r.get("result") or [])),
        f"got: {[(s.get('name'), [c.get('name') for c in s.get('children',[])]) for s in (r.get('result') or [])]}"))

# ---- struct-literal field completion ----

def comp_labels(r):
    res = (r or {}).get("result")
    if isinstance(res, dict): res = res.get("items", [])
    return [it.get("label") for it in (res or [])]

def comp_doc(r, label):
    res = (r or {}).get("result")
    if isinstance(res, dict): res = res.get("items", [])
    for it in (res or []):
        if it.get("label") == label:
            d = it.get("documentation")
            return (d.get("value") if isinstance(d, dict) else d) or ""
    return ""

case_feature("struct-literal completion offers fields",
    'struct Point { x: i32, y: i32 }\n'
    'fn main() -> i32 {\n'
    '    let p = Point {  };\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":20}}},  # inside `Point { | }`
    lambda r: (
        check("offers field `x`", "x" in comp_labels(r), f"got {comp_labels(r)[:10]}"),
        check("offers field `y`", "y" in comp_labels(r), f"got {comp_labels(r)[:10]}"),
        check("no longer leaks primitive `i32`", "i32" not in comp_labels(r), f"got {comp_labels(r)[:10]}"),
    ))

case_feature("struct-literal completion excludes filled fields",
    'struct Point { x: i32, y: i32 }\n'
    'fn main() -> i32 {\n'
    '    let p = Point { x: 1,  };\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":26}}},  # after `x: 1, |`
    lambda r: (
        check("still offers `y`", "y" in comp_labels(r), f"got {comp_labels(r)[:10]}"),
        check("drops already-set `x`", "x" not in comp_labels(r), f"got {comp_labels(r)[:10]}"),
    ))

case_feature("struct-literal value slot is not field completion",
    'struct Point { x: i32, y: i32 }\n'
    'fn main() -> i32 {\n'
    '    let p = Point { x:  };\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":23}}},  # after `x: |` (value position)
    lambda r: check("field names not offered as values",
        "y" not in comp_labels(r) and "x" not in comp_labels(r), f"got {comp_labels(r)[:10]}"))

case_feature("member completion on inferred struct let",
    'struct Pt { x: i32, y: i32 }\n'
    'fn main() -> i32 {\n'
    '    let p = Pt { x: 1, y: 2 };\n'
    '    p.\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":3,"character":6}}},  # after `p.` (no annotation)
    lambda r: (
        check("`p.` offers field x", "x" in comp_labels(r), f"got {comp_labels(r)[:10]}"),
        check("`p.` offers field y", "y" in comp_labels(r), f"got {comp_labels(r)[:10]}"),
    ))

case_feature("import leaf module lists its symbols",
    'import stdlib::net::dns::\n'
    'fn main() -> i32 { return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":0,"character":25}}},  # after `stdlib::net::dns::`
    lambda r: check("offers `resolve` after `stdlib::net::dns::`",
        "resolve" in comp_labels(r), f"got {comp_labels(r)[:12]}"))

case_feature("member completion on module-qualified call (no import)",
    'fn main() -> i32 {\n'
    '    let dns = stdlib::net::dns::resolve("x");\n'
    '    dns.\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":8}}},  # after `dns.`
    lambda r: check("`stdlib::net::dns::resolve(...)` result completes ok/val/err",
        "val" in comp_labels(r) and "ok" in comp_labels(r), f"got {comp_labels(r)[:12]}"))

case_feature("chained completion past a Vector element method",
    'fn main() -> i32 {\n'
    '    let dns = stdlib::net::dns::resolve("x");\n'
    '    let z = dns.val.get(0).\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":27}}},  # after `dns.val.get(0).`
    lambda r: check("`vec.get(i).` resolves to the element and completes its methods",
        "to_string" in comp_labels(r) and "is_v4" in comp_labels(r), f"got {comp_labels(r)[:12]}"))

case_feature("combinator chain keeps completing (filter then get then element)",
    'fn ok_pred(x: i32) -> bool { return x > 0; }\n'
    'fn main() -> i32 {\n'
    '    let v: *Vector<i32> = Vector::new();\n'
    '    let z = v.filter(ok_pred).get(0).\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":3,"character":37}}},  # after `v.filter(ok_pred).get(0).`
    lambda r: check("`filter(...).get(0).` resolves through to the i32 element",
        "to_string" in comp_labels(r) and "abs" in comp_labels(r), f"got {comp_labels(r)[:12]}"))

case_feature("combinator filter preserves the Vector for further chaining",
    'fn ok_pred(x: i32) -> bool { return x > 0; }\n'
    'fn main() -> i32 {\n'
    '    let v: *Vector<i32> = Vector::new();\n'
    '    let z = v.filter(ok_pred).\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":3,"character":30}}},  # after `v.filter(ok_pred).`
    lambda r: check("`filter(...).` still offers Vector methods (len/get/map)",
        "len" in comp_labels(r) and "get" in comp_labels(r), f"got {comp_labels(r)[:12]}"))

case_feature("member completion on result `.val` (chained)",
    'import stdlib::net::dns::*;\n'
    'fn main() -> i32 {\n'
    '    let dns = resolve("x");\n'
    '    dns.val.\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":3,"character":12}}},  # after `dns.val.`
    lambda r: check("`result.val.` offers Vector methods (len/get)",
        "len" in comp_labels(r) and "get" in comp_labels(r), f"got {comp_labels(r)[:12]}"))

case_feature("result virtual fields ok val err carry documentation",
    'fn main() -> i32 {\n'
    '    let r: !i32 = "42".try_parse_int();\n'
    '    r.\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":6}}},  # after `r.`
    lambda r: (
        check("`ok` documents success/`.val`", "succeeded" in comp_doc(r, "ok"), f"got {comp_doc(r,'ok')[:40]!r}"),
        check("`val` documents the payload + `?`", "success value" in comp_doc(r, "val"), f"got {comp_doc(r,'val')[:40]!r}"),
        check("`err` documents the failure message", "failure message" in comp_doc(r, "err"), f"got {comp_doc(r,'err')[:40]!r}"),
    ))

case_feature("option virtual fields has val carry documentation",
    'fn main() -> i32 {\n'
    '    let m: ?i32 = some(7);\n'
    '    m.\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":6}}},  # after `m.`
    lambda r: (
        check("`has` documents some/none", "some" in comp_doc(r, "has"), f"got {comp_doc(r,'has')[:40]!r}"),
        check("`val` documents the contained value", "contained value" in comp_doc(r, "val"), f"got {comp_doc(r,'val')[:40]!r}"),
    ))

case_feature("Option and Result constructors offered with docs",
    'fn main() -> i32 {\n'
    '    o\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":1,"character":5}}},  # bare-ident position
    lambda r: (
        check("offers `ok` with docs", "ok" in comp_labels(r) and "Result" in comp_doc(r, "ok"),
              f"got {comp_doc(r,'ok')[:30]!r}"),
        check("offers `some` / `none` / `err`",
              all(c in comp_labels(r) for c in ("some","none","err")),
              f"got {[l for l in comp_labels(r) if l in ('some','none','ok','err')]}"),
    ))

case_feature("hover on the ok() constructor",
    'fn mk() -> !i32 { return ok(1); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/hover",
     "params":{"position":{"line":0,"character":26}}},  # on `ok` in `ok(1)`
    expect_hover("ok() constructor hover documents the Result", "Result"))

case_feature("member completion on for-in binder element",
    'import stdlib::net::dns::*;\n'
    'fn main() -> i32 {\n'
    '    let dns = resolve("x");\n'
    '    for ip in dns.val {\n'
    '        ip.\n'
    '    }\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":4,"character":11}}},  # after `ip.`
    lambda r: check("for-in binder `ip.` offers IpAddr methods (to_string/is_v4)",
        "to_string" in comp_labels(r) and "is_v4" in comp_labels(r), f"got {comp_labels(r)[:12]}"))

# ---- struct-literal field diagnostics ----

case_diagnostics("struct literal flags wrong field type",
    'struct Cfg { name: string, port: i32 }\n'
    'fn main() -> i32 { let c: Cfg = Cfg { name: 42, port: 8080 }; return c.port; }',
    expect_codes_present=["struct-field-type"])

case_diagnostics("struct literal accepts correct field types",
    'struct Cfg { name: string, port: i32 }\n'
    'fn main() -> i32 { let c: Cfg = Cfg { name: "x", port: 8080 }; return c.port; }',
    expect_codes_absent=["struct-field-type", "unknown-struct-field"])

case_diagnostics("struct literal flags unknown field",
    'struct Cfg { name: string }\n'
    'fn main() -> i32 { let c: Cfg = Cfg { nme: "x" }; return 0; }',
    expect_codes_present=["unknown-struct-field"])

case_diagnostics("struct literal flags raw value in optional field",
    'struct Cfg { name: string, port: ?i32 }\n'
    'fn main() -> i32 { let c = Cfg { name: "x", port: 32 }; return 0; }',
    expect_codes_present=["struct-field-type"])

case_diagnostics("struct literal accepts some() in optional field",
    'struct Cfg { name: string, port: ?i32 }\n'
    'fn main() -> i32 { let c = Cfg { name: "x", port: some(32) }; return c.port ?? 0; }',
    expect_codes_absent=["struct-field-type"])

case_diagnostics("struct literal requires non-optional fields",
    'struct Alelo { alelo: string, pinto: i32 }\n'
    'fn main() -> i32 { let a = Alelo { alelo: "" }; return 0; }',
    expect_codes_present=["missing-struct-field"])

case_diagnostics("struct literal allows omitting optional fields",
    'struct Cfg { host: string, port: ?i32 }\n'
    'fn main() -> i32 { let c = Cfg { host: "x" }; return 0; }',
    expect_codes_absent=["missing-struct-field"])

case_diagnostics("complete struct literal is clean",
    'struct Alelo { alelo: string, pinto: i32 }\n'
    'fn main() -> i32 { let a = Alelo { alelo: "x", pinto: 1 }; return 0; }',
    expect_codes_absent=["missing-struct-field", "struct-field-type", "unknown-struct-field"])

# ---- call hierarchy ----

def _call_hierarchy_test():
    print("\n[call hierarchy]")
    body = ('fn helper() -> i32 { return 1; }\n'
            'fn other() -> i32 { return 2; }\n'
            'fn main() -> i32 {\n'
            '    let a = helper();\n'
            '    let b = helper();\n'
            '    let c = other();\n'
            '    return a + b + c;\n'
            '}')
    path, uri = write_tmp("call_hierarchy.glide", body)
    def item(nm):
        z = {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 1}}
        return {"name": nm, "kind": 12, "uri": uri, "range": z, "selectionRange": z,
                "data": {"name": nm, "ctx": uri}}
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "glide", "version": 1, "text": body}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/prepareCallHierarchy",
         "params": {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 5}}},
        {"jsonrpc": "2.0", "id": 3, "method": "callHierarchy/incomingCalls",
         "params": {"item": item("helper")}},
        {"jsonrpc": "2.0", "id": 4, "method": "callHierarchy/outgoingCalls",
         "params": {"item": item("main")}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    rs = run_session(msgs)
    def by_id(i): return next((r for r in rs if r.get("id") == i), None)
    pres = (by_id(2) or {}).get("result") or []
    check("prepare returns the `helper` item",
          len(pres) == 1 and pres[0].get("name") == "helper", f"got {pres}")
    ires = (by_id(3) or {}).get("result") or []
    froms = {x.get("from", {}).get("name"): len(x.get("fromRanges", [])) for x in ires}
    check("incoming: `main` calls `helper` twice", froms.get("main") == 2, f"got {froms}")
    ores = (by_id(4) or {}).get("result") or []
    tos = {x.get("to", {}).get("name"): len(x.get("fromRanges", [])) for x in ores}
    check("outgoing: `main` -> `helper` (x2)", tos.get("helper") == 2, f"got {tos}")
    check("outgoing: `main` -> `other` (x1)", tos.get("other") == 1, f"got {tos}")

_call_hierarchy_test()

# ---- code action (quick fix) ----

def _code_action_test():
    print("\n[code action]")
    body = ('struct Cfg { name: string, port: ?i32 }\n'
            'fn main() -> i32 { let c = Cfg { name: "x", port: 32 }; return 0; }')
    path, uri = write_tmp("code_action.glide", body)
    init = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}
    opened = {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri, "languageId": "glide", "version": 1, "text": body}}}
    bye = {"jsonrpc": "2.0", "method": "exit", "params": None}
    # 1) collect the published diagnostic (with its data.fix)
    rs = run_session([init, opened, bye])
    diag = None
    for r in rs:
        if r.get("method") == "textDocument/publishDiagnostics":
            for d in r["params"]["diagnostics"]:
                if d.get("code") == "struct-field-type":
                    diag = d
    check("struct-field-type carries a data.fix",
          diag is not None and (diag.get("data") or {}).get("fix") is not None, f"got {diag}")
    if diag is None:
        return
    # 2) request code actions for that diagnostic
    ca = {"jsonrpc": "2.0", "id": 5, "method": "textDocument/codeAction",
          "params": {"textDocument": {"uri": uri}, "range": diag["range"],
                     "context": {"diagnostics": [diag]}}}
    rs2 = run_session([init, opened, ca, bye])
    act = next((r for r in rs2 if r.get("id") == 5), None)
    actions = (act or {}).get("result") or []
    titles = [a.get("title") for a in actions]
    check("offers a `Wrap in some(32)` quick fix",
          any("some(32)" in (t or "") for t in titles), f"got {titles}")
    check("action is linked to its diagnostic",
          bool(actions) and bool(actions[0].get("diagnostics")), f"got {actions[:1]}")
    edits = [te.get("newText") for a in actions
             for grp in (a.get("edit", {}).get("changes", {}) or {}).values() for te in grp]
    check("fix replaces the value with `some(32)`", "some(32)" in edits, f"got {edits}")

_code_action_test()

# ---- import path completion (packages + submodules) ----

case_completion_has("import lists the net package (dir, no top-level file)",
    'import stdlib::\nfn main() -> i32 { return 0; }',
    {"line":0,"character":15},
    ["net", "http", "hashmap"])

case_completion_has("import lists net submodules",
    'import stdlib::net::\nfn main() -> i32 { return 0; }',
    {"line":0,"character":20},
    ["listener", "tcp", "tls", "ip"])

case_completion_has("code stdlib path lists dir-only packages too",
    'fn main() -> i32 {\n'
    '    stdlib::\n'
    '    return 0;\n'
    '}',
    {"line":1,"character":12},   # after `stdlib::`
    ["net", "mail", "http"])

case_completion_has("module qualifier completes its members in code",
    'import stdlib::net::ip;\n'
    'fn main() -> i32 {\n'
    '    ip::\n'
    '    return 0;\n'
    '}',
    {"line":2,"character":8},   # after `ip::`
    ["SocketAddr", "IpAddr"])

case_diagnostics("module-qualified generic type annotation parses",
    'import stdlib::hashmap;\n'
    'fn main() -> i32 { let m: *hashmap::HashMap<i32> = hashmap::HashMap::new(); return 0; }',
    expect_codes_absent=["parse"])

# ---- summary ----
print()
passed = sum(1 for _, ok in results if ok)
total = len(results)
print(f"{passed}/{total} {'all green' if passed == total else 'failures'}")
sys.exit(0 if passed == total else 1)

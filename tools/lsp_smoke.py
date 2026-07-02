#!/usr/bin/env python3
"""End-to-end smoke test for the Glide LSP.

Spawns `glide lsp` as a subprocess, drives it through Content-Length-framed
JSON-RPC messages, and checks diagnostics + position-based features
(hover, definition, references, rename, completion, documentSymbol,
documentHighlight, formatting).
"""
import json, os, re, shutil, subprocess, sys, tempfile

GLIDE = os.environ.get("GLIDE") or (os.path.abspath("./glide.exe") if os.name == "nt" else "./glide")
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
    # Labels become filenames, so strip anything that isn't filename-safe
    # (a `/` in a label would otherwise be read as a directory separator).
    safe = re.sub(r"[^A-Za-z0-9._-]", "_", name)
    path = os.path.join(tempfile.gettempdir(), safe).replace(os.sep, "/")
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

# match is exhaustive like Rust: a Result match missing the `err` arm is flagged.
case_diagnostics("non-exhaustive Result match",
    'fn f() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 {\n'
    '    match f() {\n'
    '        ok(v) => { return v; }\n'
    '    }\n'
    '    return 0;\n'
    '}',
    expect_codes_present=["match-not-exhaustive"])

# An Option match with both arms is exhaustive -> no diagnostic.
case_diagnostics("exhaustive Option match is clean",
    'fn f() -> ?i32 { return some(1); }\n'
    'fn main() -> i32 {\n'
    '    match f() {\n'
    '        some(v) => { return v; }\n'
    '        none() => { return 0; }\n'
    '    }\n'
    '}',
    expect_codes_absent=["match-not-exhaustive"])

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

# ---- null-flow narrowing ----
case_diagnostics("null-flow: .val read inside an if-condition is flagged",
    'fn find() -> ?i32 { return some(1); }\n'
    'fn main() -> i32 { let m = find(); if m.val > 0 { return 1; } return 0; }',
    expect_codes_present=["ignored-option"])

case_diagnostics("null-flow: `if m.has && m.val` && guard is not flagged",
    'fn find() -> ?i32 { return some(1); }\n'
    'fn main() -> i32 { let m = find(); if m.has && m.val > 0 { return 1; } return 0; }',
    expect_codes_absent=["ignored-option"])

case_diagnostics("null-flow: reassign `m = some(v)` then .val is not flagged",
    'fn main() -> i32 { let mut m: ?i32 = none(); m = some(5); return m.val; }',
    expect_codes_absent=["ignored-option"])

case_diagnostics("null-flow: || early-exit guards both results",
    'fn p() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 { let a = p(); let b = p();\n'
    '    if !a.ok || !b.ok { return 0; }\n'
    '    return a.val + b.val; }',
    expect_codes_absent=["unchecked-result"])

case_diagnostics("null-flow: positive `if a.ok && b.ok` guards both in then-body",
    'fn p() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 { let a = p(); let b = p();\n'
    '    if a.ok && b.ok { return a.val + b.val; }\n'
    '    return 0; }',
    expect_codes_absent=["unchecked-result"])

case_diagnostics("unnecessary-mut: `&mut x` passed to a fn justifies the mut",
    'fn bump(p: *i32) { *p = *p + 1; }\n'
    'fn main() -> i32 { let mut x: i32 = 5; bump(&mut x); return x; }',
    expect_codes_absent=["unnecessary-mut"])

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

case_feature("member completion after a module-qualified call: fs::fs_read(...).",
    'import stdlib::fs;\n'
    'fn main() -> i32 {\n'
    '    let r = fs::fs_read("x").\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":29}}},  # after the trailing '.'
    lambda r: (lambda labs: check("string methods, not the global dump",
        "contains" in labs and "i32" not in labs and len(labs) < 100,
        f"got {len(labs)} items: {labs[:6]}"))([it.get("label") for it in (r.get("result",[]) if r else [])]))

case_feature("member completion after .unwrap() on a Result<string>",
    'fn getit() -> !string { return ok("hi"); }\n'
    'fn main() -> i32 {\n'
    '    let b = getit().unwrap().\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":29}}},  # after the trailing '.'
    lambda r: (lambda labs: check("string methods, not the global dump",
        "contains" in labs and "i32" not in labs and len(labs) < 100,
        f"got {len(labs)} items: {labs[:6]}"))([it.get("label") for it in (r.get("result",[]) if r else [])]))

case_feature("module-qualified free fn resolves to its OWN return type, not a Type_method-key collision",
    'import stdlib::fs;\n'
    'fn main() -> i32 {\n'
    '    let b = fs::read("x");\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/hover",
     "params":{"position":{"line":2,"character":9}}},
    lambda r: check("fs::read is !*ByteBuffer (not fs_read's string)",
        r and r.get("result") and "ByteBuffer" in r["result"]["contents"]["value"],
        f"got {r.get('result') if r else None}"))

case_feature("hover on a let bound to a module-qualified call is its return type, not *module",
    'import stdlib::fs;\n'
    'fn main() -> i32 {\n'
    '    let res = fs::fs_read("x");\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/hover",
     "params":{"position":{"line":2,"character":9}}},
    lambda r: check("hover shows string, not *fs",
        r and r.get("result") and "string" in r["result"]["contents"]["value"]
          and "*fs" not in r["result"]["contents"]["value"],
        f"got {r.get('result') if r else None}"))

case_feature("mod:: completion attaches an import edit for an un-imported module",
    'fn main() -> i32 {\n'
    '    fs::\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":1,"character":8}}},
    lambda r: check("fs_read carries an `import stdlib::fs` edit",
        r and any(it.get("label")=="fs_read" and it.get("additionalTextEdits")
            and "import stdlib::fs" in it["additionalTextEdits"][0]["newText"]
            for it in r.get("result",[])),
        "no import edit on fs_read"))

case_feature("mod:: completion adds no import edit when the module is already imported",
    'import stdlib::fs;\n'
    'fn main() -> i32 {\n'
    '    fs::\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
     "params":{"position":{"line":2,"character":8}}},
    lambda r: check("fs_read has no spurious import edit",
        r and any(it.get("label")=="fs_read" and not it.get("additionalTextEdits")
            for it in r.get("result",[])),
        "unexpected import edit on an already-imported module"))

case_feature("hover on .unwrap() shows an intrinsic card, not null",
    'fn getit() -> !string { return ok("hi"); }\n'
    'fn main() -> i32 {\n'
    '    let b = getit().unwrap();\n'
    '    return 0;\n'
    '}',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/hover",
     "params":{"position":{"line":2,"character":22}}},
    lambda r: check("hover documents unwrap()",
        r and r.get("result") and "unwrap()" in r["result"]["contents"]["value"],
        f"got {r.get('result') if r else None}"))

case_feature("references finds 2 sites",
    'fn helper() -> i32 { return 1; }\n'
    'fn main() -> i32 { return helper(); }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/references",
     "params":{"position":{"line":0,"character":4},"context":{"includeDeclaration":True}}},
    lambda r: check("references lists 2 sites",
        r and "result" in r and len(r["result"]) == 2,
        f"got {len(r['result']) if r and 'result' in r else 0}"))

# A `Type::method` reference must anchor at the METHOD segment, never the
# qualifier — a rename anchored at the qualifier silently overwrites the type
# name (data loss). `Foo::make()` in main starts at col 27 (Foo), 32 (make).
case_feature("references on Type::method anchor at the method (rename safety)",
    'struct Foo {}\n'
    'impl Foo { fn make() -> i32 { return 1; } }\n'
    'fn main() -> i32 { let x = Foo::make(); return 0; }',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/references",
     "params":{"position":{"line":2,"character":33},"context":{"includeDeclaration":True}}},
    lambda r: check("use anchors at `make` (32), not `Foo` (27)",
        r and "result" in r
          and any(l["range"]["start"]["line"]==2 and l["range"]["start"]["character"]==32 for l in r["result"])
          and not any(l["range"]["start"]["line"]==2 and l["range"]["start"]["character"]==27 for l in r["result"]),
        f"got {[(l['range']['start']['line'], l['range']['start']['character']) for l in (r.get('result',[]) if r else [])]}"))

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

# formatting must preserve source-only spellings: the `!field` export sigil
# and `Self` (parse-time Self-substitution is skipped in fmt mode).
case_feature("formatting preserves !field marker and Self",
    'pub struct Client {\n'
    '    !token: string,\n'
    '}\n'
    'impl Client {\n'
    '    pub fn new(token: string) -> Self {\n'
    '        return Self { token: token };\n'
    '    }\n'
    '}\n',
    {"jsonrpc":"2.0","id":2,"method":"textDocument/formatting",
     "params":{"options":{"tabSize":4,"insertSpaces":True}}},
    lambda r: (
        check("keeps `!token` export marker (not flipped to private)",
            r and r.get("result") and "!token: string" in r["result"][0].get("newText",""),
            f"got: {(r or {}).get('result')}"),
        check("keeps `-> Self` and `Self {` (not lowered to `Client`)",
            r and r.get("result")
                and "-> Self" in r["result"][0].get("newText","")
                and "Self {" in r["result"][0].get("newText",""),
            f"got: {(r or {}).get('result')}")))

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

# --- "did you mean" suggestions on unknown type / field / name / fn ---
case_diag_message("unknown type suggests closest primitive",
    'fn main() -> i32 { let x: strin = ""; return 0; }',
    "did you mean `string`?")
case_diag_message("unknown type suggests closest struct",
    'struct Client { x: i32 }\nfn main() -> i32 { let c: Cleint = Client { x: 1 }; return 0; }',
    "did you mean `Client`?")
case_diag_message("no field suggests closest field",
    'struct C { token: string }\nfn use_it(c: *C) -> string { return c.tokn; }',
    "did you mean `token`?")
case_diag_message("unknown name suggests closest binding",
    'fn main() -> i32 { let count: i32 = 5; let y: i32 = cont; return 0; }',
    "did you mean `count`?")
case_diag_message("unknown function suggests closest fn",
    'fn greeting() -> i32 { return 1; }\nfn main() -> i32 { let x: i32 = greting(); return 0; }',
    "did you mean `greeting`?")

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

def case_completion_absent(label, body, pos, absent_labels):
    """Single-file completion that must NOT offer the given labels."""
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
    res = (target or {}).get("result", []) or []
    items = res.get("items", []) if isinstance(res, dict) else res
    labels = set(it.get("label") for it in items)
    for lbl in absent_labels:
        check(f"completion does NOT offer `{lbl}`", lbl not in labels, "leaked")

def _proj_uri(d, rel=""):
    p = (d + "/" + rel) if rel else d
    p = p.rstrip("/").replace(os.sep, "/")
    if len(p) >= 2 and p[1] == ":":     # percent-encode the drive colon like editors do
        p = p[0].lower() + "%3A" + p[2:]
    return "file:///" + p

PROJ_MANIFEST = ('let manifest: Package = Package { name:"t", version:"0.1.0", '
                 'bin:"src/main.glide", deps: vec_of() };\n')

def case_completion_project(label, files, open_rel, pos,
                            present=None, absent=None, import_edit=None):
    """Completion inside a real multi-file project (rootUri + glide.glide), so
    cross-file behaviour (auto-import, the project index, visibility) is
    exercised. `files` maps repo-relative paths to contents; `import_edit` is
    (label, expected_newText) to assert the additionalTextEdits."""
    print(f"\n[project] {label}")
    d = tempfile.mkdtemp().replace(os.sep, "/")
    for rel, content in files.items():
        fp = os.path.join(d, *rel.split("/"))
        os.makedirs(os.path.dirname(fp), exist_ok=True)
        with open(fp, "w", encoding="utf-8", newline="") as f:
            f.write(content)
    open_uri = _proj_uri(d, open_rel)
    text = files[open_rel]
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":_proj_uri(d)}},
        {"jsonrpc":"2.0","method":"initialized","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":open_uri,"languageId":"glide","version":1,"text":text}}},
        {"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
         "params":{"textDocument":{"uri":open_uri},"position":pos}},
        {"jsonrpc":"2.0","method":"exit","params":None},
    ]
    rs = run_session(msgs)
    target = next((r for r in rs if r.get("id") == 2), None)
    res = (target or {}).get("result", []) or []
    items = res.get("items", []) if isinstance(res, dict) else res
    by_label = {it.get("label"): it for it in items}
    labels = set(by_label)
    for lbl in (present or []):
        check(f"offers `{lbl}`", lbl in labels,
              f"got {sorted(l for l in labels if l)[:14]}")
    for lbl in (absent or []):
        check(f"does NOT offer `{lbl}`", lbl not in labels, "leaked")
    if import_edit is not None:
        lbl, expect_text = import_edit
        edits = (by_label.get(lbl) or {}).get("additionalTextEdits") or []
        texts = [e.get("newText") for e in edits]
        check(f"`{lbl}` import edit yields {expect_text!r}",
              expect_text in texts, f"got {texts}")
    shutil.rmtree(d, ignore_errors=True)

def case_diagnostics_project(label, files, open_rel, present=None, absent=None):
    """Open `open_rel` inside a real multi-file project (rootUri + glide.glide)
    and assert the diagnostic codes published for it. Proves the declaration-
    level `unused-*` lints are cross-file-safe: a `pub` symbol defined here but
    used only from another file must NOT be flagged (the analysed file can't see
    its importers), while genuinely dead non-pub decls still fire."""
    print(f"\n[diag-project] {label}")
    d = tempfile.mkdtemp().replace(os.sep, "/")
    for rel, content in files.items():
        fp = os.path.join(d, *rel.split("/"))
        os.makedirs(os.path.dirname(fp), exist_ok=True)
        with open(fp, "w", encoding="utf-8", newline="") as f:
            f.write(content)
    open_uri = _proj_uri(d, open_rel)
    text = files[open_rel]
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":_proj_uri(d)}},
        {"jsonrpc":"2.0","method":"initialized","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":open_uri,"languageId":"glide","version":1,"text":text}}},
        {"jsonrpc":"2.0","method":"exit","params":None},
    ]
    rs = run_session(msgs)
    codes = set()
    for r in rs:
        if r.get("method") == "textDocument/publishDiagnostics" \
           and r["params"].get("uri") == open_uri:
            for dg in r["params"]["diagnostics"]:
                if dg.get("code"):
                    codes.add(dg["code"])
    for c in (present or []):
        check(f"emits `{c}`", c in codes, f"got {sorted(codes)}")
    for c in (absent or []):
        check(f"does NOT emit `{c}`", c not in codes, f"got {sorted(codes)}")
    shutil.rmtree(d, ignore_errors=True)

def case_hover_has(label, body, pos, expect_substr):
    """Single-file hover whose markdown must contain `expect_substr` (ci)."""
    print(f"\n[hover] {label}")
    path, uri = write_tmp(label.replace(" ", "_") + ".glide", body)
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
        {"jsonrpc":"2.0","id":2,"method":"textDocument/hover",
         "params":{"textDocument":{"uri":uri},"position":pos}},
        {"jsonrpc":"2.0","method":"exit","params":None},
    ]
    rs = run_session(msgs)
    target = next((r for r in rs if r.get("id") == 2), None)
    result = (target or {}).get("result") or {}
    contents = result.get("contents") if isinstance(result, dict) else None
    text = contents.get("value", "") if isinstance(contents, dict) else (contents or "")
    if isinstance(text, list):
        text = " ".join(str(x) for x in text)
    check(f"hover mentions {expect_substr!r}",
          expect_substr.lower() in str(text).lower(), f"got {str(text)[:90]!r}")

def case_definition_line(label, body, pos, expect_line):
    """Single-file goto-definition that must resolve to `expect_line` (0-based);
    pass expect_line=None to assert NO definition (e.g. an intrinsic)."""
    print(f"\n[definition] {label}")
    path, uri = write_tmp(label.replace(" ", "_") + ".glide", body)
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
            "textDocument":{"uri":uri,"languageId":"glide","version":1,"text":body}}},
        {"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
         "params":{"textDocument":{"uri":uri},"position":pos}},
        {"jsonrpc":"2.0","method":"exit","params":None},
    ]
    rs = run_session(msgs)
    target = next((r for r in rs if r.get("id") == 2), None)
    result = (target or {}).get("result")
    if isinstance(result, list):
        result = result[0] if result else None
    if expect_line is None:
        check("no definition (intrinsic)", not result, f"got {result}")
        return
    line = (result or {}).get("range", {}).get("start", {}).get("line") if result else None
    check(f"definition jumps to line {expect_line}", line == expect_line, f"got {line}")

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

# Member completion through `?` with let-inference: `let x = Foo::make()?`
# must resolve `x` to `*Foo` (unwrapping the `!*Foo`), so `x.` offers Foo's
# methods. Regression for the `?` inference gap that left completion silent.
case_completion_has("? inference offers receiver methods",
    'struct Foo {}\n'
    'impl Foo {\n'
    '    fn make() -> !*Foo { return ok(null as *Foo); }\n'
    '    fn greet(self: *Foo) -> i32 { return 1; }\n'
    '    fn wave(self: *Foo) -> i32 { return 2; }\n'
    '}\n'
    'fn run() -> ! {\n'
    '    let x = Foo::make()?;\n'
    '    x.\n'
    '    return ok(0);\n'
    '}',
    {"line":8,"character":6},
    ["greet","wave"])

# Vector member completion surfaces the new ergonomic methods (generic +
# specialized impls on Vector<i32>).
case_completion_has("Vector member completion offers new ergonomic methods",
    'fn run() -> i32 {\n'
    '    let v: *Vector<i32> = Vector::new();\n'
    '    v.\n'
    '    return 0;\n'
    '}',
    {"line":2,"character":6},
    ["sort","sort_by","contains","index_of","remove","insert","enumerate"])

# ---- context-aware match-arm completion (scrutinee-typed arms) ----

# Result scrutinee -> ok(v)/err(e) arms (the headline case).
case_completion_has("match arm completion offers ok/err for a Result",
    'fn find() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 {\n'
    '    let r = find();\n'
    '    match r {\n'
    '        \n'
    '    }\n'
    '    return 0;\n'
    '}',
    {"line":4,"character":8},
    ["ok(v) => {}","err(e) => {}"])

# Option scrutinee -> some(v)/none() arms.
case_completion_has("match arm completion offers some/none for an Option",
    'fn find() -> ?i32 { return some(1); }\n'
    'fn main() -> i32 {\n'
    '    match find() {\n'
    '        \n'
    '    }\n'
    '    return 0;\n'
    '}',
    {"line":3,"character":8},
    ["some(v) => {}","none() => {}"])

# Enum scrutinee (unannotated local) -> one arm per variant.
case_completion_has("match arm completion offers enum variants",
    'enum Color { Red, Green, Blue }\n'
    'fn main() -> i32 {\n'
    '    let c = Color::Red;\n'
    '    match c {\n'
    '        \n'
    '    }\n'
    '    return 0;\n'
    '}',
    {"line":4,"character":8},
    ["Red => {}","Green => {}","Blue => {}"])

# A non-matchable scrutinee (int) must NOT get bogus arms — normal completion.
case_completion_absent("match arm completion stays quiet for an int scrutinee",
    'fn main() -> i32 {\n'
    '    let n = 5;\n'
    '    match n {\n'
    '        \n'
    '    }\n'
    '    return 0;\n'
    '}',
    {"line":3,"character":8},
    ["ok(v) => {}","some(v) => {}"])

# Dedup: an arm already written (`ok(v) => {}`) is not offered again.
case_completion_absent("match arm completion skips an already-written arm",
    'fn f() -> !i32 { return ok(1); }\n'
    'fn main() -> i32 {\n'
    '    match f() {\n'
    '        ok(v) => {}\n'
    '        \n'
    '    }\n'
    '    return 0;\n'
    '}',
    {"line":4,"character":8},
    ["ok(v) => {}"])

# ---- expected-type completion in a struct-literal field value ----

# `Style { color: | }` where `color: Color` -> the enum's variants on top.
case_completion_has("struct field value offers the field type's values",
    'enum Color { Red, Green, Blue }\n'
    'struct Style { color: Color, width: i32 }\n'
    'fn main() -> i32 {\n'
    '    let s = Style {\n'
    '        color: \n'
    '    };\n'
    '    return 0;\n'
    '}',
    {"line":4,"character":15},
    ["Color::Red","Color::Green","Color::Blue"])

# An `if` condition expects bool -> offer true/false up front.
case_completion_has("if condition offers true/false",
    'fn main() -> i32 {\n'
    '    let flag: bool = true;\n'
    '    if  {\n'
    '        return 1;\n'
    '    }\n'
    '    return 0;\n'
    '}',
    {"line":2,"character":7},
    ["true","false"])

# Method argument resolves against the RECEIVER's type (correct overload):
# `p.set(|)` where `Pen::set(self, c: Color)` -> Color's variants, even though a
# builtin `Vector::set(i, x)` shares the method name.
case_completion_has("method arg resolves the receiver-type overload",
    'enum Color { Red, Green, Blue }\n'
    'struct Pen { c: i32 }\n'
    'impl Pen {\n'
    '    pub fn set(self: *Pen, c: Color) {}\n'
    '}\n'
    'fn main() -> i32 {\n'
    '    let p: *Pen = malloc(8) as *Pen;\n'
    '    p.set();\n'
    '    return 0;\n'
    '}',
    {"line":7,"character":10},
    ["Color::Red","Color::Green","Color::Blue"])

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

# ---- type hierarchy ----

def _type_hierarchy_test():
    print("\n[type hierarchy]")
    body = ('trait Animal { fn speak(self: *Self) -> i32; }\n'
            'trait Pet: Animal { fn name(self: *Self) -> i32; }\n'
            'struct Dog { age: i32 }\n'
            'impl Pet for Dog {\n'
            '    fn speak(self: *Dog) -> i32 { return 1; }\n'
            '    fn name(self: *Dog) -> i32 { return 2; }\n'
            '}')
    path, uri = write_tmp("type_hierarchy.glide", body)
    def item(nm):
        z = {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 1}}
        return {"name": nm, "kind": 11, "uri": uri, "range": z, "selectionRange": z,
                "data": {"name": nm, "ctx": uri}}
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "glide", "version": 1, "text": body}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/prepareTypeHierarchy",
         "params": {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 7}}},
        {"jsonrpc": "2.0", "id": 3, "method": "typeHierarchy/supertypes", "params": {"item": item("Pet")}},
        {"jsonrpc": "2.0", "id": 4, "method": "typeHierarchy/subtypes", "params": {"item": item("Pet")}},
        {"jsonrpc": "2.0", "id": 5, "method": "typeHierarchy/supertypes", "params": {"item": item("Dog")}},
        {"jsonrpc": "2.0", "id": 6, "method": "typeHierarchy/subtypes", "params": {"item": item("Dog")}},
        {"jsonrpc": "2.0", "id": 7, "method": "typeHierarchy/subtypes", "params": {"item": item("Animal")}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    rs = run_session(msgs)
    def by_id(i): return next((r for r in rs if r.get("id") == i), None)
    def names(i): return sorted(x.get("name") for x in ((by_id(i) or {}).get("result") or []))
    pres = (by_id(2) or {}).get("result") or []
    check("prepare on `Pet` returns the trait item",
          len(pres) == 1 and pres[0].get("name") == "Pet" and pres[0].get("kind") == 11, f"got {pres}")
    check("supertypes(Pet) -> Animal (supertrait)", names(3) == ["Animal"], f"got {names(3)}")
    check("subtypes(Pet) -> Dog (implementor)", names(4) == ["Dog"], f"got {names(4)}")
    check("supertypes(Dog) -> Pet (implemented trait)", names(5) == ["Pet"], f"got {names(5)}")
    check("subtypes(Dog) -> [] (no struct subtyping)", names(6) == [], f"got {names(6)}")
    check("subtypes(Animal) -> Pet (sub-trait)", names(7) == ["Pet"], f"got {names(7)}")

_type_hierarchy_test()

# ---- warning squiggle width ----

def _warn_width_test():
    print("\n[warning squiggle width]")
    body = ('fn f() -> i32 {\n'
            '    let mut counter: i32 = 5;\n'
            '    return counter;\n'
            '}')
    path, uri = write_tmp("warn_width.glide", body)
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "glide", "version": 1, "text": body}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    rs = run_session(msgs)
    diags = []
    for r in rs:
        if r.get("method") == "textDocument/publishDiagnostics":
            diags = r["params"]["diagnostics"]; break
    um = next((d for d in diags if d.get("code") == "unnecessary-mut"), None)
    w = (um["range"]["end"]["character"] - um["range"]["start"]["character"]) if um else 0
    check("unnecessary-mut underlines the whole name (width=len('counter')=7)",
          w == 7, f"got width {w}")

_warn_width_test()

# ---- relatedInformation: bounds + borrows ----

def _diags_for(label, body):
    path, uri = write_tmp(label + ".glide", body)
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "glide", "version": 1, "text": body}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    for r in run_session(msgs):
        if r.get("method") == "textDocument/publishDiagnostics":
            return r["params"]["diagnostics"]
    return []

def _related_info_test():
    print("\n[relatedInformation: bounds + borrows]")
    bound = ('trait Render { fn draw(self: *Self) -> i32; }\n'
             'fn show<T: Render>(x: T) -> i32 { return 0; }\n'
             'struct Foo { x: i32 }\n'
             'fn main() -> i32 { let f: Foo = Foo { x: 1 }; return show(f); }')
    db = next((d for d in _diags_for("rel_bound", bound) if d.get("code") == "unsatisfied-bound"), None)
    rib = (db or {}).get("relatedInformation") or []
    check("unsatisfied-bound points at the bound declaration",
          len(rib) == 1 and "bound is declared here" in rib[0].get("message", ""), f"got {rib}")

    borrow = ('fn use2(p: *i32, q: *i32) -> i32 { return *p + *q; }\n'
              'fn main() -> i32 {\n'
              '    let mut x: i32 = 5;\n'
              '    let a: *i32 = &mut x;\n'
              '    let b: *i32 = &mut x;\n'
              '    return use2(a, b);\n'
              '}')
    dr = next((d for d in _diags_for("rel_borrow", borrow) if d.get("code") == "overlap-borrow"), None)
    rir = (dr or {}).get("relatedInformation") or []
    ok_line = len(rir) == 1 and rir[0]["location"]["range"]["start"]["line"] == 3
    check("overlap-borrow points at the first borrow (line 4)",
          ok_line and "first borrowed here" in rir[0].get("message", ""), f"got {rir}")

_related_info_test()

# ---- pkg! manifest macro ----

def _pkg_macro_test():
    print("\n[pkg! manifest macro]")
    # pkg!(...) resolves to a string literal at expand time, so a `: string`
    # annotation type-checks and `.len()` is valid — no errors. (The smoke
    # runs from the repo, which has a glide.glide, so it resolves to that.)
    body = ('fn main() -> i32 {\n'
            '    let v: string = pkg!("version");\n'
            '    let n: string = pkg!("name");\n'
            '    return v.len() + n.len();\n'
            '}')
    path, uri = write_tmp("pkg_macro.glide", body)
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "glide", "version": 1, "text": body}}},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    diags = []
    for r in run_session(msgs):
        if r.get("method") == "textDocument/publishDiagnostics":
            diags = r["params"]["diagnostics"]; break
    errs = [d for d in diags if d.get("severity") == 1]
    check("pkg!(...) resolves to a string (no errors)",
          len(errs) == 0, f"got {[(d.get('code'), d.get('message','')[:40]) for d in errs]}")

_pkg_macro_test()

def _panic_macro_test():
    print("\n[panic-family macros]")
    body = ('fn f(n: i32) -> i32 {\n'
            '    assert!(n > 0);\n'
            '    assert!(n < 100, "too big");\n'
            '    if n == 0 { panic!("zero"); }\n'
            '    if n == 1 { todo!(); }\n'
            '    if n == 2 { unreachable!(); }\n'
            '    if n == 3 { unimplemented!(); }\n'
            '    return n;\n'
            '}')
    errs = [d for d in _diags_for("panic_family", body) if d.get("severity") == 1]
    check("panic!/todo!/unreachable!/unimplemented!/assert! resolve (no errors)",
          len(errs) == 0, f"got {[(d.get('code'), d.get('message','')[:40]) for d in errs]}")

_panic_macro_test()

def _dbg_macro_test():
    print("\n[dbg! macro]")
    # dbg!(40) must type as i32 and dbg!("x") as string, so the annotations
    # match and the file is error-free — proves dbg! carries the arg's type.
    body = 'fn main() -> i32 { let n: i32 = dbg!(40); let s: string = dbg!("x"); return n + s.len(); }'
    errs = [d for d in _diags_for("dbg_macro", body) if d.get("severity") == 1]
    check("dbg!(x) carries x's type (no errors)",
          len(errs) == 0, f"got {[(d.get('code'), d.get('message','')[:40]) for d in errs]}")

_dbg_macro_test()

case_diagnostics("pkg! unknown field is flagged",
    'fn main() -> i32 { return pkg!("verison").len(); }',
    expect_codes_present=["unknown-pkg-field"])

case_diagnostics("cfg! resolves to a bool condition (no errors)",
    'fn main() -> i32 { if cfg!("windows") { return 1; } if cfg!("posix") { return 2; } return 0; }',
    expect_codes_absent=["unknown-cfg"])

case_diagnostics("cfg! unknown condition is flagged",
    'fn main() -> i32 { if cfg!("windoze") { return 1; } return 0; }',
    expect_codes_present=["unknown-cfg"])

case_diagnostics("cfg arch conditions x86_64 aarch64 are valid",
    'fn main() -> i32 { if cfg!("x86_64") { return 1; } if cfg!("aarch64") { return 2; } return 0; }',
    expect_codes_absent=["unknown-cfg"])

case_diagnostics("env! resolves to a string (no errors)",
    'fn main() -> i32 { let v: string = env!("PATH"); return v.len(); }',
    expect_codes_absent=["unknown-cfg","unknown-pkg-field"])

_pkg_body = 'fn main() -> i32 { let v: string = pkg!(""); return 0; }'
_pkg_col = _pkg_body.index('pkg!("') + len('pkg!("')
case_feature("pkg arg completes the manifest field names",
    _pkg_body,
    {"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion",
     "params": {"position": {"line": 0, "character": _pkg_col}}},
    lambda r: check("offers name/version/author/license/description/repository",
                    {"name", "version", "author", "license", "description", "repository"}
                        .issubset(set(comp_labels(r))),
                    f"got {comp_labels(r)}"))

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

def _suggestion_fix_test():
    """A `did you mean` diagnostic carries a quick fix that replaces the typo
    with the suggestion (unknown type here; same path for name/fn/field)."""
    print("\n[code action] did-you-mean suggestion fix")
    body = 'fn main() -> i32 {\n    let x: Strng = "hi";\n    let _ = x;\n    return 0;\n}\n'
    path, uri = write_tmp("suggest_fix.glide", body)
    init = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}
    opened = {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri, "languageId": "glide", "version": 1, "text": body}}}
    bye = {"jsonrpc": "2.0", "method": "exit", "params": None}
    rs = run_session([init, opened, bye])
    diag = None
    for r in rs:
        if r.get("method") == "textDocument/publishDiagnostics":
            for d in r["params"]["diagnostics"]:
                if d.get("code") == "unknown-type":
                    diag = d
    check("unknown-type carries a data.fix",
          diag is not None and (diag.get("data") or {}).get("fix") is not None, f"got {diag}")
    if diag is None:
        return
    ca = {"jsonrpc": "2.0", "id": 5, "method": "textDocument/codeAction",
          "params": {"textDocument": {"uri": uri}, "range": diag["range"],
                     "context": {"diagnostics": [diag]}}}
    rs2 = run_session([init, opened, ca, bye])
    act = next((r for r in rs2 if r.get("id") == 5), None)
    actions = (act or {}).get("result") or []
    titles = [a.get("title") for a in actions]
    check("offers a `Change to `string`` quick fix",
          any("Change to `string`" in (t or "") for t in titles), f"got {titles}")
    edits = [te.get("newText") for a in actions
             for grp in (a.get("edit", {}).get("changes", {}) or {}).values() for te in grp]
    check("fix replaces `Strng` with `string`", "string" in edits, f"got {edits}")

_suggestion_fix_test()

def _insert_fix_test(label, body, code, want_title, want_newtext):
    """A diagnostic carrying an insert/delete fix surfaces as a code action."""
    print(f"\n[code action] {label}")
    path, uri = write_tmp(label.replace(" ", "_") + ".glide", body)
    init = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}
    opened = {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri, "languageId": "glide", "version": 1, "text": body}}}
    bye = {"jsonrpc": "2.0", "method": "exit", "params": None}
    rs = run_session([init, opened, bye])
    diag = None
    for r in rs:
        if r.get("method") == "textDocument/publishDiagnostics":
            for d in r["params"]["diagnostics"]:
                if d.get("code") == code:
                    diag = d
    check(f"`{code}` carries a data.fix",
          diag is not None and (diag.get("data") or {}).get("fix") is not None, f"got {diag}")
    if diag is None:
        return
    ca = {"jsonrpc": "2.0", "id": 5, "method": "textDocument/codeAction",
          "params": {"textDocument": {"uri": uri}, "range": diag["range"],
                     "context": {"diagnostics": [diag]}}}
    rs2 = run_session([init, opened, ca, bye])
    act = next((r for r in rs2 if r.get("id") == 5), None)
    actions = (act or {}).get("result") or []
    titles = [a.get("title") for a in actions]
    check(f"offers `{want_title}`", any(want_title in (t or "") for t in titles), f"got {titles}")
    edits = [te.get("newText") for a in actions
             for grp in (a.get("edit", {}).get("changes", {}) or {}).values() for te in grp]
    check(f"fix newText is {want_newtext!r}", want_newtext in edits, f"got {edits}")

# Missing `;` -> `expected-semicolon` code + an "Insert `;`" quick fix.
case_diagnostics("missing semicolon yields expected-semicolon code",
    'fn main() -> i32 {\n    let x: i32 = 5\n    return x;\n}\n',
    expect_codes_present=["expected-semicolon"])
_insert_fix_test("semicolon insert fix",
    'fn main() -> i32 {\n    let x: i32 = 5\n    return x;\n}\n',
    "expected-semicolon", "Insert `;`", ";")
# Unnecessary `mut` -> a "Remove `mut`" quick fix that deletes `mut `.
_insert_fix_test("unnecessary-mut remove fix",
    'fn main() -> i32 {\n    let mut y: i32 = 3;\n    return y;\n}\n',
    "unnecessary-mut", "Remove `mut`", "")

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

# ============================================================================
# Regression guards for the 2026-05-31 LSP / macro / qualified-type session.
# (helpers above: case_completion_absent / case_completion_project /
#  case_hover_has / case_definition_line)
# ============================================================================

# --- attribute-name completion surfaces @suggest / @leaf / @used / @section ---
case_completion_has("attribute completion offers suggest/leaf/used/section",
    '@\npub fn f() -> i32 { return 0; }',
    {"line":0,"character":1},
    ["suggest","leaf","used","section","cfg","derive"])

# --- builtin compiler macros complete (pkg!, cfg!, env!, panic!, dbg!, …) ---
case_completion_has("builtin macros complete (pkg!/cfg!/dbg!/panic!)",
    'fn main() {\n    let x = pkg\n}',
    {"line":1,"character":15},
    ["pkg!","cfg!","env!","panic!","dbg!","todo!","assert!"])

# --- builtin macro hover documents the macro ---
case_hover_has("pkg! hover documents the manifest macro",
    'fn main() {\n    let x = pkg!("name");\n}',
    {"line":1,"character":13},
    "manifest")

# --- user `macro name!` hover + goto resolve at the call site ---
case_hover_has("user macro hover shows its signature",
    'macro twice!($x:expr) { $x; $x }\nfn main() {\n    twice!(println!("a"));\n}',
    {"line":2,"character":5},
    "twice")
case_definition_line("user macro goto jumps to its def",
    'macro twice!($x:expr) { $x; $x }\nfn main() {\n    twice!(println!("a"));\n}',
    {"line":2,"character":5},
    0)
# Builtin compiler macros are intrinsic — hover documents them, goto has no
# source location to resolve to (and must not crash / mis-resolve).
case_definition_line("builtin macro goto resolves to no location",
    'fn main() {\n    let x = pkg!("name");\n}',
    {"line":1,"character":13},
    None)

# --- cross-file auto-import MERGES a sibling pub fn into an existing select ---
_UTIL = ('pub fn shown() -> i32 { return 1; }\n'
         'pub fn extra() -> i32 { return 2; }\n'
         'fn hidden() -> i32 { return 3; }\n')
case_completion_project("auto-import merges a sibling pub fn into {shown}",
    {"glide.glide": PROJ_MANIFEST, "src/util.glide": _UTIL,
     "src/main.glide": 'import util::{shown};\nfn main() {\n    let a = ext\n}\n'},
    "src/main.glide", {"line":2,"character":15},
    present=["extra"], import_edit=("extra", ", extra"))
# --- non-pub sibling decl must NOT leak into bare-ident completion ---
case_completion_project("non-pub sibling decl stays hidden (import present)",
    {"glide.glide": PROJ_MANIFEST, "src/util.glide": _UTIL,
     "src/main.glide": 'import util::{shown};\nfn main() {\n    let b = hid\n}\n'},
    "src/main.glide", {"line":2,"character":15},
    absent=["hidden"])

# --- cross-file member completion respects field privacy ---
# `client.` from another file must offer the `pub` field + the method, but NOT
# the private field (reading it cross-file is a hard error).
_PRIV = ('pub struct Client {\n    pub name: string,\n    secret: string,\n}\n\n'
         'impl Client {\n    pub fn reveal(self: *Client) -> string { return self.secret; }\n}\n')
case_completion_project("cross-file member completion hides private field",
    {"glide.glide": PROJ_MANIFEST, "src/core.glide": _PRIV,
     "src/main.glide": 'import core;\nfn use_it(c: *core::Client) -> string {\n    c.\n}\n'},
    "src/main.glide", {"line":2,"character":6},
    present=["name", "reveal"], absent=["secret"])
# Same-file access still offers the private field (in-file completion).
case_completion_has("same-file member completion offers private field",
    'struct Client {\n    pub name: string,\n    secret: string,\n}\n'
    'fn use_it(c: *Client) -> string {\n    return c.\n}\n',
    {"line":5,"character":13}, ["name", "secret"])
# Reading a private field cross-file is now a hard error, not a warning.
case_diagnostics_project("cross-file private field access is an error",
    {"glide.glide": PROJ_MANIFEST, "src/core.glide": _PRIV,
     "src/main.glide": 'import core;\nfn use_it(c: *core::Client) -> string {\n    return c.secret;\n}\n'},
    "src/main.glide", present=["private-field"])


# ---- workflow-authored comprehensive cases (regression-suite-buildout) ----
# completion: attrs, builtin macros, @suggest/type-aware args, module-member
# no-leak, auto-import fresh/merge/non-pub; hover/goto: builtin+user+stdlib
# macros, Type::method right impl.

ATTR_BODY = "@\nfn main() {}\n"
case_completion_has("attr_bare", ATTR_BODY, {"line":0,"character":1},
    ["cfg","derive","expect","allow","deprecated","lint","proc_attr","proc_derive","proc_macro","proc_macro_str","suggest","leaf","used","section"])

MAC_BODY = "fn main() {\n    let x = ;\n}\n"
case_completion_has("macros_builtin", MAC_BODY, {"line":1,"character":12},
    ["pkg!","cfg!","env!","panic!","todo!","unimplemented!","unreachable!","dbg!","assert!","file!","line!","column!","function!"])

SUG_BODY = ('struct Engine { rpm: i32 }\n'
            'enum Color { Red, Green, Blue }\n'
            'impl Engine {\n'
            '    @suggest(mode, "fast", "slow")\n'
            '    pub fn run(self, mode: string) -> i32 { return 0; }\n'
            '    pub fn paint(self, c: Color) -> i32 { return 0; }\n'
            '}\n'
            'fn main() {\n'
            '    let e: Engine = Engine { rpm: 0 };\n'
            '    e.run("");\n'
            '    e.paint();\n'
            '}\n')
case_completion_has("suggest_method_quotes", SUG_BODY, {"line":9,"character":11}, ["fast","slow"])
case_completion_has("suggest_enum_param", SUG_BODY, {"line":10,"character":12}, ["Color::Red","Color::Green","Color::Blue"])

files_mm = {
  "glide.glide": PROJ_MANIFEST,
  "src/foo.glide": "pub fn alpha() -> i32 { return 1; }\n",
  "src/x/foo.glide": "pub fn beta() -> i32 { return 2; }\n",
  "src/main.glide": "import foo;\nfn main() {\n    foo::\n}\n",
}
case_completion_project("modmember_sameleaf", files_mm, "src/main.glide",
    {"line":2,"character":9}, present=["alpha"], absent=["beta"])

AL_SRC = ('pub fn custom() -> i32 { return 1; }\n'
          'pub fn other() -> i32 { return 2; }\n'
          'fn secret_helper() -> i32 { return 3; }\n')
files_a = {
  "glide.glide": PROJ_MANIFEST,
  "src/al.glide": AL_SRC,
  "src/main.glide": "fn main() {\n    custom\n}\n",
}
case_completion_project("autoimport_fresh", files_a, "src/main.glide",
    {"line":1,"character":10}, present=["custom"],
    import_edit=("custom", "import al::{custom};\n"))
files_b = {
  "glide.glide": PROJ_MANIFEST,
  "src/al.glide": AL_SRC,
  "src/main.glide": "import al::{other};\nfn main() {\n    custom\n}\n",
}
case_completion_project("autoimport_merge", files_b, "src/main.glide",
    {"line":2,"character":10}, present=["custom"],
    import_edit=("custom", ", custom"))
files_c = {
  "glide.glide": PROJ_MANIFEST,
  "src/al.glide": AL_SRC,
  "src/main.glide": "fn main() {\n    secret_helper\n}\n",
}
case_completion_project("autoimport_nonpub_hidden", files_c, "src/main.glide",
    {"line":1,"character":17}, absent=["secret_helper"])

case_hover_has("hover_pkg",
  'fn main() {\n  let n = pkg!("name");\n}\n',
  {"line":1,"character":11},
  "manifest")
case_hover_has("hover_cfg",
  'fn main() {\n  let n = cfg!("os");\n}\n',
  {"line":1,"character":11},
  "Compile-time configuration predicate")
case_hover_has("hover_env",
  'fn main() {\n  let n = env!("PATH");\n}\n',
  {"line":1,"character":11},
  "environment variable")
case_hover_has("hover_panic",
  'fn main() {\n  panic!("boom");\n}\n',
  {"line":1,"character":4},
  "Abort the program")
case_hover_has("hover_todo",
  'fn main() {\n  todo!();\n}\n',
  {"line":1,"character":3},
  "not yet implemented")
case_hover_has("hover_unimplemented",
  'fn main() {\n  unimplemented!();\n}\n',
  {"line":1,"character":4},
  "intentionally not implemented")
case_hover_has("hover_unreachable",
  'fn main() {\n  unreachable!();\n}\n',
  {"line":1,"character":4},
  "never be taken")
case_hover_has("hover_dbg",
  'fn main() {\n  let x = dbg!(1);\n}\n',
  {"line":1,"character":11},
  "drops into an expression")
case_hover_has("hover_assert_builtin",
  'fn main() {\n  assert!(true);\n}\n',
  {"line":1,"character":4},
  "Panic with the source location")
case_hover_has("hover_file",
  'fn main() {\n  let f = file!();\n}\n',
  {"line":1,"character":11},
  "current source file path")
case_hover_has("hover_line",
  'fn main() {\n  let l = line!();\n}\n',
  {"line":1,"character":11},
  "current source line number")
case_hover_has("hover_column",
  'fn main() {\n  let c = column!();\n}\n',
  {"line":1,"character":11},
  "current source column")
case_hover_has("hover_function",
  'fn main() {\n  let fn1 = function!();\n}\n',
  {"line":1,"character":13},
  "enclosing function")
case_hover_has("hover_user_macro",
  'macro twice!($x:expr) {\n  $x + $x\n}\nfn main() {\n  let y = twice!(3);\n}\n',
  {"line":4,"character":11},
  "macro twice!")
case_definition_line("goto_user_macro",
  'macro twice!($x:expr) {\n  $x + $x\n}\nfn main() {\n  let y = twice!(3);\n}\n',
  {"line":4,"character":11},
  0)
case_hover_has("hover_stdlib_assert",
  'import stdlib::testing::*;\nfn main() {\n  assert!(true);\n}\n',
  {"line":2,"character":4},
  "Panic")
case_definition_line("goto_builtin_none",
  'fn main() {\n  panic!("x");\n}\n',
  {"line":1,"character":4},
  None)
case_hover_has("hover_foo_make",
  'struct Foo { pub a: i32 }\nimpl Foo { fn make(self) -> i32 { return 1; } }\nstruct Bar { pub b: i32 }\nimpl Bar { fn make(self) -> i32 { return 2; } }\nfn main() {\n  let f: Foo = Foo { a: 0 };\n  let x = Foo::make(f);\n}\n',
  {"line":6,"character":15},
  "impl Foo")
case_hover_has("hover_bar_make",
  'struct Foo { pub a: i32 }\nimpl Foo { fn make(self) -> i32 { return 1; } }\nstruct Bar { pub b: i32 }\nimpl Bar { fn make(self) -> i32 { return 2; } }\nfn main() {\n  let b: Bar = Bar { b: 0 };\n  let x = Bar::make(b);\n}\n',
  {"line":6,"character":15},
  "impl Bar")

# ---- `Self` inside an impl resolves to the concrete implementor ----
# Completion inside `Self { … }` offers the implementor's fields; hover and
# goto-def on `Self` (both `-> Self` and `Self { }`) resolve to the struct.
SELF_BODY = ('pub struct Client {\n'
             '    token: string,\n'
             '}\n'
             '\n'
             'impl Client {\n'
             '    pub fn new(token: string) -> Self {\n'
             '        return Self {\n'
             '            token: token\n'
             '        };\n'
             '    }\n'
             '}\n')
case_completion_has("Self literal offers implementor fields", SELF_BODY,
  {"line":7,"character":12}, ["token"])
case_hover_has("hover Self (return type) resolves to implementor", SELF_BODY,
  {"line":5,"character":33}, "Client")
case_hover_has("hover Self (literal) resolves to implementor", SELF_BODY,
  {"line":6,"character":16}, "Client")
case_definition_line("goto Self (return type) jumps to struct", SELF_BODY,
  {"line":5,"character":33}, 0)
case_definition_line("goto Self (literal) jumps to struct", SELF_BODY,
  {"line":6,"character":16}, 0)
# `Self` outside any impl keeps the generic keyword card (no false resolution).
case_hover_has("hover Self outside impl keeps keyword doc",
  'fn f() -> Self {\n    return 0;\n}\n',
  {"line":0,"character":11}, "implementing type")

# `Self::` inside an impl method offers the implementor's static methods, the
# same way the concrete `TypeName::` does (Self used to resolve to nothing).
SELF_PATH_BODY = ('struct Point {\n'
                  '    x: i32,\n'
                  '}\n'
                  'impl Point {\n'
                  '    fn zero() -> Point { return Point { x: 0 }; }\n'
                  '    fn go(self: *Point) -> i32 { let p = Self::\n'
                  'return 0; }\n'
                  '}\n')
case_completion_has("Self:: offers implementor static methods", SELF_PATH_BODY,
  {"line":5,"character":47}, ["zero"])

# `self.` member access offers the implementor's fields + methods whether the
# receiver is annotated `self: *T` or a bare `self`.
SELF_DOT_BODY = ('struct G {\n'
                 '    count: i32,\n'
                 '    name: string,\n'
                 '}\n'
                 'impl G {\n'
                 '    pub fn run(self: *G) -> i32 {\n'
                 '        self.\n'
                 '        return 0;\n'
                 '    }\n'
                 '}\n')
case_completion_has("self. offers implementor fields and methods", SELF_DOT_BODY,
  {"line":6,"character":13}, ["count", "name", "run"])

# A `Self { }` literal whose implementor isn't a resolvable struct must offer an
# EMPTY list, never the global primitive/type/fn bag.
case_completion_absent("Self {} with unresolvable target offers no garbage",
  'impl Ghost {\n    fn make() -> Self {\n        return Self {\n\n        };\n    }\n}\n',
  {"line":3,"character":0}, ["i32", "string", "Vector", "println!"])

# A user struct shadowing a same-named builtin (`Pair`) offers ITS OWN fields in
# a `Self { }` literal, not the builtin's.
case_completion_has("Self {} prefers user struct over same-named builtin",
  'struct Pair {\n    a: i32,\n    b: i32,\n}\nimpl Pair {\n    fn make() -> Self {\n        return Self {\n\n        };\n    }\n}\n',
  {"line":7,"character":0}, ["a", "b"])
case_completion_absent("Self {} user-struct shadow hides builtin fields",
  'struct Pair {\n    a: i32,\n    b: i32,\n}\nimpl Pair {\n    fn make() -> Self {\n        return Self {\n\n        };\n    }\n}\n',
  {"line":7,"character":0}, ["first", "second"])

# A struct-literal field value referencing an unknown name must be flagged by
# the typer, not slip through to a codegen ICE. Field shorthand `Self { a }`
# (sugar for `Self { a: a }`) with no local `a` is the easy way to hit it.
case_diagnostics('struct-literal field shorthand with unknown name errors',
    'pub struct Github {\n    a: string,\n}\nimpl Github {\n    pub fn new() -> Self {\n        return Self {\n             a\n        };\n    }\n}\nfn main() -> i32 { return 0; }\n',
    expect_codes_present=['unknown-name'])
case_diagnostics('struct-literal field shorthand with a real local is fine',
    'struct G {\n    a: string,\n    b: i32,\n}\nfn main() -> i32 {\n    let a: string = "x";\n    let b: i32 = 1;\n    let g: G = G { a, b };\n    let _ = g.a;\n    let _ = g.b;\n    return 0;\n}\n',
    expect_codes_absent=['unknown-name'])

# A struct-literal field whose value type doesn't match the declared field type
# (wrong struct, or pointer-vs-value) is a `field-type-mismatch`. Covers the
# `Self { client: HttpClient::new() }` shape (Self resolves to the implementor).
case_diagnostics('struct-literal field value wrong-struct / pointer mismatch errors',
    'struct HttpResponse { code: i32 }\n'
    'struct HttpClientRequest { url: string }\n'
    'impl HttpClient { pub fn new() -> *HttpClient { return malloc(sizeof(HttpClient)) as *HttpClient; } }\n'
    'struct HttpClient { id: i32 }\n'
    'impl HttpClientRequest { pub fn new(u: string) -> *HttpClientRequest { return malloc(sizeof(HttpClientRequest)) as *HttpClientRequest; } }\n'
    'struct GithubRequest { client: *HttpResponse, req: HttpClientRequest }\n'
    'impl GithubRequest {\n'
    '    pub fn new() -> Self {\n'
    '        return Self { client: HttpClient::new(), req: HttpClientRequest::new("u") };\n'
    '    }\n'
    '}\n',
    expect_codes_present=['field-type-mismatch'])
case_diagnostics('struct-literal with matching field value types is fine',
    'struct A { x: i32 }\n'
    'impl A { pub fn new() -> *A { return malloc(sizeof(A)) as *A; } }\n'
    'struct W { a: *A, n: i32 }\n'
    'fn main() -> i32 {\n    let w: W = W { a: A::new(), n: 1 };\n    let _ = w.n;\n    return 0;\n}\n',
    expect_codes_absent=['field-type-mismatch'])
case_diagnostics('generic struct literal is never field-type-checked (no false positive)',
    'struct Box<T> { value: T, count: i32 }\n'
    'impl Box<T> {\n    pub fn make(v: T) -> Self {\n        return Self { value: v, count: 0 };\n    }\n}\n',
    expect_codes_absent=['field-type-mismatch'])


# ---- workflow-authored 'unused / dead / redundant' lint cases ----
# unused-struct / unused-enum / unused-const / unused-variant / redundant-import.

case_diagnostics('unused struct is flagged',
    'struct Unused {\n    x: i32,\n    y: i32,\n}\n\nfn main() {\n    let n: i32 = 1;\n    let _ = n;\n}\n',
    expect_codes_present=['unused-struct'])

case_diagnostics('used struct (literal + annotation) is not flagged',
    'struct Point {\n    x: i32,\n    y: i32,\n}\n\nfn make() -> Point {\n    return Point { x: 1, y: 2 };\n}\n\nfn main() {\n    let p: Point = make();\n    let _ = p.x;\n}\n',
    expect_codes_absent=['unused-struct'])

case_diagnostics('pub struct is never flagged even if unused locally',
    'pub struct Config {\n    name: i32,\n}\n\nfn main() {\n    let n: i32 = 0;\n    let _ = n;\n}\n',
    expect_codes_absent=['unused-struct'])

case_diagnostics('struct with an impl block is treated as used',
    'struct Counter {\n    n: i32,\n}\n\nimpl Counter {\n    fn get(self: *Counter) -> i32 { return self.n; }\n}\n\nfn main() {\n    let n: i32 = 0;\n    let _ = n;\n}\n',
    expect_codes_absent=['unused-struct'])

case_diagnostics('struct used only via pointer annotation is not flagged',
    'struct Node {\n    v: i32,\n}\n\nfn touch(p: *Node) -> i32 {\n    return p.v;\n}\n\nfn main() {\n    let n: i32 = 0;\n    let _ = n;\n}\n',
    expect_codes_absent=['unused-struct'])

case_diagnostics('unused enum is flagged',
    'enum Lonely {\n    A,\n    B,\n}\n\nfn main() {\n    let n: i32 = 1;\n    let _ = n;\n}\n',
    expect_codes_present=['unused-enum'])

case_diagnostics('enum used via variant construction is not flagged',
    'enum Color {\n    Red,\n    Green,\n}\n\nfn main() {\n    let c: Color = Color::Red;\n    let _ = c;\n}\n',
    expect_codes_absent=['unused-enum'])

case_diagnostics('pub enum is never flagged even if unused locally',
    'pub enum Status {\n    On,\n    Off,\n}\n\nfn main() {\n    let n: i32 = 0;\n    let _ = n;\n}\n',
    expect_codes_absent=['unused-enum'])

case_diagnostics('enum with an impl block is treated as used',
    'enum Dir {\n    Up,\n    Down,\n}\n\nimpl Dir {\n    fn flip(self: *Dir) -> i32 { return 0; }\n}\n\nfn main() {\n    let n: i32 = 0;\n    let _ = n;\n}\n',
    expect_codes_absent=['unused-enum'])

case_diagnostics('enum used only as a return-type annotation is not flagged',
    'enum Mode {\n    Fast,\n    Slow,\n}\n\nfn pick() -> Mode {\n    return Mode::Fast;\n}\n\nfn main() {\n    let m: Mode = pick();\n    let _ = m;\n}\n',
    expect_codes_absent=['unused-enum'])

case_diagnostics('unused-const fires on a dead private const',
    'const UNUSED_CONST: i32 = 42;\nconst USED_CONST: i32 = 7;\n\nfn main() {\n    let _x: i32 = USED_CONST + 1;\n    return;\n}\n',
    expect_codes_present=['unused-const'])

case_diagnostics('pub const stays silent (public API)',
    'pub const EXPORTED: i32 = 99;\n\nfn main() {\n    return;\n}\n',
    expect_codes_absent=['unused-const'])

case_diagnostics('const referenced only by another const is used (no warning)',
    'const BASE: i32 = 10;\nconst DERIVED: i32 = BASE + 5;\n\nfn use_it() -> i32 {\n    return DERIVED;\n}\n\nfn main() {\n    let _z: i32 = use_it();\n    return;\n}\n',
    expect_codes_absent=['unused-const'])

case_diagnostics('@allow("unused-const") silences the warning',
    '@allow("unused-const")\nconst KEPT_FOR_LATER: i32 = 5;\n\nfn main() {\n    return;\n}\n',
    expect_codes_absent=['unused-const'])

case_diagnostics('redundant exact duplicate import',
    'import stdlib::math;\nimport stdlib::math;\n\nfn main() -> i32 {\n    let x: i64 = math::abs_i64(-5);\n    println!(x);\n    return 0;\n}',
    expect_codes_present=['redundant-import'], expect_codes_absent=['unused-import'])

case_diagnostics('bare plus wildcard of same module is redundant',
    'import stdlib::math;\nimport stdlib::math::*;\n\nfn main() -> i32 {\n    let x: i64 = math::abs_i64(-5);\n    println!(x);\n    return 0;\n}',
    expect_codes_present=['redundant-import'])

case_diagnostics('single import is not redundant (negative)',
    'import stdlib::math;\n\nfn main() -> i32 {\n    let x: i64 = math::abs_i64(-5);\n    println!(x);\n    return 0;\n}',
    expect_codes_absent=['redundant-import'])

case_diagnostics('two distinct modules are not redundant (negative)',
    'import stdlib::math;\nimport stdlib::strings;\n\nfn main() -> i32 {\n    let x: i64 = math::abs_i64(-5);\n    println!(x);\n    return 0;\n}',
    expect_codes_absent=['redundant-import'])

case_diagnostics('variant never constructed nor matched warns (enum otherwise used)',
    'enum State { Idle, Running, Crashed }\nfn step(s: State) -> i32 { match s { State::Idle => { return 0; } State::Running => { return 1; } _ => { return 9; } } }\nfn main() { let s: State = State::Idle; let _ = step(s); }',
    expect_codes_present=['unused-variant'], expect_codes_absent=['unused-enum'])

case_diagnostics('all variants constructed or matched -> silent',
    'enum State { Idle, Running }\nfn step(s: State) -> i32 { match s { State::Idle => { return 0; } State::Running => { return 1; } } }\nfn main() { let a: State = State::Idle; let b: State = State::Running; let _ = step(a); let _ = step(b); }',
    expect_codes_absent=['unused-variant'])

case_diagnostics('pub enum variants are API -> silent even if unmatched',
    'pub enum Pub { A, B, C }\nfn main() { let _: Pub = Pub::A; }',
    expect_codes_absent=['unused-variant'])

# ---- cross-file lint safety (the `pub` skip is the only cross-file guard) ----
_UTIL_PUB = ('pub struct Widget { pub n: i32 }\n'
             'pub const LIMIT: i32 = 10;\n'
             'pub fn mk() -> Widget { return Widget { n: LIMIT }; }\n'
             'pub enum Color { Red, Green }\n')
_MAIN_USES = ('import util::{Widget, mk};\n'
              'fn main() { let w: Widget = mk(); println!(w.n); }\n')
# Open util.glide: its pub Widget/LIMIT/mk/Color are used ONLY from main.glide,
# which util can't see — they must NOT be flagged as unused.
case_diagnostics_project("pub symbols used cross-file are not flagged unused",
    {"glide.glide": PROJ_MANIFEST, "src/util.glide": _UTIL_PUB, "src/main.glide": _MAIN_USES},
    "src/util.glide",
    absent=["unused-struct", "unused-enum", "unused-const", "unused-fn"])
# A genuinely dead non-pub decl in the same file still fires; the pub ones stay
# silent. (A non-pub decl can't be used cross-file, so single-file reasoning is
# sound.)
case_diagnostics_project("dead non-pub still flagged while cross-file pub stays silent",
    {"glide.glide": PROJ_MANIFEST,
     "src/util.glide": _UTIL_PUB + "fn dead_one() -> i32 { return 0; }\nstruct PrivDead { x: i32 }\n",
     "src/main.glide": _MAIN_USES},
    "src/util.glide",
    present=["unused-fn", "unused-struct"],
    absent=["unused-enum", "unused-const"])

# ---- summary ----
print()
passed = sum(1 for _, ok in results if ok)
total = len(results)
print(f"{passed}/{total} {'all green' if passed == total else 'failures'}")
sys.exit(0 if passed == total else 1)

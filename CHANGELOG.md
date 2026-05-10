# Changelog

## 0.1.0 — 2026-05-10

### Language

- **`if/else` as expression**: `let x = if cond { a } else { b };` now
  produces a value at any expression position. Both branches must be a
  single expression of the same type; codegen lowers it to a C ternary
  so there's no extra runtime cost. `else` is required.
  Statement-position `if` (with `{ stmt; stmt; }` blocks) is unchanged.
- **`match` as expression**: same pattern — `let v = match e { Foo => a, Bar => b };`.
  Each arm body terminates in `return val;`; arm types must match.
- **block as expression**: `let x = { stmt; stmt; return val; };` —
  yields the `return val` expression, lets and side effects run before.
- **Integer-literal widening**: literals that don't fit in 32-bit `int`
  (e.g. `19_999_999_998`) now infer as `i64` instead of silently
  truncating. The most visible effect was inside macros like
  `assert_eq!` that did `let __t = $a;` and dropped i64 constants on
  the floor.
- **Tuple-struct sugar**: `struct ApiKey(key: string)` is sugar for
  `struct ApiKey { pub key: string }`. Implicit-pub on every field;
  brace form stays the way to declare private fields.
- **`?T` / `!T` / `?!T`**: option, result, and option-of-result types
  with `?` postfix propagation, polymorphic `?`, and `??` coalesce
  operator. `none → err` coercion when `?T` flows into a `!U` return.
- **`defer_err`**: `defer_err <expr>;` runs only on err-return paths
  (`return err(...)` and `?` propagation). Pairs with `defer` for
  cleanups that fire on every exit.
- **Associated types in traits**: `type Item;` in a trait, `type Item = T;`
  in the impl. `Self::Item` resolves at impl-site.
- **Trait `<T: A + B>` bounds**: enforced at call sites; checked across
  generic fn / generic struct / generic impl.
- **`*dyn Trait` dispatch**: trait-object fat pointer (vtable + data).
  Compares to `null`, assigns from concrete `*T`, and round-trips
  through fn params + struct fields.
- **`select!` block**: `select!` over `chan<T>` arms — `recv`, `send`,
  `Some/None recv`, `default`. Polling-loop lowering today; true
  parking deferred.
- **inline asm**: `asm [volatile] { "instr" : outs : ins : clobbers }`
  GCC-style; pairs with `@cfg("...")` for per-platform fns.
- **naked fns**: `@naked fn foo() { asm { … } }` skips prologue/epilogue.
- **string interpolation**: `format!("user={user.name}, age={user.age}")`
  with field access in placeholders.
- **`@cfg("windows" | "posix")` attribute**: gates fns + structs by
  target OS. Compiles into `#ifdef _WIN32` / `#ifndef _WIN32`.
- **`c_raw! { … }` blocks**: raw C/asm payload emitted verbatim. Used
  heavily in stdlib for low-level helpers (signal handlers, atomics,
  pthread, OpenSSL, zlib, time).

### Procedural macros

- **`macro_rules!`-style**: user-defined `macro name!(matchers) { body }`
  with `:expr`, `:ty`, `:ident` matchers and `$(... ),*` repetition.
- **Type-attached macros**: `impl T { macro name!(...) }` with
  receiver-style invocation `recv.name!(args)` and qualified
  `Type::name!(args)` form.
- **Procedural macros (Phases 0–5)**: AST-typed + raw-string flavours,
  embedded interpreter (no dlopen), same-module dispatch:
  - `@proc_derive(Name)` — derives from struct annotation
    (`@derive(Name) struct Foo { … }`)
  - `@proc_attr(Name)` — modifies an annotated decl
    (`@<name> fn foo() { … }`)
  - `@proc_macro(Name)` / `@proc_macro_str(Name)` — fn-like macros
- **Hygiene by default**: ident emitted via `expr_ident` gets a
  per-call suffix (`__macro_<id>__name`) so macros can't collide
  with caller-side names. Opt-out via `expr_ident_unsafe` /
  `stmt_let_unsafe`.
- **Dual-site error diagnostics**: macro fn body line + use-site line
  reported on every diagnostic, with `it.diagnostics` channel.
- **`@derive(JsonBind)`**: ships in stdlib::json::derive — derives
  the `JsonBind` trait for primitive-field structs (string, int,
  bool, f64, plus their `?T` variants).
- **`@handler`**: ships in stdlib::http::handler — Axum-style typed
  handler ergonomics. See HTTP section below.

### HTTP stack

A complete HTTP stack landed:

- **`stdlib::net`**: TCP, UDP, DNS, IP types, TLS via OpenSSL,
  HTTP/1.1 client (http://) and HTTPS, WebSocket (ws + wss),
  HTTP/2 (HPACK + frames + ALPN, Phase D).
- **`stdlib::http`**: `HttpRequest`, `HttpResponse` (chainable
  builder), `http_listen` (single-worker), `http_listen_workers`
  (multi-worker via SO_REUSEPORT), `https_listen`. Set replaces
  same-name headers; `add` appends; `cookie` routes through `add`.
  CRLF injection blocked in `set` / `cookie`. Lazy header cache on
  HttpRequest. Chunked-encoding writer for streaming responses.
- **`stdlib::http::router`**: method-aware Router with `:param` +
  `*wildcard` segments, `r.use_mw(mw)` middleware (Express-style
  `Chain` + `chain_next`), `r.scope(prefix, sub)` nested routers,
  `r.state(p)` shared state slot, `..` path traversal block at
  dispatch.
- **`stdlib::http::typed` + `stdlib::http::handler`**: `Json<T>`
  wrapper, `JsonBind` trait, `IntoResponse` trait, `json_respond`,
  `@handler` proc-attr macro for typed param binding + auto 400/422.
- **`stdlib::http::extract`**: `FromRequest` trait + extractors:
  `*HttpRequest`, `Bearer`, `Headers`, `Authorization<S: AuthScheme>`,
  `Basic`, `Path<T: FromPath>`, `State<T>`. Error → status mapping
  via `"<code>:<msg>"` prefix on err strings. `@handler` accepts
  unlimited typed params via this trait.
- **`stdlib::http::cors`**: CORS middleware via global config slot;
  `install_cors(cfg)` + `cors_mw`; preflight 204 + ACAO/ACAM/ACAH.
- **`stdlib::http::static`**: `serve_dir(r, opts)` — wildcard route
  mounting, mime detect, 404/403 traversal block.
- **`stdlib::http::jwt`**: HS256 verify with `JwtClaims` (sub/exp/
  iat/raw); error prefixes for `@handler` extract path.
- **`stdlib::http::multipart`**: builder + `MultipartForm` parser
  (`FromRequest` impl) — `fields` / `files` / `UploadedFile`.
- **`stdlib::http::compress`**: `gzip_mw` middleware via zlib;
  skips small bodies + already-compressed Content-Types + responses
  without `Accept-Encoding: gzip`.
- **`stdlib::http::sse`**: SSE wire format helper + `sse_response`;
  builds on the chunked streaming path.
- **`stdlib::http::client`**: `HttpClient` with redirects, cookies,
  forms, multipart, timeouts, basic + bearer auth.

### JSON

- **Method-style API** (replaces free-fn style):
  `JsonValue::object()` / `int(n)` / `string(s)` / `parse(s)` ctors
  on the type; `v.obj_set(k, x)` / `v.arr_push(x)` / `v.get(k)` /
  `v.emit()` instance methods. Typed accessors:
  `v.get_string(k)?` / `v.get_int(k)?` / `v.get_bool(k)?` /
  `v.get_float(k)?` / `v.opt_<t>(k)`.
- **`JsonBind` trait**: two-way struct ⇄ JSON binding;
  `from_json(v: *JsonValue) -> !Self` / `to_json(self: *Self) -> *JsonValue`.
- **`@derive(JsonBind)`**: auto-impls for primitive-field structs.
- **`impl<T: JsonBind> JsonBind for Vector<T>`**: blanket impl;
  `Vector<Pet>` round-trips with element-error propagation via `?`.

### Stdlib expansions

- **`stdlib::testing`**: macro-driven assertion framework + per-file
  synth main + `glide test` / `glide test --golden` runners with
  CRLF-normalised diffs.
- **`stdlib::time`**: Time/Duration with Weekday/Month, Stopwatch,
  `after` / `tick` channels, `format(spec)`, `truncate` / `round`,
  `parse_rfc3339`.
- **`stdlib::os` (31 fns)**: host + process identity, env, dirs;
  `!T` error model.
- **`stdlib::env`**: `env_set` / `unset` / `all` / `expand` + `EnvKV`.
- **`stdlib::process` v1**: `Command` builder + `Child` +
  `process_kill` / `exists`. fork+exec POSIX, CreateProcess Win.
- **`stdlib::signal` v1**: `signal_chan` + `select!` integration via
  POSIX self-pipe (Linux/macOS/BSD) or `SetConsoleCtrlHandler`
  (Windows). `signal_raise` for self-trigger tests.
- **`stdlib::iter` (16 combinators)**: eager combinators over
  `Vector<T>`. `<T, U>` generic fn with fn-pointer args end-to-end.
- **`Vector<T>` chain methods + reducers**: `v.map(f).filter(p).sum()`.
  11 generic methods + 3 specialised int reducers (sum / max / min).
- **`stdlib::sync`**: `Mutex<T>`, `Atomic`, `WaitGroup` via pthread +
  C11 atomics through `c_raw`.
- **`stdlib::crypto`**: SHA-256 + HMAC-SHA-256 (RFC 6234 + 2104,
  NIST vectors verified).
- **`stdlib::argparse`**: pure-Glide CLI flag + positional parser
  (~370 LOC). Long + short flags, 3 types, `--help` auto-generated.
- **`stdlib::mail`**: SMTP / POP3 / IMAP clients + RFC 5322 message +
  MIME builder.

### Stdlib internals

- **Slim `runtime/stdlib.c`** (174 lines, ABI only): fs/os/env/io
  primitives moved to `c_raw` blocks inside the matching stdlib
  modules. Older glide binaries that still carry the bodies in their
  embedded stdlib.c don't see the new defines, so `c_raw` blocks stay
  inactive during a bootstrap-step rebuild.
- **`builtins/` vs `stdlib/` split**: `src/builtins/` is auto-injected
  into every compile (3 files); `src/stdlib/` requires explicit
  `import "src/stdlib/X.glide"`.

### Concurrency

- **M:N coroutines**: `spawn` submits a coroutine to the M:N
  scheduler; `spawn_thread` keeps the explicit pthread escape hatch.
- **Runtime architecture v3**: own asm context switch, Vyukov chan,
  work-stealing, lazy mmap stacks, task pool. Reactor with epoll on
  Linux; serial fallback on Windows / macOS / BSD until IOCP/kqueue
  land.
- **HTTP perf state**: 200k req/s single-worker on bare metal Ubuntu
  i3-8100 (86% of nginx). WSL numbers are virtualisation artifacts.

### Compiler fixes

- **Forward-decl pass for generic monos**: `*Vector<*X>` returns
  monomorphise cleanly even when the inner type is itself generic.
- **Type-tree `subst` no longer mutates input**: trait sigs are
  shared across impls; clone-on-descend instead of polluting
  downstream.
- **`@handler` nested-generic monomorphisation**: `Json<Vector<T>>`
  propagates T through `Json::wrap.into_response()` chains.
  `let_ty` hint pinning at macro emit sites.
- **`*dyn Trait` runtime ordering**: vtable + thunk forward decls
  emit AFTER `__glide_option_<T>_t` / `__glide_result_<T>_t`
  typedefs, so trait methods returning `?T` / `!T` don't collide
  with implicit-int. Thunk auto-detects `self: *Self` vs `self: Self`
  for cast vs deref.
- **`*dyn Trait` null compare / assign / struct-lit init**: fat-
  pointer struct gets zero-init (`(__glide_dyn_T){0}`) instead of
  literal NULL; `x == null` translates to `x.vtable == NULL`.
- **`stmt_impl_trait` + `expr_struct_lit` interp intrinsics**: lets
  proc-derive macros emit trait impls + struct literals without
  field-setter intrinsics.
- **`?T` / `!T` constants exposed in interp**: `UN_TRY`,
  `OP_COALESCE`, `EX_STRUCT_LIT` etc. addressable from macro fn
  bodies.
- **HTTP/2 NUL-truncated read pipeline**: binary-safe via
  `TlsStream::read_bytes` + H2Conn ByteBuffer rx ring + HPACK
  explicit-len (frame headers `00 00 XX` no longer truncate to .len()=0).

### Known issues

- **`mail_smtp` / `mail_pop3` / `mail_imap` tests fail**: the
  in-process pattern (spawn_thread fake server + main connect)
  trips a runtime TCP recv error on the same OS thread. The mail
  clients themselves work against real servers; only the
  in-process integration tests fail. Real fix needs investigation
  of reactor + spawn_thread interaction.

### LSP performance

The LSP path was rewritten around an arena allocator after a session of
chasing per-keystroke leaks that crept the long-running `glide lsp`
process toward 30+ GiB during normal editing.

- **Per-keystroke arena**: every Vector / HashMap / AST node allocated
  by parse / expand / lower / type runs in a chunked bump arena owned
  by the active document. The next reanalysis frees the prior arena
  in bulk via `munmap` / `VirtualFree`, returning pages straight to
  the OS. Cap chunk size at 256 MiB so a 1 GiB parse no longer
  reserves 2 GiB of slack.
- **Per-request arena**: completion, hover, documentHighlight, goto,
  rename, formatting all run inside a transient arena that's
  reclaimed when the handler returns. Zed's "fire completion on every
  character" pattern was the dominant leak; on a 5000-stmt union it
  cost 150-300 MiB per request that nothing freed.
- **Cached lower output**: `load_into_with_cache` now lowers each
  imported file at populate time and stores the typer-ready stmts.
  The user-pass lower (`lower_program_user_only`) skips top-level
  stmts whose origin doesn't match the user buffer, so editing a
  small file with stdlib imports doesn't reallocate fn_body /
  then_body / else_body / impl_methods Vectors for the whole stdlib
  on every keystroke.
- **O(N) jp_unescape**: the JSON parser's escape-decoding path was
  O(N²) (concat-byte-by-byte). On Zed's didChange payloads (full
  file content with escaped newlines) it allocated multi-GiB of
  throwaway strings per request. Replaced with a single arena buffer
  that's written byte-by-byte then cast to string.
- **Heap cache keys**: parse_cache keys now strdup'd into libc heap
  so they outlive every keystroke's arena reset. Without this the
  second didOpen segfaulted on cache.contains lookup against a freed
  string.
- **Runtime string ops via __glide_palloc**: `__glide_string_concat`,
  `_substring`, `_format`, `_int_to_string`, `_char_to_string` route
  through the active arena instead of libc malloc. Outside the LSP
  the arena is null and they fall back to calloc, so build / run /
  fmt paths see no behavior change.
- **JSON tree freed**: `lsp_main` now calls `json_free(req)` on the
  parsed request tree and `free` on the raw read buffer at the end
  of each loop iteration. `handle_did_open` / `handle_did_change`
  free the prior `doc.text` after replacement.
- **Lexer / Parser / Typer**: `__glide_pfree` (free that's safe on
  arena pointers) now used in their `.free()` methods so calling
  them while inside an arena is a no-op.

Smoke tests with realistic Zed-style traffic (didChange + completion
+ documentHighlight per keystroke):

  - bootstrap/main.glide (entry point, imports the whole bootstrap):
    1.3 GiB baseline (the AST cache itself), stable across 50 iters
    where the previous build climbed to 15 GiB.
  - bootstrap/lsp.glide: 30 MiB total, stable.

The 1.3 GiB baseline for main.glide-shaped graphs is the AST cache
for transitively-imported files (~70 MiB / file × 18 files). Reducing
it further needs a smaller AST representation or a lazy-import scheme
that doesn't parse symbol bodies until they're queried.

## 0.0.1 (preview) — 2026-05-05

Early preview tagged at the bootstrap milestone (now superseded by 0.1.0).
Glide is a self-hosted, plug-and-play systems language with no Rust or
system C compiler required.

### Language

- **Memory model**: function-scoped borrows (no lifetime annotations), auto-drop
  via `let v* = …`, arenas, `defer`. Borrow checker enforces dangling-return,
  call-site aliasing, double-borrow-across-stmt, and four other invariants.
- **Errors as values**: `!T` result type, `?` propagation, `ok` / `err` builtins.
- **Generics**: monomorphized; inference from args + return hints + first method
  call on a `let v = Generic::new()` binding. Bounds: `<T: A + B>` checked at
  call sites.
- **Traits**: `trait` + `impl Trait for T`, `Self`, default methods,
  supertraits, `*dyn Trait` runtime dispatch via vtable + thunks.
- **Concurrency**: M:N coroutines (`spawn`) on top of a custom asm context
  switch (~10–15 ns), Vyukov MPMC chan with cache-padded cells, work-stealing,
  `sleep_ms` async (parks coro, frees worker), `while let v = c.recv()`
  close-detect.
- **Closures**: anonymous `fn(args) -> ret { … }` lifted to top-level
  (capture-less), passable as `fn(int) -> int` parameter.
- **Macros**: `macro name!($x:expr) { … }` non-variadic + `$($x:expr),*` variadic;
  call sites at stmt position, method position (`v.push_all!(…)`), and path
  position (`Type::name!(…)`). Expander runs between parse and typer.
- **Inline asm**: `asm { "…" : "=a"(out), "…"(in) }` GCC-style operand lists.
  `naked fn` for raw-asm bodies. `@cfg("posix"|"windows")` attribute. `c_raw! { … }`
  emits arbitrary C verbatim into the output.
- **String interpolation**: `"hello, ${name}!"` lowers to `format!(…)` at parse.
- **Imports**: bare path `import a::b::c;`, wildcard `import a::b::*;`, brace list
  `import a::b::{X, Y};`. Multi-segment paths auto-resolve to module files.

### Toolchain

- Single archive bundles everything: the `glide` binary, the auto-injected
  `src/builtins/`, the opt-in `src/stdlib/`, and a Zig toolchain (clang + libcs
  + linker for cross-compile).
- Subcommands: `glide build / run / emit / check / fmt / ast / lsp`.
- Cross-compilation via `--target=<triple>` (e.g. `x86_64-linux-gnu`,
  `aarch64-macos`, `x86_64-windows-gnu`).
- Output binary is a normal native exe with no Glide / Zig runtime dependency.
- `tools/install.{sh,ps1}` install the archive into `~/.local/share/glide`
  (Linux/macOS) or `%LOCALAPPDATA%\Programs\Glide` (Windows). No admin required.

### LSP

`glide lsp` over JSON-RPC stdio. Supports:

- **Diagnostics** with stable codes covering type errors, borrow rules,
  null-safety, unused vars / fns / params, unnecessary `mut`, dead code after
  return, missing return, arena leaks, `&temporary`, missing trait method,
  unsatisfied bound, trait-method-mismatch.
- **Hover** with doc-comment extraction for user fns/structs and built-in docs
  for keywords / macros.
- **Completion** with `.` and `:` trigger characters: locals, top-level decls,
  keywords, chan ops on `c.`, `Type::method` paths, struct fields.
- **Goto definition** for fns, struct fields, impl methods (including
  `Vector::new`, `c.send`, chained inline ctors, qualified paths).
- **Find references / document highlight** across the open file.
- **Document symbols** for outline.
- **Rename + prepareRename** (rejects keywords; precise word range).
- **Document formatting** via fmt.glide round-trip.

### Editors

- **Zed**: extension under `zed-extension/` invokes `glide lsp`. Tree-sitter
  grammar at `glide-grammar/` covers the full surface (traits, dyn, asm,
  naked, c_raw, @cfg, generic bounds, qualified imports).
- **VSCode**: extension under `vscode-extension/` with file-extension icon,
  syntax highlighting (TextMate), and LSP integration via
  `vscode-languageclient`.

### Bootstrap

- Compiler is ~12K LOC of Glide under `bootstrap/`.
- `bootstrap/seed/bootstrap.c` is the auto-emitted C seed for fresh machines:
  `cc bootstrap/seed/bootstrap.c -o glide_seed && ./glide_seed build bootstrap/main.glide -o glide`.
- Runtime impl externalized to `bootstrap/runtime/` (chan template, sched, sockets,
  prelude, stdlib helpers).

### Performance vs Go 1.26 (Windows 11, median of 5 runs)

| bench                       | Glide     | Go     | result                |
|-----------------------------|-----------|--------|-----------------------|
| Spawn + drain 100K          | 6 ms      | 10 ms  | Glide 1.7× faster     |
| Spawn + drain 1M            | 85 ms     | 94 ms  | Glide 1.1× faster     |
| Pure chan 1M (cap = 1024)   | 24 ms     | 48 ms  | Glide 2.0× faster     |
| RAM idle 100K parked        | 448 MB    | 903 MB | Glide 2.0× lighter    |
| Throughput spawn+chan 100K  | 486 ms    | 436 ms | Glide 1.1× off        |

### Known limitations

- Formatter drops comments (lexer doesn't track them yet); `glide fmt --write`
  is opt-in for that reason.
- No NLL — borrow lifetimes are block-scoped.
- No `move` returns — owned values can't escape their declaring fn.
- No `?T` nullable type yet — only borrows are non-null by check; `*T` remains
  nullable.
- Default trait method copy with mixed override / inherit patterns can produce
  incorrect codegen (workaround: override in every impl). Tracked.
- `let v* = Vector::new()` followed by use through a borrow doesn't infer `T`
  (workaround: explicit `let v: *Vector<int> = …`). Tracked.
- Reactor / async I/O, `async fn`, stack growth, `Mutex<T>`,
  `select!` over multiple chans: deferred to a future release.

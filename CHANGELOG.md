# Changelog

## unreleased

### Cross-platform

- **kqueue reactor backend** for macOS, FreeBSD, OpenBSD, NetBSD,
  and DragonFly. Replaces the sync I/O fallback those platforms
  silently fell into via the `if os_is_windows() { ... } else
  { spawn ... }` branch in `http_listen`, which on macOS/BSD sent
  every accepted conn through a sync `read` that pinned its
  worker thread.
- **AArch64 (ARM64) context switch** — `__glide_ctx_switch` now
  carries an `stp`/`ldp` backend that saves x19-x30 + d8-d15. Plus
  a `yield` (vs `pause`) hint in the spin loops and the
  `aarch64-linux-musl` triple registered for cross builds. Lets
  Glide run on Apple Silicon / Graviton / Pi 4-5 / any aarch64
  host. Sysroot tarball still has to ship before
  `glide target add aarch64-linux-musl` resolves.
- **Page size detected at runtime** (`sysconf(_SC_PAGESIZE)` /
  `GetSystemInfo`). The previous hardcoded 4 KiB rounded the
  guard page off the wrong slot on Apple Silicon (16 KiB native
  pages) so a stack overflow could corrupt the next coro's stack
  instead of tripping the guard.
- **SO_REUSEPORT dispatch per-OS**. `http_listen_workers` and
  `Router::listen_workers` now branch on
  `os_has_reuseport_balance()` (Linux: each worker binds; macOS /
  BSD / Windows: bind once + share the fd across N accept
  threads). Previously the spawn path serialised on the silent
  shared-bind semantics in macOS / BSD.
- **IOCP reactor for Windows**. `tcp_read_async` /
  `tcp_write_async` / `tcp_writev2_async` issue OVERLAPPED ops
  against `WSARecv` / `WSASend`; a dedicated reactor thread
  drains `GetQueuedCompletionStatus` and unparks the issuing
  coro. `http_listen` now spawns one coro per connection on
  Windows — `os_has_async_io()` returns 1 there. `accept_tcp_async`
  is still blocking (AcceptEx is a follow-up).

### Codegen + bootstrap

- Top-level `c_raw! { ... }` blocks emit AFTER the runtime
  templates (scheduler + sockets + reactor) instead of before, so
  bootstrap fallbacks in stdlib can `#ifdef`-guard against
  symbols the runtime promised to provide
  (`__GLIDE_RUNTIME_HAS_REACTOR_ACTIVE` etc).
- `cc` invocation on Windows no longer wraps everything in an
  outer `"..."`. The wrap was meant to handle a quoted cc path
  with spaces, but it broke `cmd.exe /c` parsing whenever the
  inner argv already had quoted paths, leaving collect2 with the
  raw `.c` file as a "linker input" instead of a compiled `.o`.
- New TLS flag `__glide_is_main_tls` set on the C main thread.
  `__glide_spawn` from a foreign pthread (e.g. an http_listen
  accept loop running on its own `spawn_thread`) now pushes onto
  the worker queue directly instead of buffering up to a batch of
  32 that may never arrive. The first 31 conns of a server thread
  used to sit unprocessed waiting for conn #32.

### Ownership

- **Non-lexical lifetimes**: a borrow's lifetime ends at its last
  use rather than at scope close, so `&mut self.a` followed by
  `&mut self.b` (with no use of the first borrow after the second
  begins) is now accepted.
- **Arena escape detection**: returning a value that derives from
  a locally-declared `*Arena` is a compile error
  (`arena-escape`). Arenas passed in as fn params are unchanged.
- **Spawn capture lifetime**: `spawn f(p)` where `p` derives from
  a local arena is now a compile error
  (`spawn-arena-escape`). Copy the data out (`let x = *p;`) or
  hand the spawn task a longer-lived arena.
- **Free-then-use detection** on methods: reading a binding after
  `x.free()` errors with `use-after-free` regardless of whether
  `x` was a `*Vector`, `*Arena`, or any other type whose `.free()`
  reclaims its backing memory.

### Runtime

- TLS handshake honours a configurable timeout (`set_tls_timeout`
  default 5 s, override via `Listener::accept_raw` + `attach`).
  Was blocking the accept loop indefinitely whenever a client
  opened a TCP conn and then never wrote the ClientHello.
- `__glide_fs_size` returns `i64` matching the Glide-side extern
  shape; the previous 32-bit return overflowed at 2 GiB.

### Standard library

- **Logging macros**: `info!` / `warn!` / `error!` / `debug!` /
  `trace!` / `fatal!` format like `println!` and route through the
  global logger. A single `import stdlib::log::*;` brings both the
  runtime fns and the macros. A bare value (`info!(x)`) is wrapped in
  `"{}"` automatically.
- **`@logged` reports values + timing**: the entry line interpolates
  the actual argument values, the exit line adds the return value and
  the elapsed time (`> add(4, 6)` / `< add -> 10 (981.2us)`). `@trace`
  stays a lightweight enter/exit flow marker. Level via `@logged(info)`.

### Language + modules

- **`pub import X::*`** re-exports the imported names, so a barrel
  module can collect symbols from several files under one import.
- **First-use generic element inference**: `let v = Vector::new()`
  followed by `v.push(32)` fixes `v` to `Vector<int>`, so a later
  `v.push("x")` is flagged. Previously the unbound element type let
  any push through.

### Diagnostics

- **Aggressive semantic checks**: unknown function / name / method /
  field / type-in-annotation, wrong argument count, and argument-type
  mismatch are now reported for bare fns, `Type::method`, and instance
  methods (including inside proc-macro expansions).
- **Exact spans**: every diagnostic underlines the precise offending
  token (the method name, the field, the type, the argument) instead
  of an approximate one-character mark at the expression start.

### Tooling

- **Plug-and-play cross-compile**: the target sysroot is auto-fetched
  on `glide build --target=...`, a `target` field in `glide.glide`
  sets a default, and the linker only pulls the heavy libs the program
  actually references. `glide target list/add` manage sysroots.
- **LSP**: enforces visibility (unknown symbols are flagged live, as
  in `glide check`), completes macros and members, jumps to a
  proc-macro definition on goto, and shows inferred-type hints.

### Fixes

- **proc-macro registry namespaced by kind**, so `@trace` (the
  function-tracing attribute) and `trace!` (the logging macro) no
  longer overwrite each other. `@trace` had silently become a no-op.
- **Static handler** tolerates an unset `cache_control`: it was
  storing a null and dereferencing it on the next request.
- **Codegen** dedupes the chan SM-handler abort helper across chan
  monomorphizations; a program using two or more `chan<T>` types hit
  a duplicate-symbol C error.
- **Spinner** writes progress to stderr, so it no longer corrupts
  stdout during C emission.

## 0.1.1 — 2026-05-13

### LSP / Editor

- Member completion chain resolver (`r.listen(8080).` ->
  `[ok, val, err]` from `!int`; `var.method(args).` walks
  recursive). Strings, comments, char literals never trigger
  completion. Member context never falls through to the bare-ident
  list (was dumping 500+ globals).
- Closure params + nested-block lets visible to `.` completion.
- Macros (`assert!`, free + proc) appear in bare-ident completion
  with auto-import edits. `@<attr>` and `@derive(Name)` are
  context-aware.
- Hover + goto on proc-macro names (`@derive(JsonBind)`,
  `@handler`) land on the registered impl fn. Hover on builtin
  attrs (`@cfg`, `@derive`, etc.) shows a one-line description.
- documentHighlight / references / rename now walk type
  annotations, attribute args, member fields, struct literals,
  EX_PATH heads + tails, and closures.
- Drop synth-position diagnostics so proc-macro outputs don't
  surface spurious `unused-param` on line 1.
- Goto on `import a::b::c;` returns null when the resolved path
  doesn't exist (was crashing Zed worktrees with bogus URIs).

### Typer

- Method-call return type resolved through the impl registry +
  owner-type-param substitution (`*Vector<int>.get()` -> `int`,
  `r.listen(8080)` -> `!int`).
- Bare fn idents type as real TY_FNPTR. Passing `fn() -> Json<X>`
  to `r.get("/", _)` errors with the full shape at typecheck time
  instead of segfaulting at runtime.
- Method-call arg list compared against the impl params.
- `unused-import` walks attr args, type annotations, and closure
  param types so `@derive(JsonBind)` and `fn(req: *HttpRequest)`
  count as uses.
- Selective imports accept proc-macro registered names
  (`import stdlib::http::handler::handler;`) and `ST_TRAIT` items.
- New `@suggest_for_fnptr(ReturnType, "...")` attribute lets
  proc-macro authors attach context-aware hints to fn-ptr-arg
  mismatches. Companion gates: `@suggest_when_got_returns(...)` /
  `@suggest_when_got_params("0" | "1+" | "any")`. Template
  placeholders: `%got_name%`, `%got%`, `%got_ret%`, `%want%`,
  `%want_ret%`.

### Codegen + expander

- Closure-lift writes back to `e.args` so a fn passed as call arg
  doesn't get re-lifted with a second id, breaking the forward-decl
  pass (`__glide_anon_1` undeclared at C link).
- `strip_compile_time_only` drops `@proc_<kind>` impl fns + their
  bootstrap/* helpers before codegen — user binaries stop emitting
  references to `Stmt` / `Expr` / `Type` carriers the installed
  `~/.glide/bin/src/` doesn't ship.

### Grammar

- Generic `@<name>` / `@<name>(args)` rule covers `@derive`,
  `@handler`, `@proc_*`, etc. Highlights as `@attribute`. Tree-
  sitter parser.c regenerated; zed-extension grammar commit
  bumped.

### Stdlib

- `@handler` and `@derive(JsonBind)` carry `///` doc-comments so
  hover shows usage + example. `@handler` also carries
  `@suggest_for_fnptr(HttpResponse, ...)` so passing a non-handler
  fn to `r.get(...)` surfaces an annotated hint.

### Install

- `glide install` now copies `<project>/bootstrap/` to
  `<install_dir>/bootstrap/` alongside `src/`. Lets proc-macros
  (which `import bootstrap::ast::*`) resolve their AST helpers
  outside the dev repo, and lets the LSP surface goto/hover for
  `Stmt` / `Expr` / `Type` inside proc-fns.

## 0.1.0 — 2026-05-12

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

### Bug-prevention lints

`glide lint <file>` (and the LSP) run a suite of static checks beyond
plain type-checking. Every code is suppressible with `@allow("<code>")`
on the enclosing fn / impl method, and `glide lint --lint-as-error`
promotes them to errors for CI. Custom user lints can be declared via
`@lint("category", "reason")` on any fn.

New codes shipped in this release:

- **`null-deref`** — flow-sensitive: warns on `*p` / `p.field` /
  `p.method()` / `p[i]` where `p` is provably null in scope (let-bound
  to `null`, or callee param that's dereferenced without a guard).
  Interprocedural arm: passing `null` literal to a fn whose param is
  dereferenced unconditionally fires at the arg position.
- **`bad-free`** — `free(s)` where `s: string` is a heap corruption
  (strings are arena-managed). Same for `free(v)` where `v: *Vector<T>`
  or `*HashMap<V>` — those have `.free()` destructors that release
  internal buffers first. Vector/HashMap own destructors are exempted.
- **`string-eq-op`** — `==` / `!=` between two strings compares
  pointers, not bytes. Suggests `s.eq(other)`.
- **`unused-import`** — `import X::{a, b};` where neither name is
  referenced in the file. Wildcard imports are not flagged.
- **`arena-set`** — `__glide_palloc_set(make())` without a paired
  restore in scope leaks the arena bracket.
- **`coro-blocking`** — `spawn fn();` where the target body calls a
  known-blocking helper (sync fs / process / http). Resolves
  spawn-target via fn-name lookup so the warning fires at the
  blocking call site, naming the spawn that reached it.
- **`unhandled-result`** — calling a fn returning `!T` and discarding
  the value. Method calls are resolved by receiver type, so
  `Conn.write -> int` and `TcpStream.write -> !int` (same method
  name, different impl targets) are correctly distinguished.
- **`ignored-option`** — `.val` on a `?T` without a preceding
  `is_some()` / `.has` guard. Recognises the negated-guard idiom
  `if !r.has { break; } ... r.val` and treats subsequent siblings as
  safe.
- **`use-after-free`** — flow-sensitive: accessing `x` after `x.free()`
  in the same scope. `defer x.free()` doesn't trip (fires at scope
  exit, not registration). Reassignment clears the freed bit.
- **`mutex-unbalanced`** — path-aware: every `.lock()` must be
  released on every exit (return / `?` / break / continue /
  fall-off-end). `defer m.unlock();` immediately after the lock
  satisfies the check across all branches. If/else are walked with
  independent held-state copies and merged conservatively.
- **`chan-leak`** — path-aware: a `chan<T>` declared in a fn must be
  `close()`d on every exit path. A close in only one branch of an
  if/else fails the check unless both branches close. Receivers
  parked on `recv()` would otherwise hang forever.
- **`leak-on-early-return`** — `defer x.free()` placed after a `?`
  propagation that can fire before the defer registers. Suggests
  moving the defer to the line immediately after the binding.

Plus the pre-existing pass: `unused-var`, `unused-param`, `unused-fn`,
`unnecessary-mut`, `arena-not-freed`, `addr-of-temporary`,
`dead-code`, `missing-return`, `large-return`, `trait-conformance`,
`deprecated-fn`, `unstable-fn`.

The set is conservative — each lint targets a specific class of bug,
zero false positives observed across `bootstrap/main.glide`,
`src/stdlib/{vector, hashmap, http, argparse, sync, net, json}.glide`
during the QA pass.

### Crash diagnostics

When a Glide program traps (`SIGSEGV`, divide-by-zero, stack overflow,
optimiser-turned-trap), the runtime now prints a usable backtrace.

- **Source-mapped frames**: codegen emits `#line N "<source>.glide"`
  before each statement, so DWARF / addr2line resolve crash addresses
  back to the original `.glide` file and line — not the generated
  `<exe>.__glide.c`.
- **Unified stack trace on Windows**: dbghelp (for system DLL frames)
  and addr2line (for user-code frames) are combined into a single
  numbered list. Previously the user-code frames printed as raw hex,
  with addr2line output in a separate appendix the reader had to
  cross-reference manually. `-lpsapi` is now part of the cc command
  to support `GetModuleInformation` for ASLR-adjusted addresses.
- **Fault address + access type**: on `0xC0000005` we print
  `= faulting <read|write|execute> of address <ptr>` so the reader
  doesn't have to decode `ExceptionInformation` themselves.

### Cross-compilation with TLS

`glide target add x86_64-linux-musl` pulls a ~13 MB sysroot bundling
the OpenSSL 3.3 headers + `libssl.a` / `libcrypto.a` / `libz.a`. With
the sysroot installed, `glide build --target=x86_64-linux-musl`
produces a fully static ELF — `stdlib::http::HttpClient`,
`stdlib::net::tls`, and the HTTPS server included — from any host.

- New `glide target {list,add,remove,dir}` subcommands manage the
  sysroot cache at `~/.glide/targets/<triple>/`.
- Build pipeline picks the bundled Zig as cc for cross targets and
  the host's system cc (gcc / clang) for host builds, so the OS-
  specific link line stays correct.
- Sysroot bundle is built from Alpine APKs (`openssl-libs-static`,
  `zlib-static`) via `tools/build_sysroot.sh`.

### HTTP server lifecycle

The server hot path leaked memory until the next OS-level pressure
event — every handler allocated through the global arena slot, and
nothing reclaimed it. Fixed across three layers:

- **Per-request arena bracket**: `_handle_conn`, `_handle_tls_conn`,
  and the router handler now wrap the body in `make → set → handle →
  restore prev → free`. Transient strings (concat / substring / split
  results from inside the handler) are reclaimed in one mmap-unmap
  per request.
- **Coro-local arena slot**: the active-arena pointer used to be a
  file-static, so cooperative yields between coros let one handler's
  `palloc_set` clobber another's. Moved into the task struct, with
  `__glide_task_arena_get` / `_set` accessors emitted by `sched.c`.
  Idle coros pay zero, busy ones each see their own bracket.
- **`__glide_string_from_buf` is arena-tracked**: the per-byte concat
  was the dominant allocation in the parse path. Replaced with a
  single `__glide_palloc(n+1)` + `memcpy` so the result is reclaimed
  with the rest of the request arena.

### LSP stability fixes

Three use-after-free crashes that surfaced during heavy-edit traffic
on bootstrap-sized projects:

- **Project index allocated in request arena**: `_refresh_project_index`
  ran during `textDocument/completion`, allocating its `*HashMap<Stmt>`
  inside the request arena that was freed before the next request.
  The next completion crashed on the dangling reference. Save / null /
  restore the active arena around the index build so the index lives
  on libc heap.
- **Code-action JsonValue shared subtrees**: building the response
  involved assigning `out.array_val = req.array_val` which made both
  JsonValues co-own the same inner Vector. `json_free(req)` then
  freed memory the response still pointed to. Deep-clone instead.
- **Manifest lint state**: `_lsp_apply_manifest_lint` allocated its
  `lint_deny` / `lint_allow` vectors in the request arena. Subsequent
  reads (during diagnostic emission) read freed memory. Same arena-
  save/null/restore pattern as the project index.

### LSP completion polish

Completion items now carry a `[module]` tag in the detail string so
the popup distinguishes `http_listen` (from `stdlib::http`) from
`http_listen` in user code at a glance. The tag is positioned at the
HEAD of the detail field so long signatures don't clip it — matches
gopls / rust-analyzer's `core::iter` indicator style.

### Toolchain + docs

- `tools/build_release.sh` packages the host + cross-compile glide
  binaries (Windows zip, Linux tar.gz). `tools/build_sysroot.sh`
  builds the sysroot bundle separately from Alpine APKs.
- `README.md` gains a Lints reference table and `glide lint` command
  entry.
- `AGENTS.md` template (emitted by `glide doc --ai` / `glide new
  --ai`) gains a §5 Lints section enumerating every code with the
  rule it enforces plus canonical patterns for arena brackets, mutex
  defers, `?` propagation, and option unwrap. Agents land in a repo
  with the full lint surface documented before generating code.

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

# Changelog

## Unreleased

### Tooling

- **`glide upgrade`** self-updates the toolchain to the latest published
  release — no more manual GitHub downloads on every version bump.
  `glide upgrade --check` reports whether a newer version is available without
  installing it. Linux/macOS for now.

### LSP / diagnostics

- Diagnostics now underline the exact operator. The `?`-propagation error, and
  binary / assignment type-mismatch errors (plus the string-`==` and tuple-`==`
  checks), no longer point at the left operand.
- Hover infers the type of a `let` bound to a method / function call, `?`
  propagation, unary / binary expressions, `as` casts, indexing, tuple literals
  and `format!` — cases that previously showed the name with no type.

## 0.6.1 — 2026-07-01

A networking point release: `TcpStream` can finally see who is on the other end
and tune the socket, instead of only reading and writing bytes.

### Networking

- **`TcpStream` learns its endpoints.** `peer_addr()` / `local_addr()` return a
  `*SocketAddr`, so a server can finally see who dialed in — the peer address
  the `accept` path used to discard. `peer_ip()` is the port-less shortcut.

  ```glide
  let conn: *TcpStream = listener.accept()?;
  println!("connection from {}", conn.peer_addr()?.to_string());
  ```

- **`TcpStream` socket tuning + half-close.** `set_nodelay`, `set_keepalive`
  (new `SO_KEEPALIVE`), `set_recv_buf`, `set_send_buf`, and `shutdown(how)`
  (`SHUT_RD` / `SHUT_WR` / `SHUT_RDWR`) round out the stream so you no longer
  reach for `sockopt::*` free functions on `conn.fd`.

- **`TcpStream::read_exact(buf, n)`** loops until `n` bytes arrive, erroring on
  an early EOF — for fixed-size framing where a short read is a protocol error.

## 0.6.0 — 2026-06-25

The memory-model release. Glide now manages memory automatically without a GC
and without lifetime annotations: a value is owned by default, moves
when you transfer it, and frees itself at the end of its scope. You never write
`malloc` / `free` for the common case, and the compiler catches use-after-move
and double-free at compile time. Borrows (`&T` / `&mut T`), the manual heap
(`new` + `free`), and arenas remain as deliberate escape hatches.

### Ownership and move

- Owned heap values auto-free at scope end (RAII). A constructor that returns a
  struct-literal value (`let t: *T = T{...}; return t;`) hands ownership to the
  caller (move-out), and the caller auto-drops it — no manual `free`, no leak,
  no crash. This works the same with or without a type annotation
  (`let g = T::new()`), in plain functions as well as impl methods.
- Ownership moves on every transfer, not only `return`: assignment (`a = b`),
  struct-field store (`T { f: x }`), let-rebind (`let b = a`), and passing an
  owned value to a `*T` parameter, where the callee takes ownership. A `&T`
  parameter borrows instead. Every value is freed exactly once.
- Reading a value after it was moved is a `use-after-move` compile error,
  enforced uniformly across rebind, assignment, and pass. Reassigning the
  variable revives it.
- `c = d` where `c` already held an owned value frees the old value first, so
  reassignment does not leak; and ownership detection follows passthrough
  returns (`fn make() { let r = T::new(); return r; }`).

### `own` fields and recursive drop

- A struct field marked `own` means the struct owns that heap value and frees it
  on drop: `struct List { head: own *Node }`. A bare `*T` field is a non-owning
  reference, left untouched. The compiler generates a recursive drop that frees
  the owned fields (by their `free` method, else recursively, else a plain free)
  and then the struct, so a linked list or tree frees its whole chain with no
  `free` written.
- `own T` (no `*`) is sugar for `own *T` — an owned heap pointer with the `*`
  left implicit.
- A struct that owns heap fields is heap-managed whether or not the binding is
  annotated; a pure-data struct stays a stack value with copy/move semantics.

### Language

- Struct-literal field shorthand: `Self { x, y }` is sugar for
  `Self { x: x, y: y }`.

### Diagnostics

- A return type written without `->` (`fn f(x) T`) now reports a clear
  `missing-arrow` error instead of derailing into a confusing downstream ICE.

### stdlib

- `HttpClientRequest` and `HttpClient` gained `free()`, so a manual
  `new()` + `do()` no longer leaks the request envelope.

## 0.5.0 — 2026-06-25

The first tagged release since 0.4.1, and it carries a lot. Builds are static
and self-contained by default, the package manager grew a working local-dep
flow and moved deps under `build/`, the editor gained proc-macro and `c_raw`
intelligence, the typer started checking `impl` method bodies, and a class of
latent undefined behavior that only the zig toolchain trips was cleared out so
the full test suite is green on Windows too. The correctness pass first staged
as 0.4.2 (below) ships as part of this release.

### Build and packaging

- `glide build` now links statically and self-contained by default, with
  OpenSSL / zlib / ngtcp2 baked in from the sysroot. `--dynamic` opts back out.
  macOS stays a hybrid (static deps, dynamic libSystem) since Apple ships no
  static libc.
- DEFLATE inflate and deflate are implemented in pure Glide, so
  `stdlib::compress` no longer links `-lz` and compress programs build fully
  static.
- The cross-compile sysroot is fetched from the last published release rather
  than the in-development version, so source builds between releases stop
  404ing.

### Package manager

- Downloaded git deps live in a content-addressed cache (`~/.glide/cache/`),
  with per-project links under `build/deps/` (gitignored, removed by
  `glide clean`) in place of a top-level `glide_modules/`.
- A local dep added with an absolute Windows path (`C:\...` / `C:/...`) now
  resolves; the absolute-path check previously recognized only a leading `/`
  and mistook the drive path for a relative one.
- A dependency `rev` or `url` carrying shell-unsafe characters is rejected
  before it can reach a `git` command.
- Missing deps are auto-fetched when a project is opened in the editor.

### Editor and LSP

- Imported files are parsed header-only (signatures kept, function bodies
  brace-skipped), cutting project-index memory use sharply.
- `@derive` and other proc-macro methods get completion, hover, and
  go-to-definition; a synthesized `impl` is no longer flagged as missing its
  trait methods.
- `c_raw!` bodies get C-aware completion (libc, runtime, and Glide locals by
  their C name) plus embedded-C syntax highlighting.
- Go-to-definition on a parameter lands on the parameter itself, and a
  qualified `pkg::T { ... }` literal gets its fields checked.
- Two per-keystroke leaks are gone (the parsed request JSON and the per-path
  overlay arena).

### Formatter

- Comments are preserved across operator chains, `match` arms, enum variants,
  and end-of-block positions, and the output uses minimal parentheses instead
  of wrapping every operator.
- Float literals are no longer rewritten to `0`.

### Typer

- `impl` method bodies are type-checked for the first time. The findings
  surface as warnings for now, scoped to focused checking (LSP / `glide check`),
  so the existing backlog can be cleared before they become hard errors without
  breaking the self-host.
- Inner-field access through an unwrapped `!T` / `?T` reports a clear
  `unwrap-needed` error instead of failing later in the C compile.

### stdlib

- `fs`, the `crypto` digests (SHA-256, SHA-1, HMAC), and `process` output
  capture are binary-safe: they carry real lengths and no longer truncate at an
  embedded NUL.
- JWT signature verification uses a constant-time comparison.

### Platform (Windows and zig)

- A run of latent undefined behavior that gcc quietly tolerates but the zig
  safety build traps was removed, so `glide test` passes on Windows+zig. `fs`
  uses the Win32 file API instead of `_stat` (which mingw and the UCRT both
  define as `_stat64i32`, a duplicate-symbol link error); the HashMap hash, the
  xorshift PRNG, and IPv4 octet packing accumulate unsigned so the overflow
  wraps cleanly; and an uninitialized `bool` read was fixed.

### Internals

- The compiler is reorganized into layered modules. `_cg_replace` is linear
  instead of O(n^2), test and bench builds run in isolated child processes, and
  cc errors are anchored back to Glide source with `c_raw` validated before the
  C compile.

## 0.4.2 — 2026-06-21

A correctness release. A wide bug hunt turned up a class of defects where
`glide check` accepted a valid program but the C compile failed or the result
ran wrong; this release closes ~22 of them at the root, with the unit suite
green after each fix and the self-host fixpoint intact. Option/Result also gain
proper editor completion.

### Identifiers vs C symbols

- A user function whose name matched a libc symbol no longer breaks the build:
  `fn div` collided with `<stdlib.h>`'s `div` (a cc error) and `fn abs` was
  silently shadowed by the gcc builtin (wrong result, no diagnostic). Such
  body-bearing functions are now renamed into the reserved `__glide_uf_`
  namespace; extern/FFI bindings keep their real name.
- Top-level consts named after a libc macro (`NULL`, `EOF`, `EXIT_FAILURE`, …)
  no longer emit a clashing `#define`.

### `match`

- `match` on an `?T` / `!T` value compiles (it emitted a placeholder C type
  before). Arm bodies are now type-checked, a typo'd or unknown variant is
  reported, and expression-position `match` gets the same exhaustiveness check
  the statement form already had. A payload-less variant (`Color::Red`) is
  typed as its enum, so those checks fire without an annotation. A `match`
  scrutinee counts as a use (no false `unused-var`).

### Closures

- A closure stored in a local and then called compiles, and passing a closure
  to a generic method (`Vector.map`) no longer crashes the compiler. Closure
  bodies are type-checked, and capturing an enclosing local is reported as a
  clean `unknown name` instead of failing later in C.

### Option / Result, tuples, strings, ints, generics

- `some(3).unwrap_or(0)` and friends work on a bare ctor literal.
- A whole tuple renders as `(a, b)` in `println!`; tuple `==` is rejected at
  type-check instead of emitting invalid C.
- A literal `%` in `print!`/`format!` is escaped; an embedded NUL in a string
  literal keeps the real `.len()`; inline narrow-int (i8/u8/…) arithmetic in
  print macros is truncated to its declared width.
- Generic structs with a by-value nested generic field, `Vector::new()` in a
  struct-literal field inside a generic impl method, and nested `?(?T)` all
  emit valid C now.

### Tooling

- `fn main() -> i32` propagates its exit code (only the retired `int` alias did
  before), so a failed compile is again visible to CI and `$?`.
- Editor completion on a `?T` / `!T` value now offers the intrinsic methods
  (`unwrap`, `unwrap_or`, `is_ok`, `map`, `and_then`, `ok_or`, `filter`, …) with
  signatures, worked examples and call snippets, alongside the `.ok`/`.err`/
  `.val` fields.

## 0.4.1 — 2026-06-19

Strings carry a length header (O(1) `.len()`), string building is O(n), several
language ergonomics land, the C seed is gone, and the editor tooling caught up.

### Strings (SDS)

- `string` now stores a 4-byte length header immediately before the data; the
  value still points at the NUL-terminated bytes, so C FFI is unchanged. `.len()`
  is O(1) and `concat`/`eq` no longer call `strlen`. Multi-byte UTF-8 length is
  the byte length, same as before.

### Performance

- `ByteBuffer` gained `to_string()` / `push_int`, and the pervasive
  `out = out.concat(x)` folds in the formatter, JSON serialiser and string/env
  helpers were rewritten as single-allocation builders — O(n) instead of O(n²).

### Language

- Compound assignment operators: `%= &= |= ^= <<= >>=` (joining `+= -= *= /=`).
- `if let some/ok/err/none(x) = e { ... }` binding form.
- `Self` is usable inside `impl` bodies (struct literals, `Self::`, casts).
- Cross-file reads of a private struct field are now a hard error.
- Generic structs used only as a field/element type of another generic struct
  now monomorphize (e.g. `Holder<T>` with a `*Cell<T>` field, including
  `Cell<T>` storing `value: T` by value).

### Diagnostics

- "Did you mean" suggestions on unknown types/names/functions/fields, offered as
  one-click fixes, plus a help catalogue keyed by diagnostic code.

### Stdlib

- Vector `remove`/`insert`/`sort_by`/`enumerate` plus specialized
  `contains`/`index_of`/`sort`; HashMap `get_or`/`insert_if_absent`; String
  `reverse`/`pad`/`strip`/`lines`/`count`.

### Bootstrap

- Removed the checked-in `bootstrap/seed/bootstrap.c`. Glide is self-hosting, so
  CI/release/`DEVELOPING.md` now bootstrap by downloading a published release
  binary and building the current sources with it.

### Tooling / LSP

- The contextual `new` keyword (the constructor method name in `Vector::new()` /
  `fn new` / `Self::new`) is now colored as a function, not a control keyword, in
  semantic tokens and the tree-sitter grammar.
- The VS Code extension resolves the `glide` binary from `~/.glide/bin` when it
  isn't on PATH (GUI apps don't inherit the shell PATH on Windows), and reports
  an error instead of failing silently. Both editor extensions bumped to 0.4.1.

## 0.4.0 — 2026-06-17

Networking + string/byte ergonomics. **Breaking:** the server-side `Listener` /
`Conn` types are removed in favour of `TcpListener` + `TcpStream` (see below). The server-side TCP API is folded onto the
same `gnet_*` foundation the client already used, so one `TcpStream` type now
serves both directions, and converting between bytes and `string` (with UTF-8)
is first-class.

### Networking (breaking)

- **`Listener` / `Conn` removed; use `TcpListener` + `TcpStream`.** The old
  server types exposed a raw `fd`, an `ok()` method you had to remember to call,
  and `read(buf: *void, max)` that forced a `malloc(...) as *u8` / `buf as *void`
  dance. They are gone. `TcpListener::bind(port)` and `accept()` now return
  `!T`, and `accept()` yields a `*TcpStream` — the *same* type `TcpStream::connect`
  returns. A connection reads and writes identically whether you dialed out or
  accepted it in.

  ```glide
  fn main() -> !i32 {
      let listener: *TcpListener = TcpListener::bind(8080)?;
      defer listener.close();
      while true {
          let conn: *TcpStream = listener.accept()?;
          spawn handle(conn);
      }
      return ok(0);
  }

  fn handle(conn: *TcpStream) -> !i32 {
      defer conn.close();
      let data: *ByteBuffer = conn.read(1024)?;   // no malloc, no casts
      defer data.free();
      let text: string = String::from_utf8(data)?;
      return ok(0);
  }
  ```

- **`TcpStream::read` returns `!*ByteBuffer`** (binary-safe). `read_str` keeps the
  text convenience; new `write_buf`, `write2` (gather-write), `sendfile`
  (zero-copy), and `write_ptr` round out the server fast path. The reuseport,
  gather-write, and sendfile primitives the HTTP server relies on are now exposed
  through `gnet_*` (new C externs `gnet_writev2_async` / `gnet_sendfile_from_path`),
  so the unified foundation keeps the old throughput.

- **UDP** gains binary-safe `send_buf` / `recv_from_buf` (returning a
  `*UdpDatagram` whose payload is a `*ByteBuffer`) alongside the existing string
  forms.

### Language

- **`?` now unwraps under `let` type inference.** `let x = f()?` (no annotation)
  declares `x` as the unwrapped `T`, not the `!T`/`?T` result struct. Previously
  codegen's inference left `?` at the result type — so `x + 1` or `x.method()`
  failed to compile and an explicit `let x: T = f()?` was required. The typer
  already unwrapped; codegen's `infer_for_codegen` now matches it.

- **Missing return is now an error, not a warning.** A function declared with a
  return type that can reach the end of its body without returning a value
  (`fn main() -> i32 { }`) now fails compilation (`missing-return`) instead of
  warning. The flow analysis got more precise so it doesn't false-fire on
  diverging code: a `while true { … }` with no loop-level `break`, and an
  exhaustive `match` *without* a `_` arm (a non-exhaustive one is already its own
  error), both count as "always returns". `-> !` functions still return `ok`
  implicitly and are exempt.

### Strings, bytes, UTF-8

- **`String::from_utf8(buf) -> !string`** (validated), **`from_utf8_lossy`**
  (U+FFFD on bad bytes), and **`is_valid_utf8`** in the new `stdlib::utf8`.
- **`ByteBuffer`** gains `from_string`, `with_capacity`, `push_bytes`,
  `push_buf`, and `push_codepoint` (UTF-8 encode a codepoint).
- **`string`** gains `as_bytes`, `char_count` (codepoints, not bytes), and
  `chars()` returning a `*Vector<i32>` of codepoints so `for cp in s.chars()`
  works.

### Editor / LSP & formatter

- **Member completion resolves through `?`.** `let x = Foo::make()?;` then `x.`
  now offers `Foo`'s methods. The LSP's AST type resolver didn't handle the `?`
  (or `*`) unary, so a `?`-bound local resolved to nothing and completion went
  silent — the most-reported gap in the new ergonomic style.
- **`fmt` keeps `-> !` bare.** Formatting no longer rewrites a value-less result
  return type `-> !` into `-> !void`; an explicit `-> !void` is normalized back
  to `-> !`.

### Fixes

- **`println!` / `print!` from a spawned coroutine now appears.** stdout (and
  stderr) are unbuffered at startup, matching Go's `os.Stdout` — previously a
  server looping forever buffered all output and a `spawn`ed handler's prints
  never reached the terminal. On Windows the blocking-`accept` loop also flushes
  pending main-thread spawns, so the per-connection coroutine actually runs
  instead of sitting in the batch buffer until 32 connections arrived.
- **`?T` / `!T` over a struct with a `*dyn Trait` field no longer miscompiles.**
  A `?HttpResponse`-style wrapper around a struct whose only `*dyn` use was a
  by-value field failed with `unknown type __glide_dyn_<Trait>`: the dyn
  collection pass skipped struct fields, and the fat-pointer typedef was emitted
  after the wrapped struct body. The typedef is now emitted up front and struct
  fields + enum payloads are scanned for dyn traits.
- **Clearer error for `let v: Vector<T> = Vector::new()`.** The by-value
  annotation mismatches the `*Vector<T>` a heap constructor returns; the
  diagnostic now points at `*Vector<T>` or dropping the annotation instead of
  showing a raw, unresolved-`T` mismatch.

## 0.3.4 — 2026-06-02

A macro release: macros can now produce values, and there's a `vec_of!` builtin
for one-liner vectors. No source-breaking changes from 0.3.3.

### Macros

- **Macros that return a value.** When a `macro` body ends with `return <expr>;`,
  a call in expression position expands to a block-expression that yields that
  value — the same `{ ...; return v }` rule as a literal block. The same macro
  still splices statements at statement position. Works in every call shape
  (bare `name!`, receiver `recv.name!`, and `Type::name!`) and in any expression
  slot — let-init, argument, operand, `return`.

  ```glide
  macro ints!($($x:expr),*) {
      let v: *Vector<i32> = Vector::new();
      $( v.push($x); )*
      return v;
  }
  let v = ints!(1, 2, 3);            // a real *Vector<i32>
  let n = doubled!(5) + doubled!(10);
  ```

- **Hygiene.** Locals introduced by a macro body are now renamed per expansion,
  so `let tmp = $x` can no longer capture a caller variable named `tmp`. Macro
  expansion is also re-walked, so a macro call nested in another macro's
  arguments (`assert_eq!(_take(doubled!(50)), 101)`) expands correctly. A macro
  defined in terms of itself stops at a recursion limit with a diagnostic
  instead of hanging the compiler.

- **`vec_of!` builtin.** `vec_of!(a, b, c)` builds a `*Vector<T>` (T inferred
  from the first element) and is available everywhere without an import. Empty
  or mixed-type calls are rejected with a clear error.

  ```glide
  let v: *Vector<i32> = vec_of!(10, 20, 30);
  let names = vec_of!("alice", "bob");   // *Vector<string>
  ```

### Editor / LSP

- Completion and hover now include the `vec_of!` builtin alongside the other
  builtin macros (`println!`, `format!`, `dbg!`, …).

### Codegen

- Fixed a bug where an unannotated `let v = { ...; return local; }` block-
  expression inferred `v` as `int` (and then failed on `v.method()`); the
  block's local declarations are now considered when inferring its value type.

## 0.3.3 — 2026-06-01

A correctness + tooling release. Module-qualified type names work everywhere,
the linter gains a full set of dead-code checks, the editor experience is
sharper, and the runtime now builds and runs cleanly on Linux and macOS as well
as Windows. No source-breaking changes from 0.3.2.

### Language & types

- **Module-qualified type names** (`http::HttpResponse`, `foo::Bar`,
  `b::Thing`) now resolve to their bare global type in every position — return
  types, value / pointer / borrow params, struct fields, `Vector<…>` elements,
  `?T` / `!T` wrappers, casts, and `match` arms (`match c { foo::Color::Red => … }`).
  Previously a qualified annotation was treated as a distinct, unresolvable type
  ("return type mismatch: expected http::HttpResponse, got HttpResponse"). A
  non-existent qualified type (`mod::Nope`) is now rejected instead of silently
  accepted.

### Linter

- New dead-code analyses, all covered by `@allow("unused")`: `unused-struct`,
  `unused-enum`, `unused-const`, `unused-variant`, and `redundant-import` (the
  same module imported twice in one file). They join the existing
  `unused-var` / `unused-param` / `unused-fn` / `unused-import` family and skip
  `pub`, generic, and impl'd declarations to stay false-positive-free.

### Editor / LSP

- Completion now offers the builtin macros (`pkg!`, `cfg!`, `env!`, `panic!`,
  `dbg!`, `assert!`, `todo!`, `unreachable!`, `file!`, `line!`, …) and the
  `@suggest` / `@leaf` / `@used` / `@section` attributes, each with hover docs.
- Hover and go-to-definition work on user and stdlib macros; hover on a method
  resolves the right `impl` when several types share a method name.
- Auto-import merges a new symbol into an existing selective import
  (`import al::{Alelo}` becomes `{Alelo, custom}`) instead of doing nothing;
  non-pub siblings stay out of completion; `foo::` no longer leaks a same-named
  submodule's members.
- `@suggest(param, "a", "b")` and type-aware argument completion fire on
  `recv.method(` calls too.

### Cross-platform & runtime

- **Linux and macOS** now build and run the full test suite, joining Windows.
  The coroutine stack works under musl and on arm64 macOS (the growable-stack
  handler can't resume there, so coros run on a fixed stack); socket read/write
  block correctly when run off a coroutine worker (raw threads); `glide test` on
  a directory no longer crashes; and `stdlib::process` no longer references the
  glibc-only `execvpe`.
- **HTTP server responds on Windows**: `http_listen` accepted connections but
  never replied; it now serves requests.
- A lib-linking `glide build` no longer prints spurious `tar` / `gzip` errors
  (the sysroot is now extracted in-process), and HTTP/3 links statically.

### Tooling

- One-command regression suite: `bash tools/test_all.sh` runs the unit tests,
  the LSP smoke, and end-to-end build/run smokes. CI runs it on Linux, Windows,
  and macOS for every push and PR.

## 0.3.2 — 2026-05-29

The plug-and-play release. A fresh `glide` install now cross-compiles for
any supported target with zero external dependencies — no system gcc, no
`apt install libssl-dev`, no MSYS2, no manual sysroot setup. Plus a pass
over package import/export ergonomics. No source-breaking changes from 0.3.1.

### Cross-compile & packaging

- **Plug-and-play host builds**: a bare `glide build` (no `--target=`) for
  a program that links openssl / zlib / ngtcp2 / nghttp3 now auto-promotes
  to a cross-build against the host's own triple, producing a fully static,
  self-contained binary. On Windows that's a single 6 MB `.exe` instead of
  the binary plus ~16 MB of companion DLLs.
- **Four official targets**: `x86_64-linux-musl`, `aarch64-linux-musl`,
  `x86_64-windows-gnu`, `aarch64-macos-none` — all listed in
  `glide target list` and auto-fetchable.
- **Bundle binaries**: the release now also ships `glide-bundle-<os>-<ver>`
  archives with the Zig toolchain, every sysroot, and the Glide stdlib
  baked into the binary. First run self-extracts everything into `~/.glide`
  and builds for any target completely offline.
- Each release also publishes per-triple sysroot tarballs for
  `glide target add <triple>`.

### Package DX

- `glide add <user/repo>[@<rev>]` shorthand — expands to a full
  `Dep::git(...)` line. Accepts bare `user/repo`, a full host
  (`gitlab.com/g/sub/r`), and an optional `@rev` (defaults to `main`).
- `glide tree` — prints the dep graph with MVS-resolved revs and a
  `[not fetched]` tag on missing cache slots.
- `glide info <name>` — manifest + cache metadata for a declared dep.
- `glide search <query>` — GitHub code-search for repos with a
  `glide.glide`, printing copy-pasteable `glide add` lines.

### Language server

- Code actions on `unresolved-import`: when the alias isn't declared,
  offers to insert a `Dep::git` / `Dep::path` line into `glide.glide`
  (cross-file WorkspaceEdit); when it's declared but uncached, offers a
  "Fetch missing dep" command wired through `workspace/executeCommand`.

### Documentation

- `glide doc` groups the index by package — Project, each declared
  dependency (with version + description + repository link), and the
  standard library — instead of one flat module list.

### Fixed

- **TLS verify on Windows**: `SSL_CTX_set_default_verify_paths` is a no-op
  on MSYS2 / mingw (its compiled-in `OPENSSLDIR` ships empty), so every
  HTTPS verify returned `tls: connect failed`. Now bridges the Windows
  ROOT + CA system stores into OpenSSL's `X509_STORE` via Crypt32.
- Windows host builds copy the runtime DLLs they link against next to the
  produced `.exe`, so a non-bundle build runs from any shell.

## 0.3.1 — 2026-05-28

A focused LSP fix release. External-package autocomplete in user projects
(`glide_modules/<dep>/...`) was effectively unreachable since the package
manager landed; this release makes it work.

### Fixed

- **External-dep completion**: `_refresh_project_index` was probing the
  dep cache directory with `__glide_file_exists`, which is implemented as
  `fopen("rb")` and always fails on a directory. Every declared dep
  silently skipped indexing, so completion offered nothing from
  `glide_modules/<dep>/src/`. Now uses `__glide_fs_is_dir` for directory
  probes.

### Language server

- `project_index` is now a real second index, separate from
  `stdlib_index` — project source and external deps no longer leak into
  the stdlib bag across project switches.
- Re-index trigger is content-driven: a fingerprint of the manifest plus
  the `glide_modules/` listing is recorded per build. Edit `glide.glide`
  or complete a `glide fetch` and the index rebuilds on the next request
  instead of waiting for an editor restart.
- Entry-file collapse: at a dep's `src/` root, the file matching the
  package name surfaces under the bare alias — `import glicord;` for the
  lib entry instead of `import glicord::glicord;`.
- Per-step trace lines under `~/.glide/lsp.log` (`project index:
  rebuilding for ...`, `dep X - indexing ...` / `cache miss at ...`).

## 0.3.0 — 2026-05-27

The intelligent-LSP, build-introspection, and dev-ergonomics release. The
language server goes from basic to IDE-class, editing large files
gets ~15× faster, and a family of compile-time macros (`pkg!`, `cfg!`, `env!`,
`panic!`, `assert!`, `dbg!`, …) lands. No source-breaking changes from 0.2.0.

### Language server

A full intelligence pass over `glide lsp` — all of it pure analysis, no AI:

- **Signature help** — parameter hints with the active parameter highlighted,
  for free functions, `recv.method` (self hidden), and `Type::static` calls.
- **Semantic tokens** — full-document semantic highlighting (keyword /
  function / method / type / parameter / variable / property / macro /
  namespace) classified from the lexer + the type/parameter tables.
- **Inlay hints** — inferred `let` **types** (`: T`), **parameter names** at
  call sites, and **ownership lifecycle** (`freed @ Ln` / `moves out` /
  `never freed`) on owned allocations.
- **Workspace symbols** — fuzzy project-wide symbol search with navigable
  locations.
- **Call hierarchy** — prepare + incoming/outgoing calls.
- **Type hierarchy** — supertypes / subtypes: a trait's supertraits and its
  implementors + sub-traits, a struct's implemented traits.
- **Go to implementation** and **go to type definition**.
- **Chain-aware completion** — `r.val.method()` and similar member/method
  chains resolve, plus struct-literal field completion and package/module
  completion.
- **Null-flow for `?T` / `!T`** — flags reading `.val` / `.ok` without a guard,
  and understands `if x.has { … }`, `if !x.has { return }` early exits,
  `&&`-chained guards, `||`-chained early exits, and narrowing on
  `x = some(v)` / `let x = ok(v)`.
- **"Why" related information** — diagnostics link to their cause: a trait
  method's declaration, the `T: Trait` bound on a generic fn (`unsatisfied-bound`),
  and the first borrow (`overlap-borrow`, "`x` was first borrowed here").
- **Error-handling DX** — quick fixes (wrap a bare value in `some(…)` / `ok(…)`),
  a must-use check on ignored results, and constructor/virtual-field docs.
- **`pkg!` editor support** — completion of the manifest field names inside
  `pkg!("…")` (each with its resolved value), and a diagnostic on an unknown
  field.
- **Whole-token diagnostics** — warnings underline the offending token, not a
  single character (matching errors).
- Per-document arenas, so hover / goto / completion survive across multiple
  open files.

### Compile-time macros

- **`pkg!("field")`** — inlines a field from the project's `glide.glide`
  (`name` / `version` / `author` / `license` / `description` / `repository`)
  as a string literal. Print your CLI's own version with zero runtime cost:
  `println!(pkg!("name"), " v", pkg!("version"))`.
- **`cfg!("…")`** — a compile-time boolean for `windows` / `posix` / `linux` /
  `macos` / `x86_64` / `aarch64`, decided by the target's C preprocessor — so
  `if cfg!("windows") { … }` is correct under cross-compilation. The
  expression companion to the `@cfg` attribute.
- **`env!("VAR")`** — the build-time value of an environment variable as a
  string literal (empty when unset). Embed a git SHA or build date with
  `GIT_SHA=$(git rev-parse HEAD) glide build`.
- **`panic!("msg")` / `todo!()` / `unimplemented!()` / `unreachable!()`** —
  print the message + `file:line` and abort (the trap handler then dumps a
  stack trace).
- **`assert!(cond[, msg])`** — panics with the message and location when the
  condition is false.
- **`dbg!(x)`** — prints `[dbg file:line] = <value>` to stderr (type-aware) and
  returns the value, so it drops into an expression: `let y = dbg!(compute())`.

### Compiler & language

- **Freestanding / kernel build mode** — `kind: "kernel"` (or
  `freestanding: true` + `linker:`) in `glide.glide` drops the hosted prelude,
  runtime, and `main` wrapper and links `-ffreestanding -nostdlib`; top-level
  `let` is emitted as a C global (with `@section` / `@used` honored).
- **Hex and binary integer literals** — `0xFF`, `0b1010` (with `_` digit
  separators).
- **Statement-level `@cfg`** — gate a single statement or `{ … }` block, not
  just a declaration.
- **Module-qualified generic type annotations** — `let v: mod::Type<T> = …`.
- **Top-level `let` globals** resolve inside function bodies.

### Performance

- **String `substring` is O(end−start), not O(strlen)** — it no longer scans
  the whole source on every call. The lexer calls it once per token, so this
  was making tokenization O(n²): editing an 11.8k-line file in the LSP dropped
  from ~2.6 s to ~0.18 s per change (~15×), and the type-checker's own passes
  sped up several-fold.

### Fixes

- Option/Result constructors (`some` / `ok` / …) now monomorphize correctly in
  struct-literal field position and field assignment.
- Module-qualified `Type::method(...)` resolves, with a quick-fix-capable
  diagnostic when it doesn't.
- Use-after-free detection is flow-sensitive: a value freed inside a branch
  that returns/breaks isn't flagged on the fall-through path, and a `let` that
  rebinds the name clears the freed state — killing false positives.
- `unnecessary-mut` no longer fires when the binding's address (`&x` / `&mut x`)
  is taken and passed to a function that can write through it.
- Extern / forward-declared functions are treated as link-time symbols (always
  visible), and the `__program__` / `__typeof__` compiler intrinsics are
  recognized — clearing false "not in scope" diagnostics when a compiler file
  is opened on its own.
- Apple-Silicon stack-growth registers and portable feature macros in the
  runtime.

### Internal

- The module loader + visibility builder were extracted into
  `bootstrap/loader.glide`, so sibling modules import the API honestly instead
  of relying on the flat-merge build.
- `net.glide` folded into the `net/` package.
- A Claude Code plugin exposes the Glide LSP to the editor's built-in tooling.
- Native multi-platform release builds in CI (Linux / macOS / Windows).

## 0.2.0 — 2026-05-25

### Types

- **Canonical fixed-width numeric types**: `i8 i16 i32 i64 i128 i256` /
  `u8 u16 u32 u64 u128 u256` / `usize isize` / `f32 f64`. The legacy C-style
  spellings `int / uint / long / ulong / float` are **retired as type names** —
  using one is an error with a hint to the canonical replacement. `i128`/`u128`
  are native (`__int128`); `i256`/`u256` are a first-class software 4×u64 struct
  with operators dispatched to runtime helpers, built via `u256::from(n)`.
  Methods (`.to_string()`, `.abs()`, …) work across every width.
- **Associated int constants** `i32::MAX` / `u64::MIN` / … for every integer
  width (i128/i256 included). Unsigned `.to_string()` no longer prints via the
  signed path (`u64::MAX` was showing `-1`).
- **Anonymous tuples**: `(A, B, ...)` as a first-class structural type — tuple
  literals `(a, b)`, positional access `t.0` / `t.1` (nested `t.0.1` too),
  multi-value return (`fn divmod(...) -> (i32, i32)`), tuple params / struct
  fields, `let (a, b) = expr;` destructuring, and tuples wrapped in `!T` / `?T`.
  Each unique element combination lowers to one C struct, so `(i32, string)` is
  the same type everywhere. Complements the `struct Name(field: T)` tuple-struct
  sugar (which stays the way to name a value type).

### Compiler

- **Flexible `main` signatures**: `fn main()`, `fn main() -> i32`, and
  `fn main() -> !T` are all accepted; the C wrapper maps each to an exit code.
- **Compile-time `format!` check**: a `{}` placeholder / argument count
  mismatch is reported at compile time instead of reading a missing printf arg.
- **First-use generic element inference**: `let v = Vector::new()` followed by
  `v.push(32)` fixes `v` to `Vector<int>`, so a later `v.push("x")` is flagged.
- **`pub import X::*`** re-exports the imported names, so a barrel module can
  collect symbols from several files under one import.

### HTTP

- **HTTP/2 server** with stream multiplexing (HPACK + frames + ALPN).
- **HTTP/3**: client over ngtcp2 + nghttp3 (interop-tested against Cloudflare),
  a server (multi-connection, streaming response bodies, self-signed cert
  helper), and **0-RTT** end-to-end (session cache + TLS resumption, early-data
  plumbing, anti-replay sliding window, disk persistence). H3 needs an
  OpenSSL 3.5+ toolchain.
- **Router + decorators**: `@route` / `@get` / `@post` / etc., `@middleware(...)`
  composed per route, `@listen(port)` / `@listen_workers(port, n)` rewrite
  `main` with the router setup, and `routes!(r)`. Method-aware router with
  `:param` / `*wildcard` segments.
- **OpenAPI 3.0.3** spec emitted from `@route`'d handlers; route discovery
  surfaced to the LSP (`documentSymbol` + `glide/routeList`, `/glide-routes`).
- **Extractors**: `Form`, `Query<T>`, `Cookies`, plus the existing typed
  `Json<T>` / `Path<T>` / headers / bearer extractors.
- **Server-Sent Events** (production): keep-alive, comments, retry,
  Last-Event-ID, `SseChannel`.
- **Static file middleware** (production): ETag, directory-traversal guard,
  `index.html`, expanded MIME map, zero-copy via `sendfile` / `TransmitFile`.
- **TLS**: server-side ALPN advertise + select callback; configurable handshake
  timeout + split-accept API.

### Concurrency + runtime

- **io_uring reactor** (Linux), opt-in via `GLIDE_REACTOR=uring` (+14% over
  epoll). Joins the kqueue (macOS/BSD) and IOCP (Windows) backends below.
- **`@leaf` + stackless spawn**: a `@leaf` coroutine runs inline on its worker
  (~192 B/task). `http_listen_workers` / the router use a state-machine fast
  path by default (≈95% of Axum); `@leaf` is auto-detected, no longer required
  on handlers, and a blocking handler aborts cleanly.
- **Growable coroutine stacks** (default 8 KiB, fault-driven grow) and a tuned
  channel park path: `park_unpark` went 23 µs → ~270 ns (per-worker run-next
  slot + spin budget, `GLIDE_CHAN_SPIN`).

### Macros

- **`@proc_pass`** — whole-program compiler passes distributable as libraries;
  **`pass_diag`** lets a pass emit custom lints (location + code + severity)
  that get the same rendering and `@allow` suppression as built-ins.
- **`@proc_macro_expr`** — proc-macros that return an expression (compile-time
  values).
- **Import-aware macro resolution**: bare `name!` resolves across dependencies
  with an ambiguity error; `dep::name!` disambiguates same-named macros by
  origin module.
- **`stdlib::meta`** — a stable surface for compile-time macro libraries.

### Package manager

- Import a dependency **by its bare name**; `glide new --lib` scaffolds a
  library as `src/<name>.glide`.
- Git deps accept **`file://` and `git://`** remotes.
- The manifest parses **`author` / `license` / `description` / `repository`**
  (informational) and they're scaffolded into a new project.
- `glide check` with no file resolves the project's entry point.

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

- **`stdlib::regex`** — a pure-Glide PCRE-like engine (classes, quantifiers,
  groups, anchors, alternation).
- **`stdlib::log`** — a structured leveled logger (rotating files, syslog,
  JSON / key-value output) plus `println!`-style level macros `info!` /
  `warn!` / `error!` / `debug!` / `trace!` / `fatal!`. One
  `import stdlib::log::*;` brings the runtime fns and the macros; a bare value
  (`info!(x)`) is wrapped in `"{}"` automatically.
- **`@logged` reports values + timing**: the entry line interpolates the actual
  argument values, the exit line adds the return value and elapsed time
  (`> add(4, 6)` / `< add -> 10 (981.2us)`). `@trace` stays a lightweight
  enter/exit flow marker. Level via `@logged(info)`.

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
- **Manifest (`glide.glide`) awareness**: the manifest is parsed for
  its fields but never compiled, so the LSP and `glide check` now skip
  semantic analysis on it (no more false `unknown type Package`), while
  still surfacing parse errors. Completion is manifest-aware too —
  `Package` + its fields at the right spots, `vec_of`, and
  `Dep::path` / `Dep::git` inside the dependency list. Field completion
  is progressive: fields already written drop off the list, and a value
  slot (`deps: ...`) offers `vec_of` rather than field names. A
  `target:` value slot completes the cross-compile triples
  (`x86_64-linux-musl`, `aarch64-macos`, `x86_64-windows-gnu`, …). Hover
  over any manifest symbol (a field, `Package`, `vec_of`, `Dep::path/git`)
  shows what it is and whether it's required.

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
HEAD of the detail field so long signatures don't clip it.

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
Glide is a self-hosted, plug-and-play systems language with no system C
compiler required.

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

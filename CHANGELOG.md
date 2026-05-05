# Changelog

## 0.1.0 — 2026-05-05

First release. Glide is a self-hosted, plug-and-play systems language with
no Rust or system C compiler required.

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
- LSP doesn't follow imports during analysis (single-file scope).
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

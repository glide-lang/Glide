# Glide — agent context

**Read this file every time you touch Glide code in this repo.**

## 1. Required reading

Before generating, editing, or reviewing any `.glide` file:

1. Open `build/doc/llms.txt`. Every `pub` item in the project
   and its dependencies lives there in plain text. If the file is
   missing, regenerate with `glide doc --ai`.
2. Scan the module(s) you'll be modifying for existing
   conventions, then match them.

This is not optional. The Glide stdlib has idioms that differ
from Rust, Go, and C — guessing leads to non-compiling output.

## 2. Language quick-reference

- **Errors as values**: `!T` (Result), `?T` (Option), `?!T`
  (Option-Result). No exceptions. Propagate with `?` postfix.
- **Memory**: arena-allocated by default. `let v* = T { … };`
  is auto-dropped at block exit. Raw allocation via `malloc`
  + `defer free(p)` is supported but rare.
- **Concurrency**: M:N coroutines via `spawn fn_call(args);`.
  Channels typed as `chan<T>`; `make_chan(cap)`, `c.send(x)`,
  `c.recv()`, `c.close()`. OS threads via `spawn_thread`.
- **Methods**: `impl T { fn method(self: *T, …) }`. Macros
  attached to types follow the same shape.
- **Traits + dyn**: `trait T { fn m(self: *Self) }` with
  optional default bodies. `*dyn T` for vtable dispatch.
- **Generics**: `fn f<T>(v: T)`. Bounds via `T: Trait + Trait`.
  Monomorphised; the compiler emits one C fn per concrete T.
- **Macros**: builtins `println!` / `format!` / `assert!`;
  string interpolation `"x={x}"`; `select!` over channels.
  Procedural macros by attribute (`@derive(JsonBind)`,
  `@handler`, `@logged`, `@trace`) and function-like
  `info!` / `warn!` / `error!` / … from `stdlib::log`.
- **Doc comments**: `///` triple-slash above the decl. Put
  examples in fenced ```glide blocks; show sample output as a
  trailing `// result` line — keep this style.

## 3. Project commands

```
glide new <name> [--lib] [--ai]   scaffold a project
glide run                        build + run from the manifest
glide build [--target=<triple>]  build only (sysroot auto-fetched for cross)
glide check <file>               parse + type-check, no codegen
glide test [path]                run *_test.glide files
glide bench [path]               run *_bench.glide microbenchmarks
glide lint [file]                run all lints (project bin by default)
glide fix [file]                 apply mechanical fixes
glide fmt <file> [--write]       pretty-print, or rewrite in place
glide doc --ai                   regenerate llms.txt for THIS file
glide add <name> <url> <rev>     add a git dep
glide add --local <name> <path>  add a path dep
glide remove <name>              drop a dep from glide.glide
glide fetch                      pull declared deps
glide update                     refresh git deps + rewrite lock
glide install <path|git-url>     install a glide binary/tool
glide target <list|add|remove>   manage cross-compile sysroots
glide clean                      wipe build/ + glide_modules/ + glide.lock
glide lsp                        language server on stdio
```

## 4. House rules

- Don't write `try` / `catch`; use `!T` + `?` propagation.
- Don't malloc without a paired `defer free(…)` unless the
  value is owned by an arena.
- Don't use `*chan<T>` parameters when `chan<T>` suffices —
  the value already IS a pointer at the C layer.
- Don't import `stdlib::http` from a bootstrap context
  (`JsonValue` collides with `bootstrap/json.glide`).
- Prefer the method form (`v.push(x)`) over the free fn
  form (`push(v, x)`) — matches `stdlib::*` conventions.

## 5. Lints (compiler-enforced patterns)

`glide lint <file>` and the LSP run these checks. Each fires
a lint code that `@allow("<code>")` on the enclosing fn or
impl method silences. Don't suppress without a justification
in a comment — the lint usually points at a real bug.

- `null-deref` — using `*p` / `p.field` / `p[i]` where `p` is
  provably null (let-bound, or callee param dereferenced
  without an `if p == null` guard).
- `bad-free` — never `free()` on a Glide `string`, `*Vector`,
  or `*HashMap`. Strings are arena-managed; collections have
  `.free()` destructors that release internal buffers first.
- `unhandled-result` — calling a `!T`-returning fn and
  discarding the value silently swallows the error. Use
  `f(...)?`, `let _ = f(...)`, or `let bound = f(...)`.
- `ignored-option` — `.val` on a `?T` without a preceding
  `is_some()` / `.has` guard reads UB on the none branch.
- `use-after-free` — accessing `x` after `x.free()` in the
  same scope is a dangling-pointer read.
- `arena-set` — `__glide_palloc_set(arena)` without a paired
  `defer __glide_palloc_set(prev);` leaks the arena bracket.
- `coro-blocking` — calling sync I/O (`fs_read`, `process_run`,
  etc.) inside a `spawn fn();` stalls the M:N worker. Switch
  to `spawn_thread` or the async variant.
- `mutex-unbalanced` — every `.lock()` needs `.unlock()` on
  every exit (return / `?` / break / continue / fall-through).
  The idiom is `m.lock(); defer m.unlock();` right after.
- `chan-leak` — channels missing `close()` on any path will
  leave receivers parked forever on `recv()`.
- `leak-on-early-return` — register `defer x.free();`
  IMMEDIATELY after the allocation, never after a `?`.

Patterns to follow as default form:

```glide
// open a coro-arena bracket
let prev: *void = __glide_palloc_get();
__glide_palloc_set(__glide_palloc_make());
defer __glide_palloc_set(prev);
defer __glide_palloc_free(__glide_palloc_get());

// allocation + cleanup
let v: *Vector<int> = Vector::new();
defer v.free();

// mutex
m.lock();
defer m.unlock();

// fallible call
let x: int = parse(s)?;

// option unwrap with check
let r: ?int = lookup(k);
if !r.has { return err("missing"); }
let v: int = r.val;
```

## 6. Tool-specific notes

- **Claude Code / Codex**: this file loads automatically.
- **Cursor**: also reads `AGENTS.md`; for older versions,
  copy this file to `.cursorrules`.
- **Continue / Aider / others**: point your config at this
  file or copy its contents into the tool's rules file.

---

_This file is auto-generated by `glide doc --ai` only when it
doesn't already exist. Subsequent runs leave your customisations
alone; the `build/doc/llms.txt` reference is regenerated every
time._

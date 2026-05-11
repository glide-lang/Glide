<p align="center">
  <img src="assets/glide-horizontal.svg" alt="Glide" width="320">
</p>

<p align="center">
  <b>A small systems language with safe memory, real concurrency, and inline assembly.</b>
</p>

Glide compiles to native machine code through a portable C backend.
The language is small — every feature listed below is in the box, not
behind a flag or an experimental release. The compiler is self-hosted
in Glide itself, ships with the standard library and a bundled C
toolchain, and exposes a built-in package manager plus an LSP server.

## features

### Memory model

- **Scope-bound borrows** (`&T`, `&mut T`) catch dangling references,
  aliased mutation, and use-after-free at compile time. No lifetime
  annotations.
- **Auto-drop allocation**: `let p: *T = T { ... };` allocates on the
  heap and frees at scope exit. Use `defer expr;` for arbitrary
  cleanups and `defer_err expr;` for cleanups that run only on the
  error return path.
- **Arenas**: `Arena::new(cap)` plus `arena.create(T)` for groups of
  objects with the same lifetime; freed in one shot when the arena
  drops.
- **Manual escape hatch** (`malloc`/`free`) for the rare case where
  the higher-level shapes don't fit.

### Errors as values

`!T` is a result; `?T` is an option; `?!T` is the combined form. The
postfix `?` operator propagates failures; `??` is the coalesce
operator.

```glide
fn parse(n: int) -> !int {
    if n < 0 { return err("negative"); }
    return ok(n * 2);
}

fn pipeline(n: int) -> !int {
    let v: int = parse(n)?;
    return ok(v + 1);
}
```

### Concurrency

M:N coroutines via `spawn`. Typed channels via `chan<T>` with
`make_chan(cap)`, `c.send(x)`, `c.recv()`, `c.close()`. The `select!`
macro multiplexes over channel operations with optional `default` and
close-aware arms. OS threads are available through `spawn_thread`
when you need them.

```glide
fn worker(c: chan<int>) {
    c.send(42);
}

fn main() -> int {
    let c: chan<int> = make_chan(1);
    spawn worker(c);
    return c.recv();
}
```

### Generics + traits

Generics are monomorphised. Bounds use `T: Trait + Trait`. `trait`
declares a contract and supports default method bodies and
supertraits. `*dyn Trait` performs runtime dispatch through a vtable
for heterogeneous collections.

```glide
trait Render { fn render(self: *Self) -> string; }

impl Render for Box    { fn render(self: *Box)    -> string { return "Box"; } }
impl Render for Circle { fn render(self: *Circle) -> string { return "Circle"; } }

fn show(r: *dyn Render) { println!(r.render()); }
```

### Metaprogramming

- **`macro_rules!`-style macros** with matchers (`$x:expr`,
  `$x:ident`, `$x:ty`) and the variadic form `$($x:expr),*`.
- **Procedural macros** run in the compiler at expansion time via an
  embedded interpreter. Four flavours: `@derive(Name)`,
  `@<attr>(...)`, `@<name>!(args)`, and `@<name>_str!("body")`. Same
  AST nodes the compiler uses internally — no separate plugin
  toolchain.
- **Compile-time location macros**: `file!()`, `line!()`, `column!()`,
  `function!()` expand to literal values at the call site.

### Low-level controls

Inline assembly accepts GCC-style operand constraints. `naked fn`
emits a function with no prologue or epilogue so you can lay down
your own calling convention. `c_raw! { ... }` injects literal C and
`@cfg("posix" | "windows")` gates declarations per platform.

```glide
fn read_tsc() -> u64 {
    let lo: u32 = 0;
    let hi: u32 = 0;
    asm volatile { "rdtsc" : "=a"(lo), "=d"(hi) }
    return ((hi as u64) << 32) | (lo as u64);
}
```

### Standard library

Bundled with the compiler:

| Area | Modules |
| --- | --- |
| Collections | `vector`, `hashmap`, `iter`, `bytes` |
| Strings + numbers | `string` methods, `math`, `base64`, `hex` |
| I/O + system | `io`, `fs`, `os`, `env`, `process`, `signal` |
| Time + concurrency | `time`, `sync` (Mutex / Atomic / WaitGroup) |
| Crypto | `crypto` (SHA-256, HMAC, SHA-1), `compress` (gzip) |
| Networking | `net` (TCP / UDP / DNS / IP), `net::tls`, `net::ws` |
| HTTP | server (`http_listen`, `https_listen`), client, routing, middleware, cookies, JWT, multipart, SSE, static files, proxy, HTTP/2 |
| Mail | SMTP / POP3 / IMAP clients + RFC 5322 / MIME |
| CLI | `argparse`, `spinner`, `backtrace`, `bench`, `testing` |
| Archives | `tar` (USTAR) |
| Serialisation | `json`, typed JSON binding, `@derive(JsonBind)` |

### Tooling

- Package manager: `glide new`, `glide add`, `glide fetch`,
  `glide update`, `glide install`, `glide clean`. Path deps + git
  deps (GitHub tarball fast-path, git-clone fallback for the rest).
  Minimum-version selection across the dep graph; SHA-256-locked
  cache.
- Built-in documentation generator: `glide doc` emits static HTML;
  `glide doc --serve` runs a local preview server; `glide doc --ai`
  also drops an `AGENTS.md` rules file plus a flat `llms.txt`
  summary intended for AI ingestion.
- Test runner: `glide test` discovers `*_test.glide` files and runs
  every `fn test_*` inside them; `glide test --golden` does stdout
  diffing.
- Benchmark runner: `glide bench` auto-tunes iteration counts and
  reports ns/op.
- LSP server: hover, completion, goto-definition (locals, params,
  fns, structs, enums, traits, macros, proc-macros, import segments,
  fields), references, rename, formatting, project-aware indexing
  across stdlib + dependencies.
- Tree-sitter grammar in `glide-grammar/`; Zed extension in
  `zed-extension/`; VS Code extension in `vscode-extension/`.

## install

Per-user install — no admin rights needed.

**Linux / macOS:**

```bash
curl -fsSL https://github.com/glide-lang/Glide/releases/latest/download/install.sh | bash
```

**Windows (PowerShell):**

```powershell
iwr https://github.com/glide-lang/Glide/releases/latest/download/install.ps1 -UseB | iex
```

Open a new shell once the installer finishes; `glide --version`
should print the installed version.

## quick start

```bash
glide new my_app          # scaffold a project
cd my_app
glide run                 # build + run from the manifest
```

For libraries:

```bash
glide new my_lib --lib    # no `bin`, src/lib.glide with a sample pub fn
```

Project layout:

```
my_app/
├── glide.glide       # manifest: name, version, bin, deps
├── src/
│   └── main.glide
├── build/            # compiler output (auto-managed, gitignored)
├── glide_modules/    # fetched / linked dependencies
└── glide.lock        # resolved revs + content hashes
```

## commands

```
glide new <name> [--lib] [--ai] [--no-vcs]
glide run                                     # build + run from manifest
glide build [<file>] [-o <out>] [--target=<triple>]
glide check <file>                            # type-check only
glide fmt <file> [--write]
glide test [path]
glide test --golden <dir>
glide bench [path]
glide doc [--open] [--serve[=<port>]] [--ai]
glide add <name> <git-url> <rev>
glide add --local <name> <path>
glide remove <name>
glide fetch
glide update
glide clean
glide install <path>
glide install <git-url> <rev>
glide lsp                                     # LSP server on stdio
glide --version
```

Cross-compile via `--target=<triple>` — any target the bundled C
toolchain (Zig) supports: `x86_64-linux-{gnu,musl}`,
`aarch64-linux-{gnu,musl}`, `x86_64-windows-{gnu,msvc}`,
`aarch64-macos`, `x86_64-macos`, `riscv64-linux-musl`, and others.

## build from source

The compiler is written in Glide and ships a C seed that compiles to
a bootstrap binary; from there Glide builds itself.

```bash
git clone https://github.com/glide-lang/Glide.git
cd Glide

# Windows hosts need -lws2_32 for the socket builtins.
cc bootstrap/seed/bootstrap.c -o glide_seed -O2 -lpthread -lm           # POSIX
cc bootstrap/seed/bootstrap.c -o glide_seed -O2 -lpthread -lm -lws2_32  # Windows

bash tools/install_zig.sh                                # fetch the bundled C toolchain
./glide_seed build bootstrap/main.glide -o glide         # self-host
./glide install .                                        # drop the binary into ~/.glide/bin
```

A release archive (host binary + bundled toolchain + stdlib) can be
built with `bash tools/build_release.sh`.

## tour

A single program exercising every language feature lives in
`examples/tour.glide`:

```bash
glide run examples/tour.glide
```

## license

MIT.

<p align="center">
  <img src="assets/glide-horizontal.svg" alt="Glide" width="320">
</p>

<p align="center">
  <b>A small systems language with safe memory, real concurrency, and inline assembly.</b>
</p>

Glide compiles to native machine code through a portable C backend. Memory is
safe without a garbage collector or lifetime annotations, concurrency is real
(M:N coroutines + typed channels), and the compiler is self-hosted in Glide.
It ships with the standard library, a bundled C toolchain, a package manager
and an LSP server — everything in the box, nothing behind a flag.

## install

Per-user, no admin rights.

**Linux / macOS**

```bash
curl -fsSL https://github.com/glide-lang/Glide/releases/latest/download/install.sh | bash
```

**Windows (PowerShell)**

```powershell
iwr https://github.com/glide-lang/Glide/releases/latest/download/install.ps1 -UseB | iex
```

Open a new shell, then `glide --version`. You also need a system C compiler on
PATH for the final link step — `gcc` (Linux), Xcode CLT (macOS), or MSYS2/UCRT
`cc` (Windows).

## hello

```bash
glide new my_app
cd my_app
glide run
```

```glide
fn parse(n: i32) -> !i32 {
    if n < 0 { return err("negative"); }
    return ok(n * 2);
}

fn worker(c: chan<i32>) {
    c.send(parse(20).unwrap_or(0));
}

fn main() -> i32 {
    let c: chan<i32> = make_chan(1);
    spawn worker(c);
    println!("got", c.recv());          // got 40
    return 0;
}
```

## what's in the box

- **Memory** — scope-bound borrows (`&T` / `&mut T`) catch dangling refs,
  aliased mutation and use-after-free at compile time. `let p: *T = T { ... }`
  heap-allocates and frees at scope exit; `defer` / `defer_err` for cleanups;
  arenas for group lifetimes. No GC, no lifetime annotations.
- **Errors as values** — `!T` result, `?T` option, `?!T` combined. Postfix `?`
  propagates, `??` coalesces, and `.unwrap` / `.map` / `.and_then` / `.ok_or`
  read like methods.
- **Concurrency** — `spawn` for M:N coroutines, typed `chan<T>`, the `select!`
  macro, `spawn_thread` for OS threads.
- **Generics + traits** — monomorphised generics with `T: Trait` bounds,
  default methods, supertraits, and `*dyn Trait` runtime dispatch.
- **Metaprogramming** — `macro_rules!`-style macros plus procedural macros
  (`@derive`, attributes, function-like) that run inside the compiler.
- **Low-level** — inline `asm`, `naked fn`, `c_raw! { ... }` for literal C, and
  `@cfg("posix" | "windows")` platform gating.
- **Standard library** — collections, strings/math, `io`/`fs`/`os`/`env`/
  `process`, `time`, `sync`, `crypto`, `compress`, a full `net` stack
  (TCP/UDP/DNS/TLS/WebSocket and an HTTP/1+2 server & client), mail (SMTP/POP3/
  IMAP), `json` + `@derive(JsonBind)`, `argparse`, `testing`, and more.
- **Tooling** — package manager, LSP, formatter, test + bench runners, a doc
  generator, and a suite of bug-prevention lints (`null-deref`, `bad-free`,
  `unhandled-result`, `use-after-free`, `mutex-unbalanced`, …). Tree-sitter
  grammar + [Zed](https://github.com/glide-lang/zed-glide) and
  [VS Code](https://github.com/glide-lang/vscode-glide) extensions.

## commands

```
glide new <name> [--lib]      scaffold a project
glide run                     build + run from the manifest
glide build [<file>] [-o <out>] [--target=<triple>]
glide check <file>            type-check only
glide fmt <file> [--write]
glide test [path]             run *_test.glide
glide bench [path]
glide lint <file> [--lint-as-error]
glide doc [--serve[=<port>]]
glide add <user/repo>[@rev]   add a dependency
glide fetch | update | remove <name> | clean
glide target <list|add|remove> [<triple>]   cross-compile sysroots
glide lsp                     language server on stdio
```

## cross-compile

The bundled Zig toolchain builds any `--target=<triple>`
(`x86_64-linux-musl`, `aarch64-macos-none`, `x86_64-windows-gnu`, …) from any
host — no Docker, no WSL. Targets that use `stdlib::http` / `stdlib::net` want
a sysroot first (`glide target add <triple>`) so the openssl/zlib libs are
bundled and the binary stays self-contained; everything else links without one.

## platforms

Linux x86_64 and Windows x86_64 are tested in CI / production (epoll and IOCP
reactors). macOS (x86_64 + arm64), the BSDs, and arm64 Linux/Windows carry
their platform code in tree but aren't hardware-verified yet — bug reports from
those are the fastest way to flip them green.

## build from source

The compiler is self-hosted, so you build it with an existing Glide binary
(the same way CI bootstraps from a published release — there is no C seed):

```bash
git clone https://github.com/glide-lang/Glide.git
cd Glide
glide build bootstrap/main.glide -o glide   # self-host with an installed glide
./glide install .                           # drop it into ~/.glide/bin
```

`bash tools/test_all.sh` runs the unit suite, LSP smoke tests and e2e smokes.
A single program exercising every language feature lives in
`examples/tour.glide` (`glide run examples/tour.glide`).

## license

MIT.

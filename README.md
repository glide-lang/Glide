<p align="center">
  <img src="assets/glide-horizontal.svg" alt="Glide" width="320">
</p>

<p align="center">
  <b>A small systems language with automatic memory safety, real concurrency, and a batteries-included standard library.</b>
</p>

Glide is a compiled systems language. It gives you native performance through a
portable C backend, memory safety without a garbage collector and without
lifetime annotations, and real M:N concurrency. The whole compiler is written
in Glide itself.

## Install

**Linux / macOS** — installs to `~/.local/bin`, no sudo:

```sh
curl -fsSL https://raw.githubusercontent.com/glide-lang/Glide/main/tools/install.sh | bash
```

**Windows** — PowerShell, installs under `%LOCALAPPDATA%` and updates your PATH:

```powershell
irm https://raw.githubusercontent.com/glide-lang/Glide/main/tools/install.ps1 | iex
```

Both fetch the latest release automatically. The default is a ~3 MB binary that
compiles your code with the `gcc`/`clang` already on your machine. Want a
toolchain that needs nothing else installed? Add `--bundle` (bash) or `-Bundle`
(PowerShell) — the self-contained build ships its own C backend. Prebuilt
archives for every platform are also on the
[releases page](https://github.com/glide-lang/Glide/releases/latest).

## Hello, Glide

```glide
fn main() {
    println!("hello, glide");
}
```

```sh
glide run hello.glide      # build and run
glide build hello.glide    # produce a standalone binary
```

`glide new <name>` scaffolds a project; inside it, `glide build` reads the
`glide.glide` manifest and produces a single static executable that runs
anywhere on that OS/arch — no runtime, no shared libraries to ship.

## The language

Memory is managed for you. A value is owned by default, moves when you hand it
off, and frees itself at the end of its scope, so the common case needs no
manual allocation or cleanup — and use-after-move and double-free are caught at
compile time. Borrows, a manual heap, and arenas stay available for the moments
you want to take control.

Errors are values rather than exceptions: results and options carried in the
type system, propagated with light postfix operators. Concurrency is built in:
spawn lightweight coroutines and pass data over typed channels. The standard
library ships networking with a full HTTP stack, its own TLS 1.2/1.3 and
constant-time crypto, JSON, compression, a package manager, a formatter, and a
language server.

## Self-sufficient by design

Glide links no third-party C library. Its TLS, crypto, HTTP, and compression
are written in Glide, so a build depends only on a C compiler and libc — and
`glide build` produces a fully static binary by default (musl on Linux) that
runs anywhere with no `.so` to install. The **bundle** build goes one step
further and embeds its own C backend, so it compiles your code with nothing
else on the system at all. Cross-compiling to another OS/arch fetches only the
target's libc, once, on first use.

## Documentation

- [TUTORIAL.md](TUTORIAL.md) — a guided walkthrough from install to a running program.
- [LANGUAGE.md](LANGUAGE.md) — the full language reference.
- [DEVELOPING.md](DEVELOPING.md) — working on the compiler itself.

## License

MIT.

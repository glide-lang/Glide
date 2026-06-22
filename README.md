<p align="center">
  <img src="assets/glide-horizontal.svg" alt="Glide" width="320">
</p>

<p align="center">
  <b>A small systems language with safe memory, real concurrency, and inline assembly.</b>
</p>

Glide compiles to native machine code through a portable C backend. You get
safe memory without a garbage collector or lifetime annotations, real M:N
concurrency, generics and traits, and a batteries-included standard library —
and the whole compiler is written in Glide itself.

```glide
fn parse(n: i32) -> !i32 {
    if n < 0 { return err("negative"); }
    return ok(n * 2);
}

fn main() -> i32 {
    println!(parse(20).unwrap_or(0));   // 40
    return 0;
}
```

### What makes it Glide

- **Safe memory** — scope-bound borrows, auto-drop heap allocations, and arenas
  catch dangling references and use-after-free at compile time. No GC, no
  lifetime annotations.
- **Errors as values** — `!T` results and `?T` options, with the `?` and `??`
  operators instead of exceptions.
- **Real concurrency** — `spawn` for M:N coroutines and typed `chan<T>`.
- **Batteries included** — a full networking/HTTP stack, crypto, JSON, a package
  manager, an LSP server and a formatter, all in the box.

### Try it

```bash
curl -fsSL https://github.com/glide-lang/Glide/releases/latest/download/install.sh | bash
glide new hello && cd hello && glide run
```

On Windows use the PowerShell installer from the
[releases page](https://github.com/glide-lang/Glide/releases/latest). A system C
compiler (`gcc` / `clang` / MSVC) needs to be on PATH for the final link.

## license

MIT.

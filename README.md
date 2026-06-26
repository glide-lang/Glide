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

Memory is managed for you. A value is owned by default, moves when you hand it
off, and frees itself at the end of its scope, so the common case needs no
manual allocation or cleanup, and use-after-move and double-free are caught at
compile time. Borrows, a manual heap, and arenas stay available for the moments
you want to take control.

Errors are values rather than exceptions: results and options carried in the
type system, propagated with light postfix operators. Concurrency is built in:
spawn lightweight coroutines and pass data over typed channels. The standard
library ships networking and a full HTTP stack, crypto, JSON, a package
manager, a formatter, and a language server.

Glide is plug-and-play. A release is a single self-contained binary that
carries its own toolchain, so a fresh install builds and cross-compiles offline
with nothing else to set up.

## Documentation

- [TUTORIAL.md](TUTORIAL.md) is a guided walkthrough from install to a running program.
- [LANGUAGE.md](LANGUAGE.md) is the full language reference.
- [DEVELOPING.md](DEVELOPING.md) covers working on the compiler itself.

Prebuilt binaries for Linux, macOS, and Windows are on the
[releases page](https://github.com/glide-lang/Glide/releases/latest).

## License

MIT.

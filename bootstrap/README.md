# bootstrap/ — the self-hosted Glide compiler

The compiler is written in Glide and lives here. `main.glide` is the entry
point (the manifest's `bin`); everything else is organized into layers that
mirror the compilation pipeline.

## Layout

```
bootstrap/
  main.glide          CLI driver + command dispatch (build/run/check/test/fmt/doc/…)
  runtime/            C runtime baked into every program (sched, reactor, prelude, *.tmpl)
  syntax/             source -> AST -> lowered AST
    lexer  ast  parser  expander  interp  lower
  sema/               semantic analysis
    typer
  codegen/            lowered AST -> C
    emit  runtime  decls
  lsp/                language server
    server  analysis  completion  features
  pkg/                module loading + package resolution
    loader  manifest  fetch  semver
  cli/                CLI-facing tooling
    diagnostics  fmt  doc  openapi  embed
  util/               shared utilities
    json
```

`codegen/runtime` generates per-type C (Display/Option/Result/tuple/chan/dyn)
— distinct from `runtime/`, which is the hand-written static C runtime.

## Import convention

Modules are addressed by their full path from the repo root, with `::`
mapping to `/`:

```glide
import bootstrap::syntax::lexer;
import bootstrap::sema::typer;
import bootstrap::codegen::emit;
```

This resolves the same from any file because the import resolver tries the
path relative to the current working directory (always the repo root when
building via `glide build bootstrap/main.glide`). Subfolder-to-subfolder
imports need the full `bootstrap::…` prefix — relative `..` paths are not a
thing in Glide imports.

Cross-module calls require the callee to be `pub` and the caller to `import`
the module. Modules may import each other freely: the loader de-duplicates
loading and visibility is resolved whole-program, so import cycles are fine.

`stdlib::…` imports (e.g. `import stdlib::hashmap::*;`) resolve against the
language's implicit `src/` source root and are unaffected by this layout.

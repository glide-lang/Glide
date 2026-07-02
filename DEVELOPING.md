# Developing the Glide compiler

This document is for people working on the compiler itself. If you just want to
use Glide, see `README.md`.

## the bootstrap loop

The compiler is written in Glide. There is no checked-in C seed: to break the
chicken-and-egg you bootstrap from a **previously published release binary**
(grab one for your platform from the GitHub Releases page) and use it to build
the current sources. Any reasonably recent release can compile `main`.

```bash
# 1. Download a release binary for your platform and put it on PATH (or use it
#    by path) from https://github.com/glide-lang/Glide/releases.

# 2. Make sure a C compiler is installed (any gcc/clang; Xcode CLT on macOS,
#    MSYS2 ucrt64 gcc on Windows). Cross-compilation additionally needs
#    clang + lld.

# 3. Use the downloaded compiler to build the real compiler from source
glide build bootstrap/main.glide -o glide      # `glide` = the downloaded binary

# 4. (optional) verify self-host
./glide build bootstrap/main.glide -o glide_gen2
./glide_gen2 build bootstrap/main.glide -o glide_gen3   # should match
```

Once `glide` works you can rebuild it with itself; the downloaded binary is only
needed for the very first build on a fresh checkout. CI and release do the same
(see `.github/workflows/*.yml`, the "Bootstrap compiler from a published
release" step).

## project structure

```
bootstrap/
  main.glide        driver: arg parsing, build/run/emit/check/fmt/lsp/test
  syntax/           lexer, parser, ast, expander, lower, interp
  sema/             typer.glide: type + borrow checker (collects diagnostics)
  codegen/          emit.glide (AST -> C), decls.glide, runtime.glide
  lsp/              server, analysis, completion, features (JSON-RPC server)
  pkg/              loader, manifest, fetch, semver (module + package resolution)
  cli/              fmt, diagnostics, doc, openapi, embed
  util/             json.glide: minimal JSON parser + emitter for the LSP

  runtime/          C runtime fragments emitted into every output:
                    sched.c (M:N scheduler), reactor.c (async I/O),
                    chan.c.tmpl (typed chan template), netcore.c,
                    http_parse.c, prelude.c, result/option templates

src/
  builtins/         auto-injected into every program (Vector, string
                    methods, builtins.glide stubs for println / format /
                    panic, plus the per-keystroke arena allocator
                    (__glide_palloc + friends) used by the LSP).
  stdlib/           opt-in via `import stdlib::module`. hashmap, fs, os,
                    env, io, time, http, net, math, crypto, etc.

tests/              *_test.glide files driven by `glide test`.

tools/
  install.{sh,ps1}            install a Glide release archive
  build_release.sh            package the small release artifact
  build_sysroot.sh            build a per-triple sysroot (static deps)
  gen_embedded_blobs.sh       bake stdlib + sysroots into the bundle binary
  dev_install.sh              cross-build + install into ~/.glide for live use
  lsp_smoke.py                end-to-end LSP smoke test
  test_all.sh                 unit + LSP + end-to-end suites

glide-grammar/      tree-sitter grammar (used by the Zed extension).
                    Editor extensions are separate repos:
                    github.com/glide-lang/zed-glide (Zed)
                    github.com/glide-lang/vscode-glide (VS Code / Open VSX)

assets/             logos and icons
```

The compiler builds by following `import`s from `bootstrap/main.glide`, not by
flattening a directory. Modules address each other from the repo root
(`import bootstrap::sema::typer;`); `stdlib::` resolves under `src/`.

## edit-build cycle

When you change anything under `bootstrap/`:

```bash
./glide build bootstrap/main.glide -o glide_new
mv glide_new glide          # replace the running compiler
./glide check some_test.glide
```

There is no seed to maintain: the bootstrap binary is just a published release,
and a release's runtime/codegen lags the working tree by one generation, so most
changes — including ones that touch the emitted runtime in `codegen/runtime.glide`
— propagate naturally through the two-build chain (release -> `glide` -> `glide`).
The only thing to watch is **using a brand-new language feature inside the
compiler sources before any release understands it**; in that case land the
feature in a release first, then bump `BOOT_TAG` in the workflow.

## testing

```bash
# Run every *_test.glide under the current dir. See TESTING.md for the
# assert! / assert_eq! macros and the L1/L2/L3 conventions.
./glide test
./glide test path/to/specific_test.glide

# Golden tests: each .glide under the directory is run, stdout compared
# to <name>.expected.
./glide test --golden tests/golden/

# Self-host: three generations should produce byte-identical compilers.
./glide build bootstrap/main.glide -o gen2
./gen2 build bootstrap/main.glide -o gen3
diff <(sha256sum gen2) <(sha256sum gen3)

# Full suite: unit + LSP smoke + end-to-end.
bash tools/test_all.sh
```

## working on the LSP

Drive `glide lsp` over stdin with `Content-Length: N\r\n\r\n<payload>` framing.
The easiest harness is `tools/lsp_smoke.py`, which builds the JSON-RPC
envelopes, opens documents, and checks diagnostics plus position features
(hover, completion, goto, rename). Add a case there for any LSP change and keep
it green.

For interactive testing, install the Zed or VS Code extension, point it at your
in-development `glide` binary (PATH or the `glide.path` setting), and watch the
server's `--trace` channel.

### memory model

The LSP holds two arenas at once:

- A **doc arena** (`state.last_arena`) that owns every Vector / HashMap / AST
  node produced by the most recent `run_analysis_and_publish`. It survives
  across requests so `doc.stmts` stays valid for hover / completion / goto, and
  is dropped in bulk on the next reanalysis.
- A **request arena** that wraps every single request dispatch. All the
  per-handler scratch bumps into it and is reclaimed when the dispatch returns.

`__glide_palloc` (in `src/builtins/builtins.glide`) is a chunked bump allocator
backed by `mmap` / `VirtualAlloc`; `__glide_pfree` is safe on either heap or
arena pointers. Outside the LSP path the active arena is null and `__glide_palloc`
falls back to `calloc`, so the build / run / fmt pipelines see no behaviour change.

The cache (`state.parse_cache`) holds the parse + lower output of each
transitively-imported file on the libc heap so it outlives every arena reset.

## working on the formatter

`bootstrap/cli/fmt.glide` walks the AST and emits canonical text. The formatter
runs on any `.glide` file:

```bash
./glide fmt bootstrap/codegen/emit.glide     # to stdout
./glide fmt foo.glide --write                # rewrite in place
```

Round-trip stability is the primary invariant: `format(format(X)) == format(X)`.
Add a test by passing a representative file through twice and `diff`-ing.

## releases

Releases are built by CI, not locally. Tag the commit and push the tag; the
`release` workflow builds the sysroots and binaries on native runners for every
platform and uploads them to the GitHub release.

```bash
git tag -a v0.X.Y -m "Glide 0.X.Y"
git push origin v0.X.Y
```

`GLIDE_VERSION` (in `bootstrap/main.glide`) is bumped to the new version before
the tag. `SYSROOT_VERSION` must still point at the **previous** release at tag
time, because the new version's sysroots aren't published until the workflow
finishes — fetching them would 404. Bump `SYSROOT_VERSION` to the new version in
a follow-up commit once the release run is green. The workflow bootstraps from
the binary named by `BOOT_TAG` in `.github/workflows/release.yml`; raise it only
when an older release can no longer compile `main`.

## known constraints

- The bootstrap parser doesn't track end-position info per node, so the LSP and
  the formatter can only point at the start of a token.
- The lexer drops comments. The formatter therefore drops them too. A
  comment-aware lexer + formatter is the next iteration.
- The LSP's working set is dominated by the AST cache (one parsed + lowered
  Vector per transitively-imported file). Editing a file with a large import
  graph (the `bootstrap/main.glide` shape) costs more than one under
  `src/stdlib/`. Reducing it further needs a smaller AST representation or a
  lazy-import scheme.

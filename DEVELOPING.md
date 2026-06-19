# Developing the Glide compiler

This document is for people working on the compiler itself. If you just want to use Glide, see `README.md`.

## the bootstrap loop

The compiler is written in Glide. There is no checked-in C seed — to break the
chicken-and-egg you bootstrap from a **previously published release binary**
(grab one for your platform from the GitHub Releases page) and use it to build
the current sources. Any reasonably recent release can compile `main`.

```bash
# 1. Download a release binary for your platform and put it on PATH (or use it
#    by path). e.g. for v0.4.0:
#      https://github.com/<owner>/javascomp/releases/tag/v0.4.0
#    The archive bundles its own runtime/zig, so it builds offline.

# 2. (if not using the release's bundled toolchain) fetch the C backend toolchain
bash tools/install_toolchain.sh

# 3. Use the downloaded compiler to build the real compiler from source
glide build bootstrap/main.glide -o glide      # `glide` = the downloaded binary

# 4. (optional) verify self-host
./glide build bootstrap/main.glide -o glide_gen2
./glide_gen2 build bootstrap/main.glide -o glide_gen3   # should match
```

Once `glide` works you can rebuild it with itself; the downloaded binary is only
needed for the very first build on a fresh checkout. CI/release do the same
(see `.github/workflows/*.yml`, the "Bootstrap compiler from a published
release" step).

## project structure

```
bootstrap/
  main.glide        driver: arg parsing, build/run/emit/check/fmt/lsp/test
  lexer.glide       byte stream -> tokens
  parser.glide      tokens -> AST
  ast.glide         Stmt / Expr / Type definitions
  expander.glide    macro_rules! expansion
  lower.glide       desugaring (for-in -> for, default-method copy)
  typer.glide       type + borrow checker (collects diagnostics)
  codegen.glide     AST -> C source (also emits the runtime)
  fmt.glide         AST -> canonical Glide source
  lsp.glide         JSON-RPC server (uses everything above)
  json.glide        minimal JSON parser + emitter for the LSP

  runtime/          C runtime fragments emitted into every output:
                    sched.c (M:N scheduler), reactor.c (async I/O),
                    chan.c.tmpl (typed chan template), socket.c,
                    http_parse.c, prelude.c, stdlib.c (string helpers
                    + arena), result.c.tmpl, spawn_stub_*.tmpl

src/
  builtins/         auto-injected into every program (Vector, string
                    methods, builtins.glide stubs for println / format /
                    panic, plus the per-keystroke arena allocator
                    (__glide_palloc + friends) used by the LSP).
  stdlib/           opt-in via `import stdlib::module`. HashMap, fs, os,
                    env, io, time, http, net, math, hashmap, etc.

examples/           one program per language feature, plus tour.glide
                    that exercises everything.
tests/              *_test.glide files driven by `glide test`.

tools/
  install_toolchain.{sh,ps1}  download the bundled C toolchain into runtime/zig/
  install.{sh,ps1}            install a Glide release archive
  build_release.sh            package glide+stdlib+toolchain into a tarball/zip
  gen_icons.py                rasterize the logo SVG to PNGs

glide-grammar/      tree-sitter grammar (used by the Zed extension)
                    Editor extensions are separate repos:
                    github.com/glide-lang/zed-glide (Zed)
                    github.com/glide-lang/vscode-glide (VS Code / Open VSX)

assets/             logos and icons
```

## edit-build cycle

When you change anything under `bootstrap/`:

```bash
./glide build bootstrap/main.glide -o glide_new
mv glide_new glide          # replace the running compiler
./glide check some_test.glide
```

There is no seed to maintain: the bootstrap binary is just a published release,
and a release's runtime/codegen lags the working tree by one generation, so most
changes — including ones that touch `emit_stdlib_runtime` in `codegen.glide` —
propagate naturally through the two-build chain (release → `glide` → `glide`).
The only thing to watch is **using a brand-new language feature inside the
compiler sources before any release understands it**; in that case land the
feature in a release first, then bump `BOOT_TAG` in the workflows.

## testing

```bash
# Run every *_test.glide under the current dir. See TESTING.md for the
# `assert!` / `assert_eq!` macros and the L1/L2/L3 conventions.
./glide test
./glide test path/to/specific_test.glide

# Golden tests: each .glide under the directory is run, stdout compared
# to <name>.expected.
./glide test --golden tests/golden/

# Self-host: three generations should produce byte-identical compilers.
./glide build bootstrap/main.glide -o gen2
./gen2 build bootstrap/main.glide -o gen3
diff <(sha256sum gen2) <(sha256sum gen3)

# One-liner smoke
echo 'fn main() -> int { return 42; }' > /tmp/h.glide
./glide run /tmp/h.glide                    # exit 42
```

## working on the LSP

Run `glide lsp` and pipe LSP requests via stdin. The protocol uses
`Content-Length: N\r\n\r\n<payload>` framing — easiest to drive from a
small Python script that builds the right JSON-RPC envelopes (there
are throwaway examples under repo root: `__lsp_smoke.py`,
`__lsp_zed_real.py`).

For interactive testing, install the Zed or VSCode extension, point it
at your in-development `glide` binary (PATH or `glide.path` setting),
and check the server's `--trace` channel.

### memory model

The LSP holds two arenas at once:

- A **doc arena** (`state.last_arena`) that owns every Vector / HashMap
  / AST node produced by the most recent `run_analysis_and_publish`.
  It survives across requests so `doc.stmts` stays valid for hover /
  completion / goto, and is dropped in bulk on the next reanalysis.
- A **request arena** that wraps every single request dispatch. All
  the per-handler scratch (completion's `seen` map and item Vectors,
  hover's uses Vector, every `concat`'d label / signature, the JSON
  leaf strings the response is built out of) bumps into it and is
  reclaimed when the dispatch returns.

The active arena is set per phase: `lsp_main` activates the request
arena before dispatch; `run_analysis_and_publish` saves the request
arena, switches to the doc arena, runs parse / expand / lower / type,
then restores the request arena. At end of dispatch the request arena
is freed, doc arena stays alive for cross-request reads.

`__glide_palloc` (in `src/builtins/builtins.glide`) is a chunked bump
allocator backed by `mmap` / `VirtualAlloc`; `__glide_pfree` is a
free that's safe on either heap or arena pointers (returns no-op for
arena-owned pointers, libc free otherwise). Vector and HashMap stamp
themselves "arena-backed" at construction time so push / resize /
free pick the right path.

Outside the LSP path the active arena is null and `__glide_palloc`
falls back to `calloc`, so the build / run / fmt pipelines see no
behaviour change.

The cache (`state.parse_cache`) holds the parse + lower output of
each transitively-imported file on libc heap so it outlives every
arena reset. `load_into_with_cache` temporarily switches the active
arena to null while populating, so cached AST / Vectors / strings
land on libc heap.

### LSP smoke tests

Useful one-off scripts (untracked, in repo root after a debug
session): `__lsp_smoke.py` (storm didChange on one file),
`__lsp_zed_real.py` (didChange + completion + documentHighlight per
iter, Zed's actual pattern), `__lsp_zed_sim.py` (workspace warmup
followed by edits). Run them against the locally-built binary
(`glide_v2.exe` etc) to catch regressions before reinstalling.

## working on the formatter

`bootstrap/fmt.glide` walks the AST and emits canonical text. The formatter can run on any `.glide` file:

```bash
./glide fmt bootstrap/codegen.glide          # to stdout
./glide fmt foo.glide --write                # rewrite in place
```

Round-trip stability is the primary invariant: `format(format(X)) == format(X)`. Add a test by passing a representative file through twice and `diff`-ing.

## releases

```bash
# Bump VERSION in tools/install*.sh, tools/build_release.sh, CHANGELOG.md, etc.
# Tag the commit
git tag v0.X.Y

# Build for each target platform
bash tools/build_release.sh                              # host
bash tools/build_release.sh --target=x86_64-linux        # cross
bash tools/build_release.sh --target=aarch64-macos       # cross

# Push and publish
git push origin main && git push origin v0.X.Y
gh release create v0.X.Y dist/glide-*.{zip,tar.gz} \
    tools/install.sh tools/install.ps1 \
    --title "Glide v0.X.Y" --notes-file CHANGELOG.md
```

The install scripts default to GitHub's `releases/latest/download/` so users get the latest tag automatically.

## known constraints

- The bootstrap parser doesn't track end-position info per node, so the LSP and the formatter can only point at the start of a token.
- The lexer drops comments. The formatter therefore drops them too. Adding a comment-aware lexer + formatter is the next iteration.
- The LSP's baseline working set is dominated by the AST cache (one
  parsed + lowered Vector per transitively-imported file). For files
  that import the whole bootstrap (`bootstrap/main.glide` shape) this
  is around 1.3 GiB. Editing files with smaller import graphs
  (`bootstrap/lsp.glide`, anything in `src/stdlib/`, `examples/`)
  costs proportionally less. Reducing further needs a smaller AST
  representation (struct-of-arrays, Idx-based references) or a
  lazy-import scheme.
- No `?T` nullable type yet — only `&T` borrows are non-null by typer enforcement. `*T` may be null at runtime.

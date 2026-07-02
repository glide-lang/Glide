# Contributing to Glide

Thanks for your interest. This document covers the practical bits.

## getting started

1. Read `DEVELOPING.md` to set up the bootstrap loop.
2. Pick something to work on:
   - Open issues tagged `good first issue` are the easiest entry points.
   - The roadmap items in `CHANGELOG.md` under "Known limitations" are all fair game.
   - Find a bug? File an issue or a PR.

## code style

The compiler is written in Glide, so it eats its own dogfood. Use `glide fmt --write` on any file you touch:

```bash
glide fmt bootstrap/parser.glide --write
```

Beyond what the formatter handles:

- 4-space indent (the formatter enforces this)
- No trailing whitespace, LF line endings
- Names: `snake_case` for fns and locals, `PascalCase` for types and structs, `SCREAMING_SNAKE_CASE` for constants
- Prefer explicit types on public functions; locals can lean on inference
- Don't introduce abstractions for hypothetical future requirements

If you change runtime helpers in `codegen.glide`, see the bootstrap-loop notes in `DEVELOPING.md` — you may need to patch `bootstrap/seed/bootstrap.c` and regenerate it.

## commit messages

Use [conventional commit prefixes](https://www.conventionalcommits.org/) — they show up in `git log` and feed the changelog:

```
feat(lsp): add hover for borrow types
fix(typer): reject `&null` in let initializer
chore: bump openssl to 3.5.8
docs(tutorial): clarify auto-drop pattern
refactor(codegen): pull mono dispatch into a helper
release: 0.2.0
```

Body is optional; lead with the *why* when the change isn't obvious from the title.

## pull requests

- Branch off `main`. Keep commits clean (one logical change per commit when possible).
- Run `glide check bootstrap/main.glide` to catch regressions in the compiler.
- Run `./glide build bootstrap/main.glide -o glide_pr` and verify gen2 self-host works (`./glide_pr build bootstrap/main.glide -o glide_pr2`).
- Run `./glide test` to make sure the testing framework's checks still pass; add `*_test.glide` files for new behavior (see `TESTING.md`).
- If your change touches the LSP path, smoke-test it before reinstalling the binary — the throwaway scripts at the repo root (`__lsp_smoke.py` etc.) drive `glide lsp` over JSON-RPC and watch RSS over a few hundred didChange iterations.
- If you change source under `bootstrap/`, regenerate `bootstrap/seed/bootstrap.c` in the same PR (or note in the PR that the seed needs a follow-up).
- Update `CHANGELOG.md` under the next release header.

## reporting bugs

Include:

- The exact command and source that triggers the bug
- Output of `glide --version` (when that lands; for now: `git rev-parse HEAD`)
- Your platform (`uname -a` / `systeminfo` / `sw_vers`)

Reduced repro is appreciated but not required for first reports.

## license

By contributing, you agree your work is licensed under the MIT License (`LICENSE`).

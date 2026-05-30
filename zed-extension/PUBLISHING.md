# Publishing the Zed extension

Zed has no upload CLI. Extensions are added by submitting a PR to
[`zed-industries/extensions`](https://github.com/zed-industries/extensions),
which references each extension as a git **submodule**. So this directory
has to live at the root of its own repository first.

## 1. Create the standalone repo (one-time)

This directory (`dist/zed-glide/`) is the extension root — `extension.toml`
sits at the top level, which is what Zed requires.

```bash
cd dist/zed-glide
git init -b main
git add .
git commit -m "Glide Zed extension 0.3.2"
gh repo create glide-lang/zed-glide --public --source=. --push
```

## 2. Open the PR to zed-industries/extensions

```bash
gh repo fork zed-industries/extensions --clone
cd extensions
git submodule add https://github.com/glide-lang/zed-glide extensions/glide
```

Add this block to the top-level `extensions.toml` (keep the file sorted —
run `pnpm install && pnpm sort-extensions`):

```toml
[glide]
submodule = "extensions/glide"
version = "0.3.2"
```

Then:

```bash
git add extensions.toml .gitmodules extensions/glide
git commit -m "Add Glide extension"
git push
gh pr create --repo zed-industries/extensions \
  --title "Add Glide extension" \
  --body "Adds the Glide language extension (LSP via \`glide lsp\` + tree-sitter grammar)."
```

Zed's CI compiles `src/glide.rs` to `wasm32-wasip1`, validates the
grammar, and merges. After merge it appears in Zed's extension list.

## 3. Updating later

Bump `version` in `extension.toml` and `Cargo.toml`, push to
`glide-lang/zed-glide`, then in the extensions repo bump the submodule
pointer + the `version` in `extensions.toml` and open another PR.

## Notes
- The grammar is fetched from the main Glide repo by commit (see
  `[grammars.glide]` in `extension.toml`); bump that pin when the
  `glide-grammar/` changes.
- The extension resolves the `glide` binary from the worktree PATH /
  `~/.glide/bin` (see `src/glide.rs`); it does not download the compiler.
- Validate locally before submitting: `cargo check --target wasm32-wasip1`.

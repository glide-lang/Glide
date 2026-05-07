# Zed extension for Glide

Registers the Glide language, launches `glide lsp`, and ships a Tree-sitter
grammar.

LSP features: diagnostics, hover, goto-definition, find references,
document highlight, document symbols, completion (keywords, locals,
top-level decls, struct fields after `.`, qualified `Type::method`,
import paths), rename + prepareRename, document formatting.

## Install

1. Put `glide` on `PATH`. From a release archive:

   ```bash
   curl -fsSL https://github.com/glide-lang/Glide/releases/latest/download/install.sh | bash
   ```

   or build from source per the repo root `README.md` (`cc bootstrap/seed/bootstrap.c -o glide_seed && ./glide_seed build bootstrap/main.glide -o glide`) and put the resulting `glide` binary on your PATH.

2. Generate the Tree-sitter parser (one-time):

   ```bash
   cargo install tree-sitter-cli
   cd grammars/glide
   tree-sitter generate
   ```

3. In Zed, command palette: `zed: install dev extension`, pick this directory.

## Updating the grammar

After editing `grammar.js` or `queries/highlights.scm`:

```bash
cd zed-extension/grammars/glide
tree-sitter generate
cd ../../..
git add zed-extension/grammars/glide
git commit -m "grammar: ..."
git rev-parse HEAD
```

Paste the SHA into `commit = "..."` in `extension.toml`, then
`zed: rebuild dev extension` in Zed.

## Updating the compiler

After rebuilding `glide` (`./glide build bootstrap/main.glide -o glide_new && cp glide_new $(which glide)`), reopen the workspace so Zed re-spawns the LSP. The LSP runs every doc analysis through a per-keystroke arena, so it doesn't need restarts to recover memory between sessions, but a fresh spawn is the simplest way to pick up a new binary.

## Format-on-save

Add to your Zed `settings.json`:

```json
{
  "languages": {
    "Glide": {
      "format_on_save": "on",
      "formatter": "language_server"
    }
  }
}
```

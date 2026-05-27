# glide-lsp (Claude Code plugin)

Registers the Glide language server (`glide lsp`) with Claude Code's built-in
`LSP` tool so hover / goto / completion / references / diagnostics work on
`.glide` files from inside Claude Code.

The server command points at the installed compiler (`~/.glide/bin/glide.exe`),
which carries its own `src/stdlib` for indexing. Re-run `bash tools/dev_install.sh`
to refresh it; no plugin change is needed when the compiler is rebuilt.

## Install

```
/plugin marketplace add <repo>/tools/glide-cc-marketplace
/plugin install glide-lsp@glide-local
```

Then restart Claude Code. Verify with the `LSP` tool (hover) on any `.glide` file.

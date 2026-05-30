# Changelog

## 0.3.2

First Marketplace release, aligned with the Glide 0.3.2 compiler.

- Syntax highlighting for `.glide` (TextMate grammar).
- Language server integration via `glide lsp`: diagnostics, hover,
  go-to-definition, find references, completion, signature help, rename,
  document/workspace symbols, semantic tokens, inlay hints, code actions
  (including "Add dep to glide.glide" and "Fetch missing dep"),
  formatting, and call/type hierarchy.
- `glide.path` setting to point at a non-PATH `glide` binary.
- `glide.trace.server` setting for LSP message tracing.

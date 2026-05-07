# Glide for Visual Studio Code

Syntax highlighting + Language Server Protocol integration for the Glide programming language.

## Features

- Diagnostics: parse errors, type errors, borrow checker, null safety, unused vars / fns / params, unnecessary `mut`, dead code after return, missing return, arena leaks, `&temporary`, missing trait method, unsatisfied bound, trait-method-mismatch.
- Hover: function signatures, struct / enum / trait declarations, doc-comment extraction, keyword and macro builtin docs.
- Goto definition (functions, struct fields, impl methods, qualified paths, chained inline ctors).
- Find references / document highlight across the open file.
- Document symbols (outline).
- Completion with `.` and `:` triggers: locals, top-level decls, keywords, chan ops on `c.`, `Type::method` paths, struct fields, import paths.
- Rename + prepareRename (rejects keywords, precise word range).
- Document formatting via `glide fmt` round-trip.

## Requirements

The `glide` binary must be on your PATH (or set `glide.path` in settings).

## Build

```bash
cd vscode-extension
npm install
npm run compile
```

To launch a development host:

1. Open this folder in VSCode
2. Press `F5`
3. A new VSCode window opens with the extension loaded
4. Open any `.glide` file

To produce a `.vsix` for installation:

```bash
npm install -g @vscode/vsce
vsce package
code --install-extension glide-0.1.0.vsix
```

## Settings

| Key                   | Default | Description                                    |
| --------------------- | ------- | ---------------------------------------------- |
| `glide.path`          | `glide` | Path to the `glide` executable.                |
| `glide.trace.server`  | `off`   | LSP message tracing (`off` / `messages` / `verbose`). |

# Publishing the VSCode extension

Two marketplaces: the Microsoft **Visual Studio Marketplace** (VS Code) and
**Open VSX** (Cursor, VSCodium, Windsurf, Gitpod). Publish to both.

## One-time setup

### Visual Studio Marketplace
1. Create an Azure DevOps org: https://dev.azure.com
2. Create the publisher (ID must be `glide-lang`, matching `package.json`):
   https://marketplace.visualstudio.com/manage
3. Create a Personal Access Token:
   - Azure DevOps → User Settings → Personal Access Tokens → New Token
   - Organization: **All accessible organizations**
   - Scopes: **Marketplace → Manage**
   - Copy the token.

### Open VSX
1. Sign in at https://open-vsx.org with GitHub.
2. Create an `glide-lang` namespace (or publish under your own).
3. Settings → Access Tokens → generate one.

## Publish

```bash
cd vscode-extension
npm install
npm run compile

# Microsoft Marketplace
npx @vscode/vsce login glide-lang     # paste the Azure PAT (once)
npx @vscode/vsce publish              # builds + uploads

# Open VSX (uses the same .vsix)
npx @vscode/vsce package              # produces glide-<version>.vsix
npx ovsx publish glide-*.vsix -p <OPEN_VSX_TOKEN>
```

## Bump a version

Edit `version` in `package.json` and add a `CHANGELOG.md` entry, then
re-run the publish commands. `vsce publish patch|minor|major` bumps and
publishes in one step.

## Notes
- The extension shells out to `glide lsp`; users need `glide` on PATH or
  the `glide.path` setting pointed at the binary. It does not bundle the
  compiler.
- `.vsix` excludes `src/` and dev deps (see `.vscodeignore`) but bundles
  the `vscode-languageclient` runtime dependency.

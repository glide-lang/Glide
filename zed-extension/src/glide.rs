use zed_extension_api as zed;

struct GlideExtension;

/// Locate the Glide compiler binary. Order:
///   1. `glide` on the worktree's PATH (covers cargo-style installs + dev shells).
///   2. `~/.glide/bin/glide[.exe]` — what `glide install` writes to.
///
/// Falling back to the install location means a fresh `glide install .`
/// works in Zed even if the user hasn't restarted yet to pick up the
/// updated user PATH.
fn resolve_glide(worktree: &zed::Worktree) -> Option<String> {
    if let Some(p) = worktree.which("glide") {
        return Some(p);
    }
    let home_var = if cfg!(windows) { "USERPROFILE" } else { "HOME" };
    let home = std::env::var(home_var).ok()?;
    let suffix = if cfg!(windows) { "/.glide/bin/glide.exe" } else { "/.glide/bin/glide" };
    let candidate = format!("{}{}", home, suffix);
    if std::path::Path::new(&candidate).exists() {
        Some(candidate)
    } else {
        None
    }
}

/// Returns method names recognised by `@route`/`@get`/`@post`/etc. Kept
/// in sync with `_is_route_attr_name` in stdlib::http::route.
const ROUTE_ATTRS: &[&str] = &[
    "route", "get", "post", "put", "delete", "patch", "head", "options", "any",
];

#[derive(Clone)]
struct RouteRow {
    method: String,
    path: String,
    handler: String,
}

/// Scan `src` for `@<method>("/path") ... fn handler(...)` shapes.
/// Lightweight regex-free pass so the slash command runs entirely
/// inside the WASM sandbox - we don't need to chase imports.
fn scan_routes(src: &str) -> Vec<RouteRow> {
    let mut out = Vec::new();
    let mut pending: Option<(String, String)> = None;
    for raw in src.lines() {
        let line = raw.trim();
        if let Some(rest) = line.strip_prefix('@') {
            let (name, args) = match rest.find('(') {
                Some(i) => (&rest[..i], &rest[i + 1..]),
                None => continue,
            };
            if !ROUTE_ATTRS.contains(&name) {
                continue;
            }
            let close = match args.rfind(')') {
                Some(i) => i,
                None => continue,
            };
            let inner = args[..close].trim();
            let (method, path) = if name == "route" {
                // @route(METHOD, "/path")
                let mut parts = inner.splitn(2, ',');
                let m = parts.next().unwrap_or("").trim().to_string();
                let p = parts
                    .next()
                    .unwrap_or("")
                    .trim()
                    .trim_matches('"')
                    .to_string();
                (m, p)
            } else {
                let p = inner.trim().trim_matches('"').to_string();
                (name.to_string(), p)
            };
            if !method.is_empty() && !path.is_empty() {
                pending = Some((method.to_uppercase(), path));
            }
            continue;
        }
        if let Some(rest) = line.strip_prefix("fn ") {
            if let Some((method, path)) = pending.take() {
                let handler = rest
                    .split(|c: char| c == '(' || c.is_whitespace())
                    .next()
                    .unwrap_or("")
                    .to_string();
                if !handler.is_empty() {
                    out.push(RouteRow { method, path, handler });
                }
            }
        }
        // Other lines (closing braces, blank lines, doc-comments) preserve
        // `pending` so the @attribute can sit several lines above the fn.
    }
    out
}

fn format_routes_md(target: &str, routes: &[RouteRow]) -> String {
    if routes.is_empty() {
        return format!("No @route'd handlers found in `{}`.", target);
    }
    let max_m = routes.iter().map(|r| r.method.len()).max().unwrap_or(6).max(6);
    let max_p = routes.iter().map(|r| r.path.len()).max().unwrap_or(4).max(4);
    let mut buf = String::new();
    buf.push_str("```\n");
    for r in routes {
        buf.push_str(&format!(
            "{:<mw$}  {:<pw$}  -> {}\n",
            r.method,
            r.path,
            r.handler,
            mw = max_m,
            pw = max_p
        ));
    }
    buf.push_str("```");
    buf
}

impl zed::Extension for GlideExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        let path = resolve_glide(worktree).ok_or_else(|| {
            "glide not found: add `~/.glide/bin` to PATH or run `glide install .`".to_string()
        })?;

        Ok(zed::Command {
            command: path,
            args: vec!["lsp".to_string()],
            env: Default::default(),
        })
    }

    /// `/glide-routes <path/to/file.glide>` — list `@get/@post/...`
    /// handlers from a Glide source file. Single-file scan; doesn't
    /// chase imports (the WASM sandbox has no process spawn, so a full
    /// AST walk would need re-parsing in Rust). Renders as a fenced
    /// `METHOD PATH -> handler` block in the Assistant panel.
    fn run_slash_command(
        &self,
        command: zed::SlashCommand,
        args: Vec<String>,
        worktree: Option<&zed::Worktree>,
    ) -> Result<zed::SlashCommandOutput, String> {
        if command.name != "glide-routes" {
            return Err(format!("unknown slash command: {}", command.name));
        }
        let target = args.first().ok_or_else(|| {
            "usage: /glide-routes <path/to/file.glide>".to_string()
        })?;
        let worktree = worktree.ok_or_else(|| "no worktree available".to_string())?;
        let src = worktree
            .read_text_file(target)
            .map_err(|e| format!("read {}: {}", target, e))?;
        let routes = scan_routes(&src);
        let text = format_routes_md(target, &routes);
        let label = format!("routes in {}", target);
        let section = zed::SlashCommandOutputSection {
            range: (0u32..text.len() as u32).into(),
            label,
        };
        Ok(zed::SlashCommandOutput {
            text,
            sections: vec![section],
        })
    }
}

zed::register_extension!(GlideExtension);

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
}

zed::register_extension!(GlideExtension);

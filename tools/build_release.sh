#!/usr/bin/env bash
# Stage everything a Glide install needs into dist/glide-<os>-<arch>-<ver>/
# and pack it up. Releases are built natively per platform (the CI matrix
# runs this on a runner of each OS/arch) — there is no cross-release mode.
#
# Layout:
#   glide-<os>-<arch>-<ver>/
#     glide(.exe)
#     src/  bootstrap/  [lib/]
#     README.md
#     LICENSE

set -e

VERSION="${VERSION:-0.2.0}"
# Path to a directory of static libs to bundle into the release tarball
# (under `lib/`). Expected to contain libssl.a, libcrypto.a,
# libngtcp2.a, libngtcp2_crypto_ossl.a, libnghttp3.a — once present
# bootstrap/main.glide prefers these over system dynamic libs, so end
# users don't need `apt install libssl-dev libngtcp2-dev` to build
# Glide programs that use stdlib::http / stdlib::http::h3.
#
# Skipped when empty (release tarball still works but assumes the user
# has the libs installed system-wide).
BUNDLE_LIBS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --version=*)     VERSION="${1#--version=}"; shift ;;
        --bundle-libs=*) BUNDLE_LIBS="${1#--bundle-libs=}"; shift ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

# ---- Resolve host (where the script runs) ----
case "$(uname -s)" in
    Linux*)              HOST_OS=linux ;;
    Darwin*)             HOST_OS=macos ;;
    CYGWIN*|MINGW*|MSYS*) HOST_OS=windows ;;
    *) echo "unsupported host OS: $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64|amd64) HOST_ARCH=x86_64 ;;
    aarch64|arm64) HOST_ARCH=aarch64 ;;
    *) echo "unsupported host arch: $(uname -m)" >&2; exit 1 ;;
esac

# ---- Target = host (releases are native per-runner) ----
TARGET_OS="$HOST_OS"
TARGET_ARCH="$HOST_ARCH"

case "$TARGET_OS" in
    windows) EXE_EXT=".exe" ;;
    linux|macos) EXE_EXT="" ;;
    *) echo "unsupported target OS: $TARGET_OS" >&2; exit 1 ;;
esac

NAME="glide-${TARGET_OS}-${TARGET_ARCH}-${VERSION}"
STAGE="dist/${NAME}"
if [ "$HOST_OS" = "windows" ]; then HOST_GLIDE="glide.exe"; else HOST_GLIDE="glide"; fi

if [ ! -x "$HOST_GLIDE" ]; then
    echo "no host $HOST_GLIDE found in repo root. Build it first by bootstrapping" >&2
    echo "from a published release (Glide is self-hosting — there is no C seed):" >&2
    echo "  # download a release binary for your platform from" >&2
    echo "  #   https://github.com/<owner>/javascomp/releases" >&2
    echo "  <downloaded-glide> build bootstrap/main.glide -o $HOST_GLIDE" >&2
    exit 1
fi
# ---- Bundle the host-built glide ----
TARGET_GLIDE="$STAGE/glide${EXE_EXT}"
mkdir -p "$STAGE"
echo ">> Bundling host glide"
cp "$HOST_GLIDE" "$TARGET_GLIDE"

# ---- Stage src/ + docs ----
# `src/builtins/` is auto-injected by the compiler; `src/stdlib/` ships
# alongside it so user code can `import "src/stdlib/X.glide"`.
echo ">> Staging $STAGE"
rm -rf "$STAGE/src" "$STAGE/runtime" "$STAGE/bootstrap"
cp -r src "$STAGE/"
# Bootstrap source ships so proc-macro impls (handler.glide, derive.glide)
# can resolve their `import bootstrap::ast::*` / `import bootstrap::interp::*`
# at expand-time, and so the LSP surfaces goto/hover for `Stmt`/`Expr`/
# `Type` inside the proc-fns.
cp -r bootstrap "$STAGE/"
cp README.md "$STAGE/" 2>/dev/null || true
cp LICENSE "$STAGE/" 2>/dev/null || true

# ---- Stage bundled static libs (optional) ----
# When `--bundle-libs=<dir>` was given, copy the .a files into `lib/`
# so bootstrap/main.glide picks them up via `_host_lib_bundle_dir`.
# Required set: libssl libcrypto libngtcp2 libngtcp2_crypto_ossl
# libnghttp3. Missing entries are tolerated — the linker falls back to
# system dynamic libs for whatever isn't bundled (so the install still
# builds non-h3 code on hosts without ngtcp2).
if [ -n "$BUNDLE_LIBS" ] && [ -d "$BUNDLE_LIBS" ]; then
    echo ">> Bundling static libs from $BUNDLE_LIBS"
    mkdir -p "$STAGE/lib"
    # The bundle builder stages libs under lib/ (with headers as a
    # sibling include/); older flat layouts are still accepted.
    src="$BUNDLE_LIBS"
    [ -d "$BUNDLE_LIBS/lib" ] && src="$BUNDLE_LIBS/lib"
    for lib in libssl.a libcrypto.a libngtcp2.a libngtcp2_crypto_ossl.a libnghttp3.a; do
        if [ -f "$src/$lib" ]; then
            cp "$src/$lib" "$STAGE/lib/"
            echo "   + $lib"
        else
            echo "   - $lib (skipped — not found)"
        fi
    done
fi

# ---- Archive ----
# MSYS/MinGW-bash on Windows host can't `chmod +x` POSIX-style — the
# NTFS layer ignores it, and the tar wires up the binary at 0644. Use
# python's tarfile (portable, present on every host we ship from) to
# fix mode-per-file: 0755 for the binaries that need to be runnable,
# 0644 for everything else.
echo ">> Archiving"
case "$TARGET_OS" in
    windows)
        ( cd dist && rm -f "${NAME}.zip" && \
          if command -v zip >/dev/null 2>&1; then \
              zip -qr "${NAME}.zip" "$NAME"; \
          elif command -v powershell >/dev/null 2>&1; then \
              powershell -NoProfile -Command "Compress-Archive -Path '$NAME' -DestinationPath '${NAME}.zip' -Force"; \
          else \
              python3 -c "import shutil; shutil.make_archive('${NAME}', 'zip', '.', '$NAME')"; \
          fi )
        ARCHIVE="dist/${NAME}.zip"
        ;;
    linux|macos)
        python3 - <<PYEOF
import os, tarfile
name = "${NAME}"
src = os.path.join("dist", name)
out = os.path.join("dist", f"{name}.tar.gz")
exec_files = {f"{name}/glide"}
def fix(ti):
    if ti.isdir():
        ti.mode = 0o755
    elif ti.name in exec_files:
        ti.mode = 0o755
    else:
        ti.mode = 0o644
    return ti
with tarfile.open(out, "w:gz") as t:
    t.add(src, arcname=name, filter=fix)
PYEOF
        ARCHIVE="dist/${NAME}.tar.gz"
        ;;
esac

SIZE=$(du -sh "$ARCHIVE" | cut -f1)
echo ">> Done: $ARCHIVE ($SIZE)"

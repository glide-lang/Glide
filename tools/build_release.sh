#!/usr/bin/env bash
# Stage everything a Glide install needs into dist/glide-<os>-<arch>-<ver>/
# and pack it up.
#
# Modes:
#   tools/build_release.sh                          # host only
#   tools/build_release.sh --target=x86_64-linux    # cross-compile via Zig
#
# When --target is given, the script:
#   1. Downloads the target-platform Zig toolchain into a staging dir
#   2. Cross-builds the glide binary using the local Zig as `cc` with
#      --target=<triple>
#   3. Bundles target glide + target Zig + stdlib
#
# Layout:
#   glide-<os>-<arch>-<ver>/
#     glide(.exe)
#     stdlib/
#     runtime/zig/...
#     README.md
#     LICENSE

set -e

VERSION="${VERSION:-0.1.1}"
ZIG_VERSION="${ZIG_VERSION:-0.14.1}"
TARGET=""
# Path to a directory of static libs to bundle into the release tarball
# (under `lib/`). Expected to contain libssl.a, libcrypto.a, libz.a,
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
        --target=*)      TARGET="${1#--target=}"; shift ;;
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

# ---- Resolve target (what we're building for) ----
if [ -z "$TARGET" ]; then
    TARGET_OS="$HOST_OS"
    TARGET_ARCH="$HOST_ARCH"
else
    # Accept arch-os, e.g. x86_64-linux, aarch64-macos, x86_64-windows
    TARGET_ARCH="${TARGET%-*}"
    TARGET_OS="${TARGET##*-}"
fi

# Map our short OS names to Zig's triplet vocabulary. Linux uses musl
# (not glibc) so the bundled glide binary stays self-contained — and
# the matching sysroot under ~/.glide/targets/x86_64-linux-musl
# carries the zlib + openssl headers / static libs that the stdlib
# c_raw blocks need.
case "$TARGET_OS" in
    windows) ZIG_TRIPLE="${TARGET_ARCH}-windows-gnu";  EXE_EXT=".exe" ;;
    linux)   ZIG_TRIPLE="${TARGET_ARCH}-linux-musl";   EXE_EXT="" ;;
    macos)   ZIG_TRIPLE="${TARGET_ARCH}-macos-none";   EXE_EXT="" ;;
    *) echo "unsupported target OS: $TARGET_OS" >&2; exit 1 ;;
esac

NAME="glide-${TARGET_OS}-${TARGET_ARCH}-${VERSION}"
STAGE="dist/${NAME}"
if [ "$HOST_OS" = "windows" ]; then HOST_GLIDE="glide.exe"; else HOST_GLIDE="glide"; fi

if [ ! -x "$HOST_GLIDE" ]; then
    echo "no host $HOST_GLIDE found in repo root. Build it first:" >&2
    echo "  cc bootstrap/seed/bootstrap.c -o glide_seed -O2 -lpthread -lm" >&2
    echo "  ./glide_seed build bootstrap/main.glide -o $HOST_GLIDE" >&2
    exit 1
fi
if [ ! -d runtime/zig ]; then
    echo "no runtime/zig/ found. Run tools/install_toolchain.sh first." >&2
    exit 1
fi

# ---- Download the target-platform Zig if cross-building ----
TARGET_ZIG_DIR=""
if [ "$TARGET_OS" != "$HOST_OS" ] || [ "$TARGET_ARCH" != "$HOST_ARCH" ]; then
    TARGET_ZIG_DIR="dist/staging/zig-${TARGET_ARCH}-${TARGET_OS}-${ZIG_VERSION}"
    if [ ! -d "$TARGET_ZIG_DIR" ]; then
        echo ">> Fetching Zig $ZIG_VERSION for ${TARGET_ARCH}-${TARGET_OS}"
        case "$TARGET_OS" in
            windows) ZEXT="zip" ;;
            *)       ZEXT="tar.xz" ;;
        esac
        ZNAME="zig-${TARGET_ARCH}-${TARGET_OS}-${ZIG_VERSION}"
        URL="https://ziglang.org/download/${ZIG_VERSION}/${ZNAME}.${ZEXT}"
        TMP="$(mktemp -d)"
        if command -v curl >/dev/null 2>&1; then
            curl -fL --progress-bar -o "$TMP/zig.${ZEXT}" "$URL"
        else
            wget --show-progress -O "$TMP/zig.${ZEXT}" "$URL"
        fi
        # Prefer system unzip; fall back to Python's zipfile so the
        # script doesn't require an extra apt install on minimal hosts.
        ( cd "$TMP" && \
          if [ "$ZEXT" = "zip" ]; then \
              if command -v unzip >/dev/null 2>&1; then \
                  unzip -q "zig.${ZEXT}"; \
              else \
                  python3 -c "import zipfile, sys; zipfile.ZipFile('zig.${ZEXT}').extractall('.')"; \
              fi; \
          else \
              tar -xf "zig.${ZEXT}"; \
          fi )
        mkdir -p dist/staging
        mv "$TMP/$ZNAME" "$TARGET_ZIG_DIR"
        rm -rf "$TMP"
    fi
fi

# ---- Cross-build glide for target if needed ----
TARGET_GLIDE="$STAGE/glide${EXE_EXT}"
mkdir -p "$STAGE"

if [ "$TARGET_OS" = "$HOST_OS" ] && [ "$TARGET_ARCH" = "$HOST_ARCH" ]; then
    echo ">> Bundling host glide"
    cp "$HOST_GLIDE" "$TARGET_GLIDE"
else
    echo ">> Cross-building glide for $ZIG_TRIPLE"
    "./$HOST_GLIDE" build bootstrap/main.glide --target="$ZIG_TRIPLE" -o "$TARGET_GLIDE"
fi

# ---- Stage src/ + Zig + docs ----
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
mkdir -p "$STAGE/runtime"
if [ -n "$TARGET_ZIG_DIR" ]; then
    cp -r "$TARGET_ZIG_DIR" "$STAGE/runtime/zig"
else
    cp -r runtime/zig "$STAGE/runtime/zig"
fi
cp README.md "$STAGE/" 2>/dev/null || true
cp LICENSE "$STAGE/" 2>/dev/null || true

# ---- Stage bundled static libs (optional) ----
# When `--bundle-libs=<dir>` was given, copy the .a files into `lib/`
# so bootstrap/main.glide picks them up via `_host_lib_bundle_dir`.
# Required set: libssl libcrypto libz libngtcp2 libngtcp2_crypto_ossl
# libnghttp3. Missing entries are tolerated — the linker falls back to
# system dynamic libs for whatever isn't bundled (so the install still
# builds non-h3 code on hosts without ngtcp2).
if [ -n "$BUNDLE_LIBS" ] && [ -d "$BUNDLE_LIBS" ]; then
    echo ">> Bundling static libs from $BUNDLE_LIBS"
    mkdir -p "$STAGE/lib"
    for lib in libssl.a libcrypto.a libz.a libngtcp2.a libngtcp2_crypto_ossl.a libnghttp3.a; do
        if [ -f "$BUNDLE_LIBS/$lib" ]; then
            cp "$BUNDLE_LIBS/$lib" "$STAGE/lib/"
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
exec_files = {f"{name}/glide", f"{name}/runtime/zig/zig"}
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

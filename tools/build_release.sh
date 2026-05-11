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

VERSION="${VERSION:-0.1.0}"
ZIG_VERSION="${ZIG_VERSION:-0.14.1}"
TARGET=""

while [ $# -gt 0 ]; do
    case "$1" in
        --target=*) TARGET="${1#--target=}"; shift ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
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

# Map our short OS names to Zig's triplet vocabulary.
case "$TARGET_OS" in
    windows) ZIG_TRIPLE="${TARGET_ARCH}-windows-gnu";  EXE_EXT=".exe" ;;
    linux)   ZIG_TRIPLE="${TARGET_ARCH}-linux-gnu";    EXE_EXT="" ;;
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
rm -rf "$STAGE/src" "$STAGE/runtime"
cp -r src "$STAGE/"
mkdir -p "$STAGE/runtime"
if [ -n "$TARGET_ZIG_DIR" ]; then
    cp -r "$TARGET_ZIG_DIR" "$STAGE/runtime/zig"
else
    cp -r runtime/zig "$STAGE/runtime/zig"
fi
cp README.md "$STAGE/" 2>/dev/null || true
cp LICENSE "$STAGE/" 2>/dev/null || true

# ---- Archive ----
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
        ( cd dist && tar -czf "${NAME}.tar.gz" "$NAME" )
        ARCHIVE="dist/${NAME}.tar.gz"
        ;;
esac

SIZE=$(du -sh "$ARCHIVE" | cut -f1)
echo ">> Done: $ARCHIVE ($SIZE)"

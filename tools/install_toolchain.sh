#!/usr/bin/env bash
# Download and unpack a Zig release into runtime/zig/. Used both during
# Glide development (so `glide build` finds a bundled Zig) and during
# release packaging.
#
#   ZIG_VERSION  override the Zig version (default below)
#   RUNTIME_DIR  override the install dir (default: runtime/zig)

set -e

ZIG_VERSION="${ZIG_VERSION:-0.14.1}"
RUNTIME_DIR="${RUNTIME_DIR:-runtime/zig}"

case "$(uname -s)" in
    Linux*)              OS=linux ;;
    Darwin*)             OS=macos ;;
    CYGWIN*|MINGW*|MSYS*) OS=windows ;;
    *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac

case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

if [ "$OS" = "windows" ]; then
    EXT="zip"
else
    EXT="tar.xz"
fi

NAME="zig-${ARCH}-${OS}-${ZIG_VERSION}"
URL="https://ziglang.org/download/${ZIG_VERSION}/${NAME}.${EXT}"

echo ">> Downloading $URL"
DL_DIR="$(mktemp -d)"
trap 'rm -rf "$DL_DIR"' EXIT

if command -v curl >/dev/null 2>&1; then
    curl -fL --progress-bar -o "$DL_DIR/zig.${EXT}" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget --show-progress -O "$DL_DIR/zig.${EXT}" "$URL"
else
    echo "neither curl nor wget is available" >&2
    exit 1
fi

echo ">> Extracting"
( cd "$DL_DIR" && \
  if [ "$EXT" = "zip" ]; then unzip -q "zig.${EXT}"; else tar -xf "zig.${EXT}"; fi )

rm -rf "$RUNTIME_DIR"
mkdir -p "$(dirname "$RUNTIME_DIR")"
mv "$DL_DIR/$NAME" "$RUNTIME_DIR"

echo ">> Installed at $RUNTIME_DIR"
"$RUNTIME_DIR/zig" version

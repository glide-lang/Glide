#!/usr/bin/env bash
# Glide installer for Linux and macOS.
#
# Usage:
#   # local archive
#   tools/install.sh --archive ./dist/glide-linux-x86_64-0.1.0.tar.gz
#
#   # remote (after release is published)
#   curl -sSf https://github.com/.../releases/download/.../install.sh | bash
#
# Installs to ~/.local/share/glide and writes a wrapper to ~/.local/bin/glide
# (no sudo needed). Asks the user to add ~/.local/bin to PATH if missing.

set -e

VERSION="${VERSION:-0.7.0}"
ARCHIVE=""
INSTALL_DIR="${INSTALL_DIR:-$HOME/.local/share/glide}"
BIN_DIR="${BIN_DIR:-$HOME/.local/bin}"
DOWNLOAD_URL_BASE="${DOWNLOAD_URL_BASE:-https://github.com/glide-lang/Glide/releases/download/v${VERSION}}"

while [ $# -gt 0 ]; do
    case "$1" in
        --archive) ARCHIVE="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --install-dir) INSTALL_DIR="$2"; shift 2 ;;
        --bin-dir) BIN_DIR="$2"; shift 2 ;;
        --url-base) DOWNLOAD_URL_BASE="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

case "$(uname -s)" in
    Linux*)  OS=linux ;;
    Darwin*) OS=macos ;;
    *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

NAME="glide-${OS}-${ARCH}-${VERSION}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -n "$ARCHIVE" ] && [ -f "$ARCHIVE" ]; then
    echo ">> Using local archive: $ARCHIVE"
    cp "$ARCHIVE" "$TMP/glide.tar.gz"
else
    URL="${DOWNLOAD_URL_BASE}/${NAME}.tar.gz"
    echo ">> Downloading $URL"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --progress-bar -o "$TMP/glide.tar.gz" "$URL"
    else
        wget --show-progress -O "$TMP/glide.tar.gz" "$URL"
    fi
fi

echo ">> Extracting"
tar -xzf "$TMP/glide.tar.gz" -C "$TMP"

rm -rf "$INSTALL_DIR"
mkdir -p "$(dirname "$INSTALL_DIR")"
mv "$TMP/$NAME" "$INSTALL_DIR"
echo ">> Installed to $INSTALL_DIR"

# Defensive: ensure the binaries are executable even if the tar.gz
# was built on a host that didn't preserve the +x bit (MSYS / MinGW
# bash on Windows is the usual offender).
chmod +x "$INSTALL_DIR/glide" 2>/dev/null || true

# Wrapper in ~/.local/bin so PATH stays clean of one entry per tool.
mkdir -p "$BIN_DIR"
cat > "$BIN_DIR/glide" <<EOF
#!/bin/sh
exec "$INSTALL_DIR/glide" "\$@"
EOF
chmod +x "$BIN_DIR/glide"
echo ">> Wrapper at $BIN_DIR/glide"

# Tell the user to add to PATH if needed.
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) echo ""
       echo "NOTE: $BIN_DIR is not on your PATH. Add to your shell rc:"
       echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
       ;;
esac

echo ""
echo "Done. Try: glide --help"

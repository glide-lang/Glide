#!/usr/bin/env bash
# Produce the windows-x86_64 bundle from inside msys2 (ucrt64 shell).
#
# msys2 ucrt64 ships static .a builds of openssl, zlib, ngtcp2,
# nghttp3, ngtcp2_crypto_ossl via pacman packages. We just need to
# pull them out of /ucrt64/lib/.
#
# Run as:  OUT=/path/to/out tools/bundle/windows-x86_64-mingw/build.sh

set -euo pipefail

OUT="${OUT:-./out}"
mkdir -p "$OUT"

# Confirm we're in the right shell — pacman + the right prefix.
if [ ! -x /usr/bin/pacman ]; then
    echo "pacman not found — run this from msys2 mingw/ucrt shell" >&2
    exit 1
fi

# Required packages. Pinned by major series; pacman picks the latest
# patch automatically when running pacman -S, so this list also doubles
# as a setup checklist.
REQ_PKGS="\
    mingw-w64-ucrt-x86_64-openssl
    mingw-w64-ucrt-x86_64-zlib
    mingw-w64-ucrt-x86_64-ngtcp2
    mingw-w64-ucrt-x86_64-nghttp3
"

for pkg in $REQ_PKGS; do
    if ! pacman -Qi "$pkg" >/dev/null 2>&1; then
        echo "missing package: $pkg" >&2
        echo "install with:  pacman -S --needed $pkg" >&2
        exit 1
    fi
done

# ngtcp2_crypto_ossl ships inside the ngtcp2 package on msys2.
LIBS="\
    libssl.a
    libcrypto.a
    libz.a
    libngtcp2.a
    libnghttp3.a
    libngtcp2_crypto_ossl.a
"

for lib in $LIBS; do
    src="/ucrt64/lib/$lib"
    if [ ! -f "$src" ]; then
        echo "missing $src — package may have changed layout" >&2
        exit 1
    fi
    cp "$src" "$OUT/$lib"
done

echo ">> Bundle contents:"
ls -la "$OUT"

#!/usr/bin/env bash
# Produce the macOS bundle (x86_64 or arm64) on an actual Mac.
#
# Strategy: pull openssl + zlib static libs out of brew (they ship in
# the openssl@3 and zlib formulas as .a files), then build ngtcp2 +
# nghttp3 + ngtcp2_crypto_ossl from source via Apple Clang.
#
# Brew dependencies (install up front):
#   brew install openssl@3 zlib autoconf automake libtool pkg-config
#
# Run as:  OUT=/path/to/out tools/bundle/macos/build.sh macos-aarch64

set -euo pipefail

TARGET="${1:-macos-aarch64}"
OUT="${OUT:-./out}"
NGHTTP3_TAG="${NGHTTP3_TAG:-v1.5.0}"
NGTCP2_TAG="${NGTCP2_TAG:-v1.7.0}"

case "$TARGET" in
    macos-x86_64)  ARCH="x86_64"  ;;
    macos-aarch64) ARCH="arm64"   ;;
    *) echo "unsupported target: $TARGET" >&2; exit 1 ;;
esac

if [ "$(uname -s)" != "Darwin" ]; then
    echo "this script must run on macOS" >&2; exit 1
fi

mkdir -p "$OUT"

# Locate brew prefixes for openssl@3 and zlib. On arm64 Macs brew lives
# under /opt/homebrew; on x86_64 it's /usr/local. `brew --prefix` does
# the right thing on either.
OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || echo "")"
ZLIB_PREFIX="$(brew --prefix zlib 2>/dev/null || echo "")"
if [ -z "$OPENSSL_PREFIX" ] || [ ! -f "$OPENSSL_PREFIX/lib/libssl.a" ]; then
    echo "openssl@3 not installed or static libs missing — brew install openssl@3" >&2
    exit 1
fi
if [ -z "$ZLIB_PREFIX" ] || [ ! -f "$ZLIB_PREFIX/lib/libz.a" ]; then
    echo "zlib not installed or static libs missing — brew install zlib" >&2
    exit 1
fi

cp "$OPENSSL_PREFIX/lib/libssl.a"    "$OUT/"
cp "$OPENSSL_PREFIX/lib/libcrypto.a" "$OUT/"
cp "$ZLIB_PREFIX/lib/libz.a"         "$OUT/"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---- nghttp3 ----
cd "$WORK"
git clone --depth 1 --branch "$NGHTTP3_TAG" \
    https://github.com/ngtcp2/nghttp3.git
cd nghttp3
git submodule update --init --depth 1
autoreconf -i
./configure \
    --enable-static --disable-shared --enable-lib-only \
    CFLAGS="-arch $ARCH"
make -j"$(sysctl -n hw.ncpu)"
cp lib/.libs/libnghttp3.a "$OUT/"

# ---- ngtcp2 + ngtcp2_crypto_ossl ----
cd "$WORK"
git clone --depth 1 --branch "$NGTCP2_TAG" \
    https://github.com/ngtcp2/ngtcp2.git
cd ngtcp2
git submodule update --init --depth 1
autoreconf -i
PKG_CONFIG_PATH="$OPENSSL_PREFIX/lib/pkgconfig" \
./configure \
    --enable-static --disable-shared --enable-lib-only \
    --with-openssl \
    CFLAGS="-arch $ARCH"
make -j"$(sysctl -n hw.ncpu)"
cp lib/.libs/libngtcp2.a "$OUT/"
cp crypto/ossl/.libs/libngtcp2_crypto_ossl.a "$OUT/"

echo ">> Bundle contents:"
ls -la "$OUT"

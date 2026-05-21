#!/bin/sh
# Built into the bundle Docker image; runs inside the container with /out
# bind-mounted to the host's dist/bundle-linux-x86_64-musl/ directory.
#
# Pinned upstream tags. Bump together with a verification run so the
# resulting .a files actually link cleanly against bootstrap/main.glide.

set -eu

NGHTTP3_TAG="${NGHTTP3_TAG:-v1.5.0}"
NGTCP2_TAG="${NGTCP2_TAG:-v1.7.0}"

mkdir -p /out

# ---- nghttp3 ----
# Standalone library; no external deps. --enable-lib-only skips the
# example apps that would otherwise pull libev / ngtcp2.
cd /build
git clone --depth 1 --branch "$NGHTTP3_TAG" \
    https://github.com/ngtcp2/nghttp3.git
cd nghttp3
git submodule update --init --depth 1
autoreconf -i
./configure --enable-static --disable-shared --enable-lib-only
make -j"$(nproc)"
cp lib/.libs/libnghttp3.a /out/

# ---- ngtcp2 + ngtcp2_crypto_ossl ----
# --with-openssl makes the build emit crypto/ossl/libngtcp2_crypto_ossl.a
# linked against the system openssl-libs-static.
cd /build
git clone --depth 1 --branch "$NGTCP2_TAG" \
    https://github.com/ngtcp2/ngtcp2.git
cd ngtcp2
git submodule update --init --depth 1
autoreconf -i
./configure \
    --enable-static \
    --disable-shared \
    --enable-lib-only \
    --with-openssl
make -j"$(nproc)"
cp lib/.libs/libngtcp2.a /out/
cp crypto/ossl/.libs/libngtcp2_crypto_ossl.a /out/

# ---- openssl + zlib (already built static by Alpine) ----
cp /usr/lib/libssl.a    /out/
cp /usr/lib/libcrypto.a /out/
cp /usr/lib/libz.a      /out/

echo ">> Bundle contents:"
ls -la /out/

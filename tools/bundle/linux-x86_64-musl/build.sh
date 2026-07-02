#!/bin/sh
# Built into the bundle Docker image; runs inside the container with /out
# bind-mounted to the host's dist/bundle-<target>/ directory.
#
# Pinned upstream versions + sha256s. Bump together with a verification
# run so the resulting .a files actually link cleanly against
# bootstrap/main.glide (and the h3 test passes against the sysroot).

set -eu

OPENSSL_VER="${OPENSSL_VER:-3.5.7}"
NGHTTP3_VER="${NGHTTP3_VER:-1.17.0}"
NGTCP2_VER="${NGTCP2_VER:-1.24.0}"

OPENSSL_SHA256="${OPENSSL_SHA256:-}"
NGHTTP3_SHA256="${NGHTTP3_SHA256:-}"
NGTCP2_SHA256="${NGTCP2_SHA256:-}"

PREFIX=/opt/stack
mkdir -p /out/lib /out/include "$PREFIX"

fetch() {
    # fetch <url> <dest> <sha256-or-empty>
    curl -fsSL "$1" -o "$2"
    if [ -n "$3" ]; then
        echo "$3  $2" | sha256sum -c - >/dev/null
    else
        echo ">> WARNING: no sha256 pinned for $2 — recording actual:"
        sha256sum "$2"
    fi
}

# ---- openssl (from source: ngtcp2 crypto_ossl needs >= 3.5) ----
cd /build
fetch "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz" \
      openssl.tar.gz "$OPENSSL_SHA256"
tar -xzf openssl.tar.gz
cd "openssl-${OPENSSL_VER}"
# ./config auto-detects the platform, so the same script serves x86_64
# and (via buildx) aarch64 containers. --libdir=lib avoids lib64.
./config no-shared no-tests no-apps no-docs --prefix="$PREFIX" --libdir=lib
make -j"$(nproc)"
make install_sw

# ---- nghttp3 (release tarball ships ./configure) ----
cd /build
fetch "https://github.com/ngtcp2/nghttp3/releases/download/v${NGHTTP3_VER}/nghttp3-${NGHTTP3_VER}.tar.gz" \
      nghttp3.tar.gz "$NGHTTP3_SHA256"
tar -xzf nghttp3.tar.gz
cd "nghttp3-${NGHTTP3_VER}"
./configure --enable-static --disable-shared --enable-lib-only --prefix="$PREFIX"
make -j"$(nproc)"
make install

# ---- ngtcp2 + ngtcp2_crypto_ossl (against the 3.5 openssl above) ----
cd /build
fetch "https://github.com/ngtcp2/ngtcp2/releases/download/v${NGTCP2_VER}/ngtcp2-${NGTCP2_VER}.tar.gz" \
      ngtcp2.tar.gz "$NGTCP2_SHA256"
tar -xzf ngtcp2.tar.gz
cd "ngtcp2-${NGTCP2_VER}"
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" ./configure \
    --enable-static \
    --disable-shared \
    --enable-lib-only \
    --with-openssl \
    --prefix="$PREFIX"
make -j"$(nproc)"
make install
[ -f "$PREFIX/lib/libngtcp2_crypto_ossl.a" ] || {
    echo "ngtcp2 built without crypto_ossl (openssl >= 3.5 not detected?)" >&2
    exit 1
}

# ---- stage the bundle: libs + headers ----
cp "$PREFIX"/lib/libssl.a "$PREFIX"/lib/libcrypto.a \
   "$PREFIX"/lib/libngtcp2.a "$PREFIX"/lib/libngtcp2_crypto_ossl.a \
   "$PREFIX"/lib/libnghttp3.a /out/lib/
cp -r "$PREFIX"/include/openssl "$PREFIX"/include/ngtcp2 \
      "$PREFIX"/include/nghttp3 /out/include/

# The container runs as root; hand the bind-mounted output back to the
# host user (whoever owns the mounted /out directory).
chown -R "$(stat -c '%u:%g' /out)" /out

echo ">> Bundle contents:"
ls -la /out/lib /out/include

#!/usr/bin/env bash
# Build OpenSSL 3.5+ (native QUIC) + ngtcp2 (crypto_ossl backend) + nghttp3
# for HTTP/3 server compilation. System OpenSSL on Debian/Ubuntu (3.0.x) is
# too old — it lacks the QUIC API (`SSL_set_quic_tls_cbs`, `ngtcp2_crypto_ossl_*`)
# that `src/stdlib/http/h3.glide` calls. The quictls fork (3.1.4+quic) ships
# different symbols (`ngtcp2_crypto_quictls_*`) and does NOT satisfy the
# `_ossl` ABI either — only stock OpenSSL 3.5+ does.
#
# Total time: ~25-40 min on a 4-core box (OpenSSL is the big one). Idempotent.
# After it finishes, h3 binaries link cleanly with:
#   PKG_CONFIG_PATH=/opt/openssl35/lib/pkgconfig:/usr/local/lib/pkgconfig \
#       cc ... -L/usr/local/lib -lngtcp2 -lngtcp2_crypto_ossl -lnghttp3 \
#               -L/opt/openssl35/lib -lssl -lcrypto
# Glide's bootstrap/main.glide auto-detects the libs at build time.
set -eu

OPENSSL_PREFIX=${OPENSSL_PREFIX:-/opt/openssl35}
NGTCP2_PREFIX=${NGTCP2_PREFIX:-/usr/local}
OPENSSL_VER=${OPENSSL_VER:-3.5.0}

if [ ! -f "$OPENSSL_PREFIX/lib/libssl.so" ]; then
    cd /tmp
    if [ ! -d "openssl-$OPENSSL_VER" ]; then
        wget -q "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz" \
            -O "openssl-$OPENSSL_VER.tar.gz"
        tar xf "openssl-$OPENSSL_VER.tar.gz"
    fi
    cd "openssl-$OPENSSL_VER"
    ./Configure linux-x86_64 --prefix="$OPENSSL_PREFIX" --libdir=lib shared
    make -j$(nproc)
    make install_sw
fi

if [ ! -f "$NGTCP2_PREFIX/include/ngtcp2/ngtcp2_crypto_ossl.h" ] \
   || [ ! -f "$NGTCP2_PREFIX/lib/libngtcp2_crypto_ossl.a" ]; then
    cd /tmp
    if [ ! -d ngtcp2 ]; then
        git clone --depth 1 --recursive https://github.com/ngtcp2/ngtcp2.git
    fi
    cd ngtcp2
    make distclean 2>/dev/null || true
    autoreconf -i
    PKG_CONFIG_PATH="$OPENSSL_PREFIX/lib/pkgconfig" \
        ./configure --with-openssl --prefix="$NGTCP2_PREFIX" \
            --disable-shared --enable-static
    make -j$(nproc)
    make install
fi

if ! pkg-config --exists libnghttp3 || [ ! -f "$NGTCP2_PREFIX/lib/libnghttp3.a" ]; then
    cd /tmp
    if [ ! -d nghttp3 ]; then
        git clone --depth 1 --recursive https://github.com/ngtcp2/nghttp3.git
    fi
    cd nghttp3
    autoreconf -i
    ./configure --prefix="$NGTCP2_PREFIX" --disable-shared --enable-static \
        --enable-lib-only
    make -j$(nproc)
    make install
fi

echo "HTTP/3 toolchain ready under $OPENSSL_PREFIX + $NGTCP2_PREFIX"
echo "Add to env before glide build:"
echo "  export LD_LIBRARY_PATH=$OPENSSL_PREFIX/lib:\$LD_LIBRARY_PATH"
echo "  export PKG_CONFIG_PATH=$OPENSSL_PREFIX/lib/pkgconfig:$NGTCP2_PREFIX/lib/pkgconfig"

#!/usr/bin/env bash
# Build quictls (OpenSSL + QUIC) + ngtcp2 + nghttp3 + ngtcp2_crypto_ossl
# for HTTP/3 server compilation. The Ubuntu/Debian apt-shipped ngtcp2
# only carries the gnutls crypto backend; Glide's h3.glide includes
# `<ngtcp2/ngtcp2_crypto_ossl.h>` which requires the OpenSSL build.
#
# Total time: ~10-20 min on a 4-core box. Idempotent — skips already-built
# stages. After it finishes, h3 binaries link cleanly with:
#   PKG_CONFIG_PATH=/opt/quictls/lib/pkgconfig:/usr/local/lib/pkgconfig \
#       cc ... -L/usr/local/lib -lngtcp2 -lngtcp2_crypto_ossl -lnghttp3 \
#               -L/opt/quictls/lib -lssl -lcrypto
# Glide's bootstrap/main.glide auto-detects the libs at build time.
set -eu

INSTALL_PREFIX=${INSTALL_PREFIX:-/opt/quictls}
NGTCP2_PREFIX=${NGTCP2_PREFIX:-/usr/local}

if [ ! -f "$INSTALL_PREFIX/lib/libssl.so" ]; then
    cd /tmp
    if [ ! -d quictls ]; then
        git clone --depth 1 -b openssl-3.1.4+quic \
            https://github.com/quictls/openssl.git quictls
    fi
    cd quictls
    ./Configure linux-x86_64 --prefix="$INSTALL_PREFIX" --libdir=lib -shared
    make -j$(nproc)
    make install_sw
fi

if [ ! -f "$NGTCP2_PREFIX/include/ngtcp2/ngtcp2_crypto_ossl.h" ]; then
    cd /tmp
    if [ ! -d ngtcp2 ]; then
        git clone --depth 1 --recursive https://github.com/ngtcp2/ngtcp2.git
    fi
    cd ngtcp2
    autoreconf -i
    PKG_CONFIG_PATH="$INSTALL_PREFIX/lib/pkgconfig" \
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
    ./configure --prefix="$NGTCP2_PREFIX" --disable-shared --enable-static
    make -j$(nproc)
    make install
fi

echo "HTTP/3 toolchain ready under $INSTALL_PREFIX + $NGTCP2_PREFIX"
echo "Add to env before glide build:"
echo "  export LD_LIBRARY_PATH=$INSTALL_PREFIX/lib:\$LD_LIBRARY_PATH"
echo "  export PKG_CONFIG_PATH=$INSTALL_PREFIX/lib/pkgconfig:$NGTCP2_PREFIX/lib/pkgconfig"

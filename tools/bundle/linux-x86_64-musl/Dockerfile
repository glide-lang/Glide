# Builds the static lib bundle for x86_64-linux-musl.
#
# Output (mounted at /out):
#   libssl.a, libcrypto.a, libz.a, libngtcp2.a, libnghttp3.a,
#   libngtcp2_crypto_ossl.a
#
# Alpine 3.20 ships openssl 3.3 + zlib 1.3 static, but ngtcp2/nghttp3
# aren't packaged yet. We build them from source against the system
# openssl-libs-static so the whole bundle is consistent.
#
# Pinned versions in build.sh; bump there.

FROM alpine:3.20

RUN apk add --no-cache \
        build-base \
        git \
        autoconf \
        automake \
        libtool \
        pkgconf \
        cmake \
        openssl-dev \
        openssl-libs-static \
        zlib-dev \
        zlib-static \
        linux-headers

WORKDIR /build
COPY build.sh /build.sh
ENTRYPOINT ["/bin/sh", "/build.sh"]

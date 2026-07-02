# Builds the static lib set for linux-musl (x86_64 natively; aarch64 via
# `docker buildx --platform linux/arm64` — the scripts are arch-agnostic).
#
# Output (mounted at /out):
#   lib/libssl.a, lib/libcrypto.a, lib/libz.a,
#   lib/libngtcp2.a, lib/libnghttp3.a, lib/libngtcp2_crypto_ossl.a
#   include/{openssl,ngtcp2,nghttp3}/..., include/{zlib.h,zconf.h}
#
# openssl is built FROM SOURCE: ngtcp2's ossl crypto backend needs the
# QUIC TLS API from openssl >= 3.5, newer than any Alpine package.
# ngtcp2/nghttp3 come from release tarballs (they ship ./configure — no
# git/autoreconf needed). zlib comes prebuilt from Alpine.
#
# Pinned versions in build.sh; bump there. Base image pinned by digest
# (alpine:3.20) — the sysroot's libc/ tree comes from the same Alpine
# release, so the digest is part of the supply chain.

FROM alpine@sha256:c64c687cbea9300178b30c95835354e34c4e4febc4badfe27102879de0483b5e

RUN apk add --no-cache \
        build-base \
        perl \
        curl \
        pkgconf \
        zlib-dev \
        zlib-static \
        linux-headers

WORKDIR /build
COPY build.sh /build.sh
ENTRYPOINT ["/bin/sh", "/build.sh"]

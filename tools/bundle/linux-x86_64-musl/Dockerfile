# Builds the static lib set for linux-musl (x86_64 natively; aarch64 via
# `docker buildx --platform linux/arm64` — the scripts are arch-agnostic).
#
# Output (mounted at /out):
#   lib/libssl.a, lib/libcrypto.a,
#   lib/libngtcp2.a, lib/libnghttp3.a, lib/libngtcp2_crypto_ossl.a
#   include/{openssl,ngtcp2,nghttp3}/...
#
# openssl is built FROM SOURCE: ngtcp2's ossl crypto backend needs the
# QUIC TLS API from openssl >= 3.5, newer than any Alpine package.
# ngtcp2/nghttp3 come from release tarballs (they ship ./configure — no
# git/autoreconf needed). No zlib: stdlib::compress is pure Glide.
#
# Pinned versions in build.sh; bump there. Base image pinned by digest
# (alpine:3.20) — the sysroot's libc/ tree comes from the same Alpine
# release, so the digest is part of the supply chain.

# NB: this must be the multi-arch INDEX digest (docker pull reports it),
# not a per-arch manifest digest — the aarch64 sysroot builds this same
# Dockerfile on an ARM runner, and an amd64-only pin hands it x86 layers
# (apk then dies with an exec-format 255).
FROM alpine@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc

RUN apk add --no-cache \
        build-base \
        perl \
        curl \
        pkgconf \
        linux-headers

WORKDIR /build
COPY build.sh /build.sh
ENTRYPOINT ["/bin/sh", "/build.sh"]

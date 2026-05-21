#!/usr/bin/env bash
# Produce the static-lib bundle for a given target. The bundle drops
# into dist/bundle-<target>/ and is consumed by tools/build_release.sh
# via --bundle-libs=<dir>:
#
#   tools/bundle/build.sh linux-x86_64-musl
#   tools/build_release.sh --target=x86_64-linux \
#       --bundle-libs=dist/bundle-linux-x86_64-musl
#
# Per-target strategy:
#   linux-x86_64-musl / aarch64-musl: Docker + Alpine base. Builds
#       ngtcp2 / nghttp3 from source against openssl-libs-static.
#   macos-x86_64 / aarch64: native build via tools/bundle/macos/build.sh
#       (Apple Clang + brew openssl statics). Must run on a Mac.
#   windows-x86_64-mingw: native build via
#       tools/bundle/windows-x86_64-mingw/build.ps1 (msys2 ucrt64
#       toolchain + pacman packages). Must run on Windows with msys2.
#
# Linux targets are easy to produce from any host with Docker; the
# other two require a matching native environment (or a GitHub Actions
# job with that runner).

set -euo pipefail

TARGET="${1:-}"
if [ -z "$TARGET" ]; then
    echo "usage: $0 <target>" >&2
    echo "targets:" >&2
    echo "  linux-x86_64-musl" >&2
    echo "  linux-aarch64-musl" >&2
    echo "  macos-x86_64" >&2
    echo "  macos-aarch64" >&2
    echo "  windows-x86_64-mingw" >&2
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO_ROOT/dist/bundle-$TARGET"
mkdir -p "$OUT"

case "$TARGET" in
    linux-x86_64-musl)
        IMG="glide-bundle-linux-x86_64-musl"
        docker build -t "$IMG" "$REPO_ROOT/tools/bundle/linux-x86_64-musl"
        docker run --rm -v "$OUT:/out" "$IMG"
        ;;
    linux-aarch64-musl)
        IMG="glide-bundle-linux-aarch64-musl"
        # Cross-build via buildx + qemu emulation. Slow but works on
        # any x86_64 host with Docker.
        docker buildx build \
            --platform linux/arm64 \
            -t "$IMG" \
            --load \
            "$REPO_ROOT/tools/bundle/linux-x86_64-musl"
        docker run --rm --platform linux/arm64 -v "$OUT:/out" "$IMG"
        ;;
    macos-x86_64|macos-aarch64)
        if [ "$(uname -s)" != "Darwin" ]; then
            echo "macos bundle must be built on a Mac" >&2
            exit 1
        fi
        OUT="$OUT" "$REPO_ROOT/tools/bundle/macos/build.sh" "$TARGET"
        ;;
    windows-x86_64-mingw)
        if [ "$(uname -s | head -c 5)" != "MINGW" ] \
           && [ "$(uname -s | head -c 4)" != "MSYS" ]; then
            echo "windows bundle must be built under msys2 (mingw / msys shell)" >&2
            exit 1
        fi
        OUT="$OUT" "$REPO_ROOT/tools/bundle/windows-x86_64-mingw/build.sh"
        ;;
    *)
        echo "unsupported target: $TARGET" >&2
        exit 1
        ;;
esac

echo ">> Bundle ready at: $OUT"
ls -la "$OUT"

#!/usr/bin/env bash
# Build a cross-compile sysroot tarball for a given target.
#
# Sysroots package the headers + static libs the bundled Zig toolchain
# doesn't ship (openssl, zlib). Users opt in via `glide target add`,
# which downloads the matching tarball off the Glide release page and
# stages it under ~/.glide/targets/<triple>/{include,lib}/.
#
# Usage:
#   tools/build_sysroot.sh                              # x86_64-linux-musl, current VERSION
#   tools/build_sysroot.sh --target=x86_64-linux-musl   # explicit
#
# Output: dist/glide-sysroot-<triple>-<VERSION>.tar.gz
#
# Source packages: Alpine Linux APKs (apk format is a tar.gz with extra
# metadata; we only care about the file payload). musl is used as the
# libc so the produced cross binaries are fully self-contained.

set -e

VERSION="${VERSION:-0.1.1}"
TARGET="x86_64-linux-musl"
ALPINE_VER="${ALPINE_VER:-v3.20}"

# Pinned package versions. Bump when Alpine moves them.
OPENSSL_PKG="openssl-3.3.7-r0"
ZLIB_PKG="zlib-1.3.2-r0"

while [ $# -gt 0 ]; do
    case "$1" in
        --target=*) TARGET="${1#--target=}"; shift ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

case "$TARGET" in
    x86_64-linux-musl)
        ARCH="x86_64"
        ;;
    aarch64-linux-musl)
        ARCH="aarch64"
        ;;
    *)
        echo "unsupported target: $TARGET" >&2
        echo "(supported: x86_64-linux-musl, aarch64-linux-musl)" >&2
        exit 1
        ;;
esac

BASE="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_VER}/main/${ARCH}"
STAGING="dist/staging/sysroot-${TARGET}"
SYSROOT="dist/sysroot-${TARGET}"
OUT="dist/glide-sysroot-${TARGET}-${VERSION}.tar.gz"

rm -rf "$STAGING" "$SYSROOT" "$OUT"
mkdir -p "$STAGING" "$SYSROOT/include" "$SYSROOT/lib"

echo ">> Fetching Alpine packages from $BASE"
for pkg in \
    "${OPENSSL_PKG//openssl-/openssl-dev-}" \
    "${OPENSSL_PKG//openssl-/openssl-libs-static-}" \
    "${ZLIB_PKG//zlib-/zlib-dev-}" \
    "${ZLIB_PKG//zlib-/zlib-static-}"
do
    file="${pkg}.apk"
    echo "   $file"
    if ! curl -fsSL "${BASE}/${file}" -o "${STAGING}/${file}"; then
        echo "failed to fetch ${BASE}/${file}" >&2
        exit 1
    fi
    # APK = tar.gz with sometimes-malformed metadata stream; ignore exit.
    tar -xzf "${STAGING}/${file}" -C "${STAGING}" 2>/dev/null || true
done

echo ">> Assembling sysroot"
# Headers: usr/include/openssl + usr/include/zlib.h
[ -d "${STAGING}/usr/include/openssl" ] || { echo "openssl headers missing" >&2; exit 1; }
[ -f "${STAGING}/usr/include/zlib.h" ]  || { echo "zlib.h missing" >&2; exit 1; }
cp -r "${STAGING}/usr/include/openssl" "${SYSROOT}/include/"
cp "${STAGING}/usr/include/zlib.h" "${SYSROOT}/include/"
# zlib.h #includes zconf.h — without the sibling header zlib.h is
# unusable.
[ -f "${STAGING}/usr/include/zconf.h" ] || { echo "zconf.h missing" >&2; exit 1; }
cp "${STAGING}/usr/include/zconf.h" "${SYSROOT}/include/"

# Static libs: usr/lib/libssl.a + libcrypto.a + lib/libz.a
[ -f "${STAGING}/usr/lib/libssl.a" ]    || { echo "libssl.a missing" >&2; exit 1; }
[ -f "${STAGING}/usr/lib/libcrypto.a" ] || { echo "libcrypto.a missing" >&2; exit 1; }
[ -f "${STAGING}/lib/libz.a" ]          || { echo "libz.a missing" >&2; exit 1; }
cp "${STAGING}/usr/lib/libssl.a" "${SYSROOT}/lib/"
cp "${STAGING}/usr/lib/libcrypto.a" "${SYSROOT}/lib/"
cp "${STAGING}/lib/libz.a" "${SYSROOT}/lib/"

echo ">> Archiving"
( cd "$SYSROOT" && tar -czf "../glide-sysroot-${TARGET}-${VERSION}.tar.gz" include lib )

SIZE=$(du -sh "$OUT" | cut -f1)
echo ">> Done: $OUT ($SIZE)"
echo ""
echo "Upload alongside the main release archives:"
echo "  gh release upload v${VERSION} ${OUT}"
echo ""
echo "Users will pull it via:"
echo "  glide target add ${TARGET}"

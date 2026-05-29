#!/usr/bin/env bash
# Build a cross-compile sysroot tarball for a given target.
#
# Sysroots package the headers + static libs the bundled Zig toolchain
# doesn't ship (openssl, zlib). Users opt in via `glide target add`,
# which downloads the matching tarball off the Glide release page and
# stages it under ~/.glide/targets/<triple>/{include,lib}/.
#
# Usage:
#   tools/build_sysroot.sh                              # current host triple
#   tools/build_sysroot.sh --target=x86_64-linux-musl   # explicit
#
# Supported targets:
#   x86_64-linux-musl   — Alpine APKs (works on any Linux/Win/Mac host)
#   aarch64-linux-musl  — Alpine APKs
#   x86_64-windows-gnu  — MSYS2 mingw packages (host must have MSYS2)
#   aarch64-macos-none  — Homebrew (host must be macOS arm64)
#
# Output: dist/glide-sysroot-<triple>-<VERSION>.tar.gz

set -e

VERSION="${VERSION:-0.3.1}"
TARGET=""

while [ $# -gt 0 ]; do
    case "$1" in
        --target=*) TARGET="${1#--target=}"; shift ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

# Default to the running host's triple if --target= wasn't passed.
if [ -z "$TARGET" ]; then
    case "$(uname -s)" in
        Linux)   host_os="linux"; host_abi="musl" ;;
        Darwin)  host_os="macos"; host_abi="none" ;;
        MINGW*|MSYS*|CYGWIN*) host_os="windows"; host_abi="gnu" ;;
        *) echo "unsupported host: $(uname -s)" >&2; exit 1 ;;
    esac
    case "$(uname -m)" in
        x86_64|amd64)   host_arch="x86_64" ;;
        aarch64|arm64)  host_arch="aarch64" ;;
        *) echo "unsupported host arch: $(uname -m)" >&2; exit 1 ;;
    esac
    TARGET="${host_arch}-${host_os}-${host_abi}"
fi

OUT="dist/glide-sysroot-${TARGET}-${VERSION}.tar.gz"
STAGING="dist/staging/sysroot-${TARGET}"
SYSROOT="dist/sysroot-${TARGET}"
rm -rf "$STAGING" "$SYSROOT" "$OUT"
mkdir -p "$STAGING" "$SYSROOT/include" "$SYSROOT/lib"

# -------------------------------------------------------------------------
# Linux (musl): pull static libs from Alpine APKs. Works on any host
# because the APKs are plain tar.gz; the script doesn't need an Alpine
# environment to extract them.
# -------------------------------------------------------------------------
build_linux_musl() {
    local arch="$1"
    local alpine_ver="${ALPINE_VER:-v3.20}"
    local openssl_pkg="openssl-3.3.7-r0"
    local zlib_pkg="zlib-1.3.2-r0"
    local base="https://dl-cdn.alpinelinux.org/alpine/${alpine_ver}/main/${arch}"
    echo ">> Fetching Alpine packages from $base"
    for pkg in \
        "${openssl_pkg//openssl-/openssl-dev-}" \
        "${openssl_pkg//openssl-/openssl-libs-static-}" \
        "${zlib_pkg//zlib-/zlib-dev-}" \
        "${zlib_pkg//zlib-/zlib-static-}"
    do
        local file="${pkg}.apk"
        echo "   $file"
        if ! curl -fsSL "${base}/${file}" -o "${STAGING}/${file}"; then
            echo "failed to fetch ${base}/${file}" >&2
            exit 1
        fi
        # APK = tar.gz with sometimes-malformed metadata stream; ignore exit.
        tar -xzf "${STAGING}/${file}" -C "${STAGING}" 2>/dev/null || true
    done

    echo ">> Assembling sysroot"
    [ -d "${STAGING}/usr/include/openssl" ] || { echo "openssl headers missing" >&2; exit 1; }
    [ -f "${STAGING}/usr/include/zlib.h" ]  || { echo "zlib.h missing" >&2; exit 1; }
    cp -r "${STAGING}/usr/include/openssl" "${SYSROOT}/include/"
    cp "${STAGING}/usr/include/zlib.h" "${SYSROOT}/include/"
    [ -f "${STAGING}/usr/include/zconf.h" ] || { echo "zconf.h missing" >&2; exit 1; }
    cp "${STAGING}/usr/include/zconf.h" "${SYSROOT}/include/"

    [ -f "${STAGING}/usr/lib/libssl.a" ]    || { echo "libssl.a missing" >&2; exit 1; }
    [ -f "${STAGING}/usr/lib/libcrypto.a" ] || { echo "libcrypto.a missing" >&2; exit 1; }
    [ -f "${STAGING}/lib/libz.a" ]          || { echo "libz.a missing" >&2; exit 1; }
    cp "${STAGING}/usr/lib/libssl.a" "${SYSROOT}/lib/"
    cp "${STAGING}/usr/lib/libcrypto.a" "${SYSROOT}/lib/"
    cp "${STAGING}/lib/libz.a" "${SYSROOT}/lib/"
}

# -------------------------------------------------------------------------
# Windows (mingw-w64): grab headers + static libs from a local MSYS2
# install. Run from MSYS2's UCRT64 / MINGW64 shell on the Windows
# runner so /ucrt64 or /mingw64 actually exists.
# -------------------------------------------------------------------------
build_windows_gnu() {
    local arch="$1"
    if [ "$arch" != "x86_64" ]; then
        echo "windows-gnu: only x86_64 is supported (mingw-w64 ARM64 packages aren't standard)" >&2
        exit 1
    fi
    # Try UCRT64 first (modern, what the dev install uses), then MINGW64.
    local prefix=""
    for cand in /ucrt64 /mingw64 /c/msys64/ucrt64 /c/msys64/mingw64; do
        if [ -f "$cand/lib/libssl.a" ] && [ -d "$cand/include/openssl" ]; then
            prefix="$cand"; break
        fi
    done
    if [ -z "$prefix" ]; then
        echo "windows-gnu: no MSYS2 prefix found with libssl.a + openssl/." >&2
        echo "   install: pacman -S mingw-w64-ucrt-x86_64-{openssl,zlib}" >&2
        exit 1
    fi
    echo ">> Staging from MSYS2 prefix: $prefix"

    cp -r "$prefix/include/openssl" "${SYSROOT}/include/"
    cp "$prefix/include/zlib.h" "${SYSROOT}/include/"
    cp "$prefix/include/zconf.h" "${SYSROOT}/include/"

    cp "$prefix/lib/libssl.a" "${SYSROOT}/lib/"
    cp "$prefix/lib/libcrypto.a" "${SYSROOT}/lib/"
    cp "$prefix/lib/libz.a" "${SYSROOT}/lib/"

    # HTTP/3 stack — ship when MSYS2 has the static .a + headers. The
    # ngtcp2_crypto_ossl variant is the one stdlib::http::h3 links
    # against (the gnutls flavour exists in the same package set but
    # we never call it). Sysroot stays minimal when these are absent.
    if [ -d "$prefix/include/ngtcp2" ] && [ -f "$prefix/lib/libngtcp2.a" ]; then
        cp -r "$prefix/include/ngtcp2" "${SYSROOT}/include/"
        cp "$prefix/lib/libngtcp2.a" "${SYSROOT}/lib/"
        if [ -f "$prefix/lib/libngtcp2_crypto_ossl.a" ]; then
            cp "$prefix/lib/libngtcp2_crypto_ossl.a" "${SYSROOT}/lib/"
        fi
    fi
    if [ -d "$prefix/include/nghttp3" ] && [ -f "$prefix/lib/libnghttp3.a" ]; then
        cp -r "$prefix/include/nghttp3" "${SYSROOT}/include/"
        cp "$prefix/lib/libnghttp3.a" "${SYSROOT}/lib/"
    fi
}

# -------------------------------------------------------------------------
# macOS: take static libs from Homebrew's openssl@3 + zlib. Must run on
# a Mac of the matching arch (no cross-bottle for openssl static libs).
# -------------------------------------------------------------------------
build_macos() {
    local arch="$1"
    if [ "$arch" != "aarch64" ] && [ "$arch" != "arm64" ]; then
        echo "macos: only aarch64/arm64 is wired (Apple Silicon)" >&2
        exit 1
    fi
    if [ "$(uname -s)" != "Darwin" ]; then
        echo "macos sysroot must be built ON a Mac (uname -s = $(uname -s))" >&2
        exit 1
    fi

    local openssl_dir
    local zlib_dir
    openssl_dir="$(brew --prefix openssl@3 2>/dev/null || true)"
    zlib_dir="$(brew --prefix zlib 2>/dev/null || true)"

    if [ -z "$openssl_dir" ] || [ ! -d "$openssl_dir/include/openssl" ] || [ ! -f "$openssl_dir/lib/libssl.a" ]; then
        echo "macos: openssl@3 not installed via brew or static libs missing" >&2
        echo "   brew install openssl@3 zlib" >&2
        exit 1
    fi
    if [ -z "$zlib_dir" ] || [ ! -f "$zlib_dir/include/zlib.h" ] || [ ! -f "$zlib_dir/include/zconf.h" ] || [ ! -f "$zlib_dir/lib/libz.a" ]; then
        echo "macos: zlib not installed via brew or static libs missing" >&2
        echo "   brew install openssl@3 zlib" >&2
        exit 1
    fi
    echo ">> Staging from Homebrew: $openssl_dir + $zlib_dir"

    cp -r "$openssl_dir/include/openssl" "${SYSROOT}/include/"
    cp "$zlib_dir/include/zlib.h" "${SYSROOT}/include/"
    cp "$zlib_dir/include/zconf.h" "${SYSROOT}/include/"

    cp "$openssl_dir/lib/libssl.a" "${SYSROOT}/lib/"
    cp "$openssl_dir/lib/libcrypto.a" "${SYSROOT}/lib/"
    cp "$zlib_dir/lib/libz.a" "${SYSROOT}/lib/"
}

# -------------------------------------------------------------------------
# Dispatch.
# -------------------------------------------------------------------------
case "$TARGET" in
    x86_64-linux-musl)
        build_linux_musl x86_64 ;;
    aarch64-linux-musl)
        build_linux_musl aarch64 ;;
    x86_64-windows-gnu)
        build_windows_gnu x86_64 ;;
    aarch64-macos-none)
        build_macos aarch64 ;;
    *)
        echo "unsupported target: $TARGET" >&2
        echo "supported: x86_64-linux-musl, aarch64-linux-musl," >&2
        echo "           x86_64-windows-gnu, aarch64-macos-none" >&2
        exit 1
        ;;
esac

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

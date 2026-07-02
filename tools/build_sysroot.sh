#!/usr/bin/env bash
# Build a cross-compile sysroot tarball for a given target.
#
# Sysroot v2 layout:
#   include/ lib/    third-party static libs + headers (openssl, ngtcp2,
#                    nghttp3) — same role as v1, consumed by the
#                    -I/-L link assembly in bootstrap/main.glide.
#   libc/            the target's C library + startup files + libgcc, so
#                    a host clang can cross-compile with
#                    `--target=<triple> --sysroot=<sr>/libc -B<sr>/libc/gcc`.
#                    linux: musl tree (libc/usr/{include,lib}) from Alpine
#                    APKs; windows: mingw-w64 tree (libc/{include,lib})
#                    from MSYS2 packages. macOS has no libc/ — that target
#                    only builds natively on a Mac (Apple clang).
#
# Users opt in via `glide target add`, which downloads the tarball off
# the Glide release page into ~/.glide/targets/<triple>/.
#
# Usage:
#   tools/build_sysroot.sh                              # current host triple
#   tools/build_sysroot.sh --target=x86_64-linux-musl   # explicit
#
# Supported targets:
#   x86_64-linux-musl   — Docker (Alpine container builds the lib stack;
#   aarch64-linux-musl    APKs supply the musl libc tree). Any host.
#   x86_64-windows-gnu  — MSYS2 mingw packages (host must have MSYS2)
#   aarch64-macos-none  — Homebrew (host must be macOS arm64)
#
# Output: dist/glide-sysroot-<triple>-<VERSION>.tar.gz

set -e

VERSION="${VERSION:-0.3.1}"
TARGET=""

# Alpine release the linux libc/ tree comes from. Must match the digest
# pinned in tools/bundle/linux-x86_64-musl/Dockerfile — libc.a and the
# lib stack should be built by the same musl generation.
ALPINE_VER="${ALPINE_VER:-v3.20}"
MUSL_VER="1.2.5-r3"
GCC_APK_VER="13.2.1_git20240309-r1"
# sha256 per (package, arch) — APKs at a pinned version are immutable.
sha_musl_x86_64="70705bdeb1a8d54ee1ec7ce3b06f176206f4f3b105d86cf5576d74b8277adfa0"
sha_musl_dev_x86_64="36abcf8a199826080b9b2a45f86782afae4ae5c8b8331909e5113911e2bdcad1"
sha_gcc_x86_64="f3c6095905cbe31d77ac7e3ba3397058b952751d0dd6b0511e8baa464ef0fa99"
sha_musl_aarch64="e455c49c6c3de1dfcd4b9867c35097f588de2fb01a939c77ddb149f8e6086a24"
sha_musl_dev_aarch64="89a641400ddd298cf2b40a3b6d7d0a20278d5a01cae79fc639ca8fd27b2fdf12"
sha_gcc_aarch64="07ca04d225d3b8a36fb6f215650d2c2d4714d6b5e14bda568abd17176e6a3ad2"

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

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="dist/glide-sysroot-${TARGET}-${VERSION}.tar.gz"
STAGING="dist/staging/sysroot-${TARGET}"
SYSROOT="dist/sysroot-${TARGET}"
rm -rf "$STAGING" "$SYSROOT" "$OUT"
mkdir -p "$STAGING" "$SYSROOT/include" "$SYSROOT/lib"

_fetch_checked() {
    # _fetch_checked <url> <dest> <sha256>
    curl -fsSL "$1" -o "$2"
    echo "$3  $2" | sha256sum -c - >/dev/null || {
        echo "sha256 mismatch for $1" >&2
        exit 1
    }
}

# -------------------------------------------------------------------------
# Linux (musl): the lib stack (openssl 3.5 + ngtcp2/nghttp3, all
# static) is built inside the pinned Alpine container — Alpine gcc IS a
# native musl toolchain, no cross shims needed. The libc/ tree (musl
# headers + crt + libc.a + libgcc) comes from pinned Alpine APKs, which
# are plain tar.gz — no Alpine environment needed to extract them.
# -------------------------------------------------------------------------

_stage_linux_libc() {
    local arch="$1"
    local base="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_VER}/main/${arch}"
    local apkdir="${STAGING}/apk"
    local extract="${STAGING}/apk-extract"
    mkdir -p "$apkdir" "$extract"

    local sha_musl sha_musl_dev sha_gcc
    eval "sha_musl=\$sha_musl_${arch}"
    eval "sha_musl_dev=\$sha_musl_dev_${arch}"
    eval "sha_gcc=\$sha_gcc_${arch}"

    echo ">> Fetching Alpine ${ALPINE_VER}/${arch} libc packages"
    _fetch_checked "${base}/musl-${MUSL_VER}.apk"     "${apkdir}/musl.apk"     "$sha_musl"
    _fetch_checked "${base}/musl-dev-${MUSL_VER}.apk" "${apkdir}/musl-dev.apk" "$sha_musl_dev"
    _fetch_checked "${base}/gcc-${GCC_APK_VER}.apk"   "${apkdir}/gcc.apk"      "$sha_gcc"

    for a in "$apkdir"/*.apk; do
        # APK = tar.gz with sometimes-malformed metadata stream; ignore exit.
        tar -xzf "$a" -C "$extract" 2>/dev/null || true
    done

    echo ">> Staging libc/ tree (musl + crt + libgcc)"
    local lc="${SYSROOT}/libc"
    mkdir -p "$lc/usr/lib" "$lc/gcc"
    cp -r "$extract/usr/include" "$lc/usr/"
    # crt startfiles + libc + the empty ABI stubs (libm, libpthread, …).
    cp "$extract"/usr/lib/*.o "$extract"/usr/lib/lib*.a "$lc/usr/lib/"
    # libgcc + crtbegin/crtend into a fixed, version-free path so the
    # compiler flags stay deterministic: -B<sr>/libc/gcc -L<sr>/libc/gcc.
    local gd
    gd="$(ls -d "$extract"/usr/lib/gcc/*-alpine-linux-musl/*/ | head -1)"
    [ -n "$gd" ] || { echo "gcc APK layout unexpected — no gcc dir" >&2; exit 1; }
    cp "$gd"/crtbegin*.o "$gd"/crtend*.o "$gd"/libgcc.a "$gd"/libgcc_eh.a "$lc/gcc/"
}

build_linux_musl() {
    local arch="$1"
    command -v docker >/dev/null 2>&1 || {
        echo "linux-musl sysroots are built in a pinned Alpine container — docker is required." >&2
        exit 1
    }

    local img="glide-bundle-linux-musl-${arch}"
    local bundle_out="${STAGING}/bundle"
    mkdir -p "$bundle_out"
    echo ">> Building the static lib stack in Docker (${arch})"
    if [ "$arch" = "$(uname -m)" ] || { [ "$arch" = "x86_64" ] && [ "$(uname -m)" = "amd64" ]; }; then
        docker build -t "$img" "$REPO_ROOT/tools/bundle/linux-x86_64-musl"
        docker run --rm -v "$(pwd)/$bundle_out:/out" "$img"
    else
        # Cross-arch via buildx + qemu emulation. Slow but works on any
        # x86_64 host with Docker.
        local platform="linux/arm64"
        [ "$arch" = "x86_64" ] && platform="linux/amd64"
        docker buildx build --platform "$platform" -t "$img" --load \
            "$REPO_ROOT/tools/bundle/linux-x86_64-musl"
        docker run --rm --platform "$platform" -v "$(pwd)/$bundle_out:/out" "$img"
    fi

    echo ">> Assembling sysroot"
    cp -r "$bundle_out"/include/. "${SYSROOT}/include/"
    cp "$bundle_out"/lib/*.a "${SYSROOT}/lib/"

    _stage_linux_libc "$arch"
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
        echo "   install: pacman -S mingw-w64-ucrt-x86_64-openssl" >&2
        exit 1
    fi
    echo ">> Staging from MSYS2 prefix: $prefix"

    cp -r "$prefix/include/openssl" "${SYSROOT}/include/"

    cp "$prefix/lib/libssl.a" "${SYSROOT}/lib/"
    cp "$prefix/lib/libcrypto.a" "${SYSROOT}/lib/"

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

    # libc/ tree: mingw-w64 headers + crt + winpthreads + libgcc so a
    # host clang on Linux/mac can cross-compile to windows-gnu
    # (`--sysroot=<sr>/libc -B<sr>/libc/gcc`). mingw layout keeps
    # include/ and lib/ at the top level (no usr/).
    echo ">> Staging libc/ tree (mingw-w64 headers + crt + libgcc)"
    local lc="${SYSROOT}/libc"
    mkdir -p "$lc/lib" "$lc/gcc"
    cp -r "$prefix/include" "$lc/"
    cp "$prefix"/lib/*.o "$lc/lib/" 2>/dev/null || true
    cp "$prefix"/lib/libmingw32.a "$prefix"/lib/libmingwex.a \
       "$prefix"/lib/libmsvcrt.a "$prefix"/lib/libucrt*.a \
       "$prefix"/lib/libkernel32.a "$prefix"/lib/libuser32.a \
       "$prefix"/lib/libadvapi32.a "$prefix"/lib/libshell32.a \
       "$prefix"/lib/libws2_32.a "$prefix"/lib/libmswsock.a \
       "$prefix"/lib/libdbghelp.a "$prefix"/lib/libpsapi.a \
       "$prefix"/lib/libcrypt32.a "$prefix"/lib/libbcrypt.a \
       "$prefix"/lib/libntdll.a "$prefix"/lib/libwinpthread.a \
       "$prefix"/lib/libssp*.a \
       "$lc/lib/" 2>/dev/null || true
    # Everything else the mingw crt references at link time.
    cp "$prefix"/lib/lib*.a "$lc/lib/" 2>/dev/null || true
    local gd
    gd="$(ls -d "$prefix"/lib/gcc/x86_64-w64-mingw32/*/ 2>/dev/null | head -1)"
    if [ -n "$gd" ]; then
        cp "$gd"/crtbegin*.o "$gd"/crtend*.o "$gd"/libgcc.a "$gd"/libgcc_eh.a "$lc/gcc/" 2>/dev/null || true
    fi
}

# -------------------------------------------------------------------------
# macOS: take static libs from Homebrew's openssl@3 + zlib. Must run on
# a Mac of the matching arch (no cross-bottle for openssl static libs).
# No libc/ tree — the macOS target only builds natively (Apple clang);
# cross-compiling TO macOS is not supported.
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
    openssl_dir="$(brew --prefix openssl@3 2>/dev/null || true)"

    if [ -z "$openssl_dir" ] || [ ! -d "$openssl_dir/include/openssl" ] || [ ! -f "$openssl_dir/lib/libssl.a" ]; then
        echo "macos: openssl@3 not installed via brew or static libs missing" >&2
        echo "   brew install openssl@3" >&2
        exit 1
    fi
    echo ">> Staging from Homebrew: $openssl_dir"

    cp -r "$openssl_dir/include/openssl" "${SYSROOT}/include/"

    cp "$openssl_dir/lib/libssl.a" "${SYSROOT}/lib/"
    cp "$openssl_dir/lib/libcrypto.a" "${SYSROOT}/lib/"
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
parts="include lib"
[ -d "$SYSROOT/libc" ] && parts="$parts libc"
( cd "$SYSROOT" && tar -czf "../glide-sysroot-${TARGET}-${VERSION}.tar.gz" $parts )

SIZE=$(du -sh "$OUT" | cut -f1)
echo ">> Done: $OUT ($SIZE)"
echo ""
echo "Upload alongside the main release archives:"
echo "  gh release upload v${VERSION} ${OUT}"
echo ""
echo "Users will pull it via:"
echo "  glide target add ${TARGET}"

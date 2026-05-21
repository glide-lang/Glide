# Static-lib bundle pipeline

Produces the `.a` files (`libssl`, `libcrypto`, `libz`, `libngtcp2`,
`libnghttp3`, `libngtcp2_crypto_ossl`) that ride inside the release
tarball so end users don't need `apt install libssl-dev` / equivalents
before they can build Glide programs that use `stdlib::http` or
`stdlib::http::h3`.

## How it plugs in

```
tools/bundle/build.sh <target>           → dist/bundle-<target>/*.a
tools/build_release.sh                   ↑
        --target=<glide-triple>          consumes via --bundle-libs
        --bundle-libs=dist/bundle-<target>
```

`bootstrap/main.glide`'s `_host_lib_bundle_dir()` probes
`<install>/../lib/` (and `<install>/lib/`) for `libssl.a` at runtime.
When present, the build line switches from `-lssl -lcrypto ...` to
`-L<bundle> -l:libssl.a -l:libcrypto.a ...`, statically linking the
bundled libs.

## Targets

| Target                | Where to build                 | Source              |
|-----------------------|--------------------------------|---------------------|
| linux-x86_64-musl     | any host with Docker           | Alpine 3.20 base    |
| linux-aarch64-musl    | any host with Docker + qemu    | Alpine 3.20 + buildx|
| macos-x86_64          | macOS x86_64                   | brew + source build |
| macos-aarch64         | macOS arm64                    | brew + source build |
| windows-x86_64-mingw  | Windows + msys2 ucrt64 shell   | pacman packages     |

Linux is the easiest because Docker abstracts the host. macOS/Windows
need a matching runner (CI matrix or a real machine).

## Pinned upstream versions

Set in each `build.sh`:
- nghttp3 v1.5.0
- ngtcp2  v1.7.0

Both libraries move slowly. Bump together when an upstream release
brings a behaviour change we want (e.g. a 0-RTT key-derivation
adjustment in ngtcp2_crypto_ossl).

OpenSSL + zlib versions follow whatever the platform package manager
ships (Alpine 3.20 → openssl 3.3; brew → latest stable; msys2 →
rolling).

## CI matrix (suggested)

```yaml
jobs:
  bundle:
    strategy:
      matrix:
        target:
          - linux-x86_64-musl
          - linux-aarch64-musl
        runs-on: [ubuntu-latest]
    steps:
      - run: tools/bundle/build.sh ${{ matrix.target }}
      - uses: actions/upload-artifact@v4
        with: { name: bundle-${{ matrix.target }}, path: dist/bundle-${{ matrix.target }} }

  bundle-macos:
    strategy:
      matrix:
        include:
          - { target: macos-x86_64,  runs-on: macos-13   }
          - { target: macos-aarch64, runs-on: macos-14   }
    runs-on: ${{ matrix.runs-on }}
    steps:
      - run: brew install openssl@3 zlib autoconf automake libtool pkg-config
      - run: tools/bundle/build.sh ${{ matrix.target }}
      # ...upload artifact

  bundle-windows:
    runs-on: windows-latest
    steps:
      - uses: msys2/setup-msys2@v2
        with:
          msystem: UCRT64
          install: >-
            mingw-w64-ucrt-x86_64-openssl
            mingw-w64-ucrt-x86_64-zlib
            mingw-w64-ucrt-x86_64-ngtcp2
            mingw-w64-ucrt-x86_64-nghttp3
      - shell: msys2 {0}
        run: tools/bundle/build.sh windows-x86_64-mingw
      # ...upload artifact
```

The release workflow downloads all bundle artifacts and feeds each
into a matching `tools/build_release.sh --bundle-libs=...` invocation
to produce the per-target install tarballs.

## Verifying a bundle

After producing one, the sanity check is just running
`tools/build_release.sh` against it and confirming an end-user build
links cleanly:

```sh
# Stage the bundle into a release tarball
tools/build_release.sh --bundle-libs=dist/bundle-linux-x86_64-musl

# Extract somewhere and try to build a program that uses h3
tar -xf dist/glide-linux-x86_64-0.1.1.tar.gz -C /tmp/
PATH=/tmp/glide-linux-x86_64-0.1.1:$PATH \
    glide build examples/h3-hello.glide -o /tmp/h3-hello

# If h3-hello starts up + serves a request, the bundle is good.
```

The smoke test for h3 is intentionally end-to-end: linker errors come
from missing symbols (libssl gone), runtime crashes from API mismatch
(openssl version drift), and a healthy server proves both layers ok.

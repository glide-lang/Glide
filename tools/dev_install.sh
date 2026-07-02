#!/usr/bin/env bash
# Dev install (Windows / MSYS2 + git-bash).
#
# Cross-builds a SELF-CONTAINED glide.exe and installs it together with the
# CURRENT src/ + bootstrap/ into ~/.glide/bin. Keeping the
# binary and its src/ in lockstep is the point: the installed LSP indexes
# ~/.glide/bin/src/stdlib, so a stale src/ there is why autocomplete misses
# newly added modules (log, regex, …). Run this after changing the compiler
# or the stdlib.
#
#   bash tools/dev_install.sh
#
# Why the --target build instead of a plain host build: the host `cc`
# (ucrt64) ships no zlib.h, and the compiler uses stdlib::compress. A
# host-triple --target build rides the native mingw gcc against a tiny
# staged sysroot (real zlib + a stub openssl header the compiler never
# links) with -static, yielding a binary with no mingw DLL dependencies.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
GLIDE=./glide.exe
[ -x "$GLIDE" ] || { echo "no ./glide.exe in repo root — build one first" >&2; exit 1; }
INSTALL="$HOME/.glide/bin"
MG=/c/msys64/mingw64
AR="$MG/bin/ar"

# --- stage a minimal windows-gnu sysroot (zlib real + openssl stub) ---
SR="$HOME/.glide/targets/x86_64-windows-gnu"
rm -rf "$SR"; mkdir -p "$SR/include/openssl" "$SR/lib"
cp "$MG/include/zlib.h" "$MG/include/zconf.h" "$SR/include/"
cp "$MG/lib/libz.a" "$SR/lib/"
printf '/* stub: satisfies the sysroot probe; openssl is not linked */\n' > "$SR/include/openssl/ssl.h"
"$AR" rc "$SR/lib/libssl.a"; "$AR" rc "$SR/lib/libcrypto.a"

echo ">> cross-building self-contained glide.exe ..."
"$GLIDE" build bootstrap/main.glide --target=x86_64-windows-gnu -o dist_glide.exe

# Drop the stub right away — a real --target=x86_64-windows-gnu http build
# would otherwise link the empty openssl and fail confusingly.
rm -rf "$SR"

# --- install binary (rename the possibly-running one, then copy) ---
mkdir -p "$INSTALL"
[ -f "$INSTALL/glide.exe" ] && mv -f "$INSTALL/glide.exe" "$INSTALL/glide.exe.old" 2>/dev/null || true
cp dist_glide.exe "$INSTALL/glide.exe"
cp dist_glide.exe "$GLIDE"
rm -f "$INSTALL/glide.exe.old" dist_glide.exe dist_glide.exe.__glide.c

# --- sync src/ + bootstrap/ so the install matches the repo ---
# Copy OVER in place rather than rm-then-copy: a running editor LSP holds
# files under the install, so `rm -rf` fails "Device or resource busy".
# cp-over refreshes everything it can; any file locked by the live LSP is
# skipped (|| true) and updates next time the editor is closed. The binary
# above already swapped via rename, which Windows allows even while running.
echo ">> syncing src/ + bootstrap/ ..."
mkdir -p "$INSTALL/src" "$INSTALL/bootstrap"
cp -r src/.       "$INSTALL/src/"       2>/dev/null || true
cp -r bootstrap/. "$INSTALL/bootstrap/" 2>/dev/null || true
# Legacy: drop a zig toolchain a previous dev install left behind.
rm -rf "$INSTALL/runtime/zig" 2>/dev/null || true

echo ">> done -> $INSTALL"
"$INSTALL/glide.exe" version
echo ">> installed stdlib modules: $(ls "$INSTALL"/src/stdlib/*.glide 2>/dev/null | wc -l)"
echo "   restart your editor's Glide LSP to pick up the refreshed stdlib."

#!/usr/bin/env bash
# Smoke: a `kind: "kernel"` (freestanding) project links to a valid static
# ELF with the host C compiler — no libc, custom linker script, entry at
# `kmain`. This path had no coverage while it was zig-only; now it must
# work with any gcc/clang (or clang+lld from a non-x86_64 host).
set -u

GLIDE="${GLIDE:-glide}"

fail() { echo "FAIL: $*" >&2; exit 1; }

command -v "$GLIDE" >/dev/null 2>&1 || [ -x "$GLIDE" ] || fail "glide not found at $GLIDE"

T="$(mktemp -d)" || fail "mktemp failed"
trap 'rm -rf "$T"' EXIT

cat > "$T/glide.glide" <<'EOF'
let manifest: Package = Package {
    name:    "smoke_kernel",
    version: "0.0.1",
    kind:    "kernel",
    bin:     "kmain.glide",
    linker:  "link.ld",
};
EOF

cat > "$T/kmain.glide" <<'EOF'
// Minimal freestanding kernel: write to the VGA buffer and halt. No
// strings/runtime here — freestanding drops the hosted prelude, so only
// raw pointers and integers are available.
fn kmain() {
    let vga: *u16 = 0xB8000 as *u16;
    // "GL" in light-grey-on-black VGA cells.
    vga[0] = 0x0747 as u16;
    vga[1] = 0x074C as u16;
    while true {}
}
EOF

cat > "$T/link.ld" <<'EOF'
ENTRY(kmain)
SECTIONS {
    . = 1M;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
EOF

( cd "$T" && "$GLIDE" build 2> "$T/err.log" ) || {
    cat "$T/err.log" >&2
    fail "freestanding build failed"
}

ELF="$T/build/smoke_kernel"
[ -f "$ELF" ] || ELF="$T/smoke_kernel"
[ -f "$ELF" ] || ELF="$(find "$T" -maxdepth 3 -type f -name 'smoke_kernel*' | head -1)"
[ -n "$ELF" ] && [ -f "$ELF" ] || fail "kernel ELF not produced"

# LC_ALL=C: readelf/file localize their output, which breaks the greps.
LC_ALL=C file "$ELF" | grep -q "ELF 64-bit" || fail "not a 64-bit ELF: $(file "$ELF")"
LC_ALL=C file "$ELF" | grep -q "statically linked" || fail "not statically linked"
# The entry symbol must be kmain's address (not 0 / not _start).
entry="$(LC_ALL=C readelf -h "$ELF" | awk '/Entry point/ {print $NF}')"
kaddr="$(LC_ALL=C readelf -sW "$ELF" | awk '$8 == "kmain" {print "0x"$2}' | head -1)"
[ -n "$entry" ] && [ "$entry" != "0x0" ] || fail "e_entry is zero"
if [ -n "$kaddr" ]; then
    [ "$((entry))" -eq "$((kaddr))" ] || fail "entry $entry != kmain $kaddr"
fi
# No undefined symbols — freestanding means everything is in the image.
undef="$(LC_ALL=C nm -u "$ELF" 2>/dev/null | wc -l)"
[ "$undef" -eq 0 ] || fail "$undef undefined symbols: $(LC_ALL=C nm -u "$ELF" | head -3)"

echo "OK: freestanding kernel ELF ($entry entry, 0 undefined symbols)"

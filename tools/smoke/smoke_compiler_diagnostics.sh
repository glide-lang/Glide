#!/usr/bin/env bash
# Invalid Glide should fail in the checker with a useful diagnostic, before
# generated C reaches the C compiler.
set -eu

GLIDE="${GLIDE:-glide}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

cat > "$T/field.glide" <<'EOF'
struct Inner {}
impl Inner {
    fn shutdown(self: *Inner, how: i32) {}
}
struct Outer { inner: *Inner }
fn main() {
    let inner: *Inner = Inner {};
    let outer: Outer = Outer { inner: inner };
    outer.inner.shutdown();
}
EOF

if "$GLIDE" check "$T/field.glide" > "$T/field.log" 2>&1; then
    echo "FAIL: field receiver's missing argument passed checking" >&2
    exit 1
fi
grep -q 'wrong arg count for `shutdown`' "$T/field.log" || {
    cat "$T/field.log" >&2
    exit 1
}

cat > "$T/spawn.glide" <<'EOF'
struct Worker {}
impl Worker {
    fn run(self) {}
}
fn main() {
    let w: Worker = Worker {};
    spawn w.run();
}
EOF

if "$GLIDE" check "$T/spawn.glide" > "$T/spawn.log" 2>&1; then
    echo "FAIL: spawn of a stack receiver escaped checking" >&2
    exit 1
fi
grep -q 'spawn-method-receiver' "$T/spawn.log" || {
    cat "$T/spawn.log" >&2
    exit 1
}

cat > "$T/c_raw.glide" <<'EOF'
c_raw! {
static double sse(double a, double b) { return a + b; }
}
extern fn sse(a: f64, b: f64, q: f64) -> f64;
fn main() -> i32 { return 0; }
EOF

if "$GLIDE" build "$T/c_raw.glide" -o "$T/c_raw" > "$T/c_raw.log" 2>&1; then
    echo "FAIL: mismatched c_raw extern signature built" >&2
    exit 1
fi
grep -q 'c-raw-error' "$T/c_raw.log" || {
    cat "$T/c_raw.log" >&2
    exit 1
}

echo "PASS: field arity, spawn receiver, and c_raw diagnostics"

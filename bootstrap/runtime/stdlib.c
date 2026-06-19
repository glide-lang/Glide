// ---- glide runtime ----
// Slim-runtime markers: when set, the corresponding family of helpers
// is NOT in this stdlib.c — a c_raw block in the matching stdlib module
// (`fs.glide`, `os.glide`, `env.glide`, `io.glide`) provides the bodies.
// Older glide binaries that still carry the bodies in their embedded
// stdlib.c don't see these defines, so the c_raw blocks stay inactive
// during the bootstrap step that promotes the new compiler.
#define GLIDE_FS_VIA_CRAW
#define GLIDE_OS_VIA_CRAW
#define GLIDE_ENV_VIA_CRAW
#define GLIDE_IO_VIA_CRAW
#define GLIDE_PROCESS_VIA_CRAW
#define GLIDE_SIGNAL_VIA_CRAW
// Forward declaration of the per-keystroke arena allocator defined in
// src/builtins/builtins.glide. Routing the string-runtime allocations
// through it lets the LSP reclaim every concat / substring / format /
// int_to_string buffer in bulk on the next reanalysis. Outside the LSP
// (no arena active) __glide_palloc falls back to calloc, so build /
// run / fmt paths see no behaviour change.
extern void* __glide_palloc(int size);
/* ---- SDS strings ---------------------------------------------------------
   A 4-byte little-endian length header sits immediately BEFORE the data; the
   `string` value points at the NUL-terminated data, so the C FFI is unaffected
   (C sees a normal char*). `__glide_str_alloc(len)` allocates header+data+NUL,
   stamps the length, and returns the data pointer. Reading the length is O(1)
   once `__glide_string_len` switches to the header (phase 2); until then it
   stays strlen-based and the header is written-but-unused. Strings are never
   individually freed (arena-managed), so the header is reclaimed with the
   data — no free-path change. */
static void __glide_str_hdr_set(char* base, int len) {
    base[0] = (char)(len & 0xff);
    base[1] = (char)((len >> 8) & 0xff);
    base[2] = (char)((len >> 16) & 0xff);
    base[3] = (char)((len >> 24) & 0xff);
}
static char* __glide_str_alloc(int len) {
    if (len < 0) len = 0;
    char* base = (char*)__glide_palloc(4 + len + 1);
    __glide_str_hdr_set(base, len);
    base[4 + len] = 0;
    return base + 4;
}
/* Header length read (unaligned-safe). Used by phase-2 __glide_string_len. */
static int __glide_str_hdr_len(const char* s) {
    int l; memcpy(&l, s - 4, 4); return l;
}
/* Phase 2: O(1) length from the header (every string is headered — literals via
   codegen, allocations via __glide_str_alloc, FFI returns via __glide_string_adopt). */
static int __glide_string_len(const char* s) { return __glide_str_hdr_len(s); }
static bool __glide_string_eq(const char* a, const char* b) {
    int la = __glide_str_hdr_len(a), lb = __glide_str_hdr_len(b);
    if (la != lb) return false;
    return memcmp(a, b, (size_t)la) == 0;
}
static char __glide_string_at(const char* s, int i) { return s[i]; }
/* Wrap a raw (headerless) C string — e.g. an FFI return — into a headered copy. */
static const char* __glide_string_adopt(const char* c) {
    if (c == 0) return 0;
    size_t n = strlen(c);
    char* out = __glide_str_alloc((int)n);
    memcpy(out, c, n);
    return out;
}
static const char* __glide_string_concat(const char* a, const char* b) {
    int la = __glide_str_hdr_len(a), lb = __glide_str_hdr_len(b);   /* O(1), no strlen */
    char* out = __glide_str_alloc(la + lb);
    memcpy(out, a, la); memcpy(out + la, b, (size_t)lb);
    return out;
}
static const char* __glide_string_substring(const char* s, int start, int end) {
    /* O(end-start), not O(strlen(s)): copy from `start` up to `end`, stopping
       at the NUL terminator. The actual copied length `j` may be < cap, so we
       stamp the header with `j`. Callers always pass start within [0, len]. */
    if (start < 0) start = 0;
    if (end < start) end = start;
    int cap = end - start;
    char* base = (char*)__glide_palloc(4 + cap + 1);
    char* out = base + 4;
    int j = 0;
    for (int i = 0; i < cap; i++) {
        char c = s[start + i];
        if (c == 0) break;
        out[j++] = c;
    }
    out[j] = 0;
    __glide_str_hdr_set(base, j);
    return out;
}
/* Backs the panic! / todo! / unimplemented! / unreachable! macros: print the
   message + source location, then abort (the trap handler prints a stack
   trace). assert! routes through __glide_assert. */
static void __glide_panic(const char* msg, const char* file, int line) {
    fprintf(stderr, "\n\x1b[1;31mpanic\x1b[0m: %s\n  \x1b[1;36mat\x1b[0m %s:%d\n",
            msg ? msg : "", file ? file : "?", line);
    fflush(stderr);
    abort();
}
/* Take a raw byte buffer + length and produce a NUL-terminated Glide
   string in one allocation. Used by the HTTP server to skip the
   per-byte concat that would otherwise allocate `n` strings to
   convert one read buffer. */
const char* __glide_string_from_buf(void* buf, int n) {
    if (n < 0) n = 0;
    // Arena-aware (see __glide_str_alloc): the header+data live in the active
    // arena and are reclaimed in bulk, or via __glide_pfree at scope exit.
    char* out = __glide_str_alloc(n);
    if (n > 0) memcpy(out, buf, (size_t)n);
    return out;
}
static int __glide_char_to_int(char c) { return (int)(unsigned char)c; }
static bool __glide_char_is_digit(char c) { return c >= '0' && c <= '9'; }
static bool __glide_char_is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
/* Wrap a single byte into a fresh 1-char string so user code can push it into a
   builder (concat etc.). The byte is re-allocated each call — fine for the
   one-shot helper case; tight loops should buffer differently. */
static const char* __glide_char_to_string(char c) {
    char* out = __glide_str_alloc(1);
    out[0] = c;
    return out;
}
/* 64-bit so every int width (i8..i64, isize) shares one routine; narrower
   types promote losslessly and the result is assigned back to the caller's
   own type. */
static long long __glide_int_abs(long long n) { return n < 0 ? -n : n; }
/* `.to_int()` on a numeric — narrows the receiver to i32. Inlines to a bare
   cast at -O2, so it costs nothing. (char.to_int() has its own helper.) */
static int __glide_int_to_int(long long n) { return (int)n; }
static int __glide_uint_to_int(unsigned long long n) { return (int)n; }
static const char* __glide_int_to_string(long long n) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", n);
    char* out = __glide_str_alloc(len);
    memcpy(out, buf, (size_t)len);
    return out;
}
/* 128-bit helpers. printf has no length modifier for __int128, so to_string
   builds the digits by hand. abs negates through the unsigned domain so
   INT128_MIN doesn't overflow. */
static const char* __glide_u128_to_string(unsigned __int128 n) {
    char tmp[40];
    int t = 0;
    if (n == 0) { tmp[t++] = '0'; }
    while (n > 0) { tmp[t++] = (char)('0' + (int)(n % 10)); n /= 10; }
    char* out = __glide_str_alloc(t);
    for (int i = 0; i < t; i++) out[i] = tmp[t - 1 - i];
    return out;
}
static const char* __glide_i128_to_string(__int128 n) {
    if (n < 0) {
        unsigned __int128 m = (unsigned __int128)(-(n + 1)) + 1;
        const char* s = __glide_u128_to_string(m);
        size_t len = strlen(s);
        char* out = __glide_str_alloc((int)(len + 1));
        out[0] = '-';
        memcpy(out + 1, s, len);
        return out;
    }
    return __glide_u128_to_string((unsigned __int128)n);
}
static __int128 __glide_i128_abs(__int128 n) { return n < 0 ? -n : n; }
static unsigned __int128 __glide_u128_abs(unsigned __int128 n) { return n; }

/* ---- 256-bit integers (software, 4x u64 limbs, d[0] least significant) ----
   One C struct backs both u256 and i256; the signed ops reinterpret the same
   two's-complement bits. Native C has no >128-bit int, so every operator the
   compiler emits for these types routes here. */
typedef struct { unsigned long long d[4]; } __glide_u256;

static __glide_u256 __glide_u256_from_u64(unsigned long long v) {
    __glide_u256 r; r.d[0] = v; r.d[1] = 0; r.d[2] = 0; r.d[3] = 0; return r;
}
static __glide_u256 __glide_u256_from_i64(long long v) {
    __glide_u256 r; r.d[0] = (unsigned long long)v;
    unsigned long long ext = (v < 0) ? ~0ULL : 0ULL;
    r.d[1] = ext; r.d[2] = ext; r.d[3] = ext; return r;
}
static int __glide_u256_is_zero(__glide_u256 a) {
    return (a.d[0] | a.d[1] | a.d[2] | a.d[3]) == 0;
}
static __glide_u256 __glide_u256_add(__glide_u256 a, __glide_u256 b) {
    __glide_u256 r; unsigned __int128 carry = 0;
    for (int i = 0; i < 4; i++) {
        unsigned __int128 s = (unsigned __int128)a.d[i] + b.d[i] + carry;
        r.d[i] = (unsigned long long)s; carry = s >> 64;
    }
    return r;
}
static __glide_u256 __glide_u256_sub(__glide_u256 a, __glide_u256 b) {
    __glide_u256 r; unsigned __int128 borrow = 0;
    for (int i = 0; i < 4; i++) {
        unsigned __int128 s = (unsigned __int128)a.d[i] - b.d[i] - borrow;
        r.d[i] = (unsigned long long)s; borrow = (s >> 64) & 1;
    }
    return r;
}
static __glide_u256 __glide_u256_mul(__glide_u256 a, __glide_u256 b) {
    __glide_u256 r; for (int i = 0; i < 4; i++) r.d[i] = 0;
    for (int i = 0; i < 4; i++) {
        unsigned __int128 carry = 0;
        for (int j = 0; i + j < 4; j++) {
            unsigned __int128 cur = (unsigned __int128)r.d[i + j]
                + (unsigned __int128)a.d[i] * b.d[j] + carry;
            r.d[i + j] = (unsigned long long)cur; carry = cur >> 64;
        }
    }
    return r;
}
static int __glide_u256_cmp(__glide_u256 a, __glide_u256 b) {
    for (int i = 3; i >= 0; i--) {
        if (a.d[i] < b.d[i]) return -1;
        if (a.d[i] > b.d[i]) return 1;
    }
    return 0;
}
static __glide_u256 __glide_u256_and(__glide_u256 a, __glide_u256 b) {
    __glide_u256 r; for (int i = 0; i < 4; i++) r.d[i] = a.d[i] & b.d[i]; return r;
}
static __glide_u256 __glide_u256_or(__glide_u256 a, __glide_u256 b) {
    __glide_u256 r; for (int i = 0; i < 4; i++) r.d[i] = a.d[i] | b.d[i]; return r;
}
static __glide_u256 __glide_u256_xor(__glide_u256 a, __glide_u256 b) {
    __glide_u256 r; for (int i = 0; i < 4; i++) r.d[i] = a.d[i] ^ b.d[i]; return r;
}
static __glide_u256 __glide_u256_shl(__glide_u256 a, __glide_u256 bsh) {
    unsigned long long n = bsh.d[0] & 255;
    __glide_u256 r; for (int i = 0; i < 4; i++) r.d[i] = 0;
    int limb = (int)(n / 64), bit = (int)(n % 64);
    for (int i = 3; i >= 0; i--) {
        int src = i - limb; if (src < 0) continue;
        r.d[i] |= a.d[src] << bit;
        if (bit && src - 1 >= 0) r.d[i] |= a.d[src - 1] >> (64 - bit);
    }
    return r;
}
static __glide_u256 __glide_u256_shr(__glide_u256 a, __glide_u256 bsh) {
    unsigned long long n = bsh.d[0] & 255;
    __glide_u256 r; for (int i = 0; i < 4; i++) r.d[i] = 0;
    int limb = (int)(n / 64), bit = (int)(n % 64);
    for (int i = 0; i < 4; i++) {
        int src = i + limb; if (src > 3) continue;
        r.d[i] |= a.d[src] >> bit;
        if (bit && src + 1 <= 3) r.d[i] |= a.d[src + 1] << (64 - bit);
    }
    return r;
}
static void __glide_u256_divmod(__glide_u256 a, __glide_u256 b,
                                __glide_u256* q, __glide_u256* rem) {
    __glide_u256 quo; for (int i = 0; i < 4; i++) quo.d[i] = 0;
    __glide_u256 r;   for (int i = 0; i < 4; i++) r.d[i] = 0;
    if (__glide_u256_is_zero(b)) { *q = quo; *rem = r; return; }
    for (int bit = 255; bit >= 0; bit--) {
        unsigned long long carry = 0;   /* r <<= 1 */
        for (int i = 0; i < 4; i++) {
            unsigned long long nc = r.d[i] >> 63;
            r.d[i] = (r.d[i] << 1) | carry; carry = nc;
        }
        r.d[0] |= (a.d[bit / 64] >> (bit % 64)) & 1ULL;
        if (__glide_u256_cmp(r, b) >= 0) {
            r = __glide_u256_sub(r, b);
            quo.d[bit / 64] |= (1ULL << (bit % 64));
        }
    }
    *q = quo; *rem = r;
}
static __glide_u256 __glide_u256_div(__glide_u256 a, __glide_u256 b) {
    __glide_u256 q, r; __glide_u256_divmod(a, b, &q, &r); return q;
}
static __glide_u256 __glide_u256_mod(__glide_u256 a, __glide_u256 b) {
    __glide_u256 q, r; __glide_u256_divmod(a, b, &q, &r); return r;
}
static const char* __glide_u256_to_string(__glide_u256 a) {
    if (__glide_u256_is_zero(a)) { char* z = __glide_str_alloc(1); z[0] = '0'; return z; }
    char tmp[80]; int t = 0;
    __glide_u256 ten = __glide_u256_from_u64(10);
    while (!__glide_u256_is_zero(a)) {
        __glide_u256 q, r; __glide_u256_divmod(a, ten, &q, &r);
        tmp[t++] = (char)('0' + (int)r.d[0]); a = q;
    }
    char* out = __glide_str_alloc(t);
    for (int i = 0; i < t; i++) out[i] = tmp[t - 1 - i];
    return out;
}
static __glide_u256 __glide_u256_abs(__glide_u256 a) { return a; }  /* unsigned: identity */
/* ---- i256: signed view over the same bits ---- */
static int __glide_i256_is_neg(__glide_u256 a) { return (int)((a.d[3] >> 63) & 1); }
static __glide_u256 __glide_i256_neg(__glide_u256 a) {
    __glide_u256 r; for (int i = 0; i < 4; i++) r.d[i] = ~a.d[i];
    return __glide_u256_add(r, __glide_u256_from_u64(1));
}
static __glide_u256 __glide_i256_abs(__glide_u256 a) {
    return __glide_i256_is_neg(a) ? __glide_i256_neg(a) : a;
}
static int __glide_i256_cmp(__glide_u256 a, __glide_u256 b) {
    int na = __glide_i256_is_neg(a), nb = __glide_i256_is_neg(b);
    if (na != nb) return na ? -1 : 1;
    return __glide_u256_cmp(a, b);
}
static const char* __glide_i256_to_string(__glide_u256 a) {
    if (__glide_i256_is_neg(a)) {
        __glide_u256 m = __glide_i256_neg(a);
        const char* s = __glide_u256_to_string(m);
        size_t len = strlen(s);
        char* out = __glide_str_alloc((int)(len + 1));
        out[0] = '-'; memcpy(out + 1, s, len); return out;
    }
    return __glide_u256_to_string(a);
}
static __glide_u256 __glide_i256_div(__glide_u256 a, __glide_u256 b) {
    int na = __glide_i256_is_neg(a), nb = __glide_i256_is_neg(b);
    __glide_u256 ua = na ? __glide_i256_neg(a) : a;
    __glide_u256 ub = nb ? __glide_i256_neg(b) : b;
    __glide_u256 q = __glide_u256_div(ua, ub);
    return (na ^ nb) ? __glide_i256_neg(q) : q;
}
static __glide_u256 __glide_i256_mod(__glide_u256 a, __glide_u256 b) {
    int na = __glide_i256_is_neg(a);
    __glide_u256 ua = na ? __glide_i256_neg(a) : a;
    __glide_u256 ub = __glide_i256_is_neg(b) ? __glide_i256_neg(b) : b;
    __glide_u256 r = __glide_u256_mod(ua, ub);
    return na ? __glide_i256_neg(r) : r;
}
/* i256 add/sub/mul/bitwise/shift are bit-identical to u256 (two's complement);
   alias so codegen can emit a uniform `__glide_<ty>_<op>` per operand type. */
static __glide_u256 __glide_i256_from_i64(long long v) { return __glide_u256_from_i64(v); }
static __glide_u256 __glide_i256_from_u64(unsigned long long v) { return __glide_u256_from_u64(v); }
static __glide_u256 __glide_i256_add(__glide_u256 a, __glide_u256 b) { return __glide_u256_add(a, b); }
static __glide_u256 __glide_i256_sub(__glide_u256 a, __glide_u256 b) { return __glide_u256_sub(a, b); }
static __glide_u256 __glide_i256_mul(__glide_u256 a, __glide_u256 b) { return __glide_u256_mul(a, b); }
static __glide_u256 __glide_i256_and(__glide_u256 a, __glide_u256 b) { return __glide_u256_and(a, b); }
static __glide_u256 __glide_i256_or(__glide_u256 a, __glide_u256 b)  { return __glide_u256_or(a, b); }
static __glide_u256 __glide_i256_xor(__glide_u256 a, __glide_u256 b) { return __glide_u256_xor(a, b); }
static __glide_u256 __glide_i256_shl(__glide_u256 a, __glide_u256 b) { return __glide_u256_shl(a, b); }
static __glide_u256 __glide_i256_shr(__glide_u256 a, __glide_u256 b) { return __glide_u256_shr(a, b); }
/* limits for `u256::MAX` / `i256::MIN` etc. (structs, so not C constants) */
static __glide_u256 __glide_u256_max(void) { __glide_u256 r; r.d[0]=~0ULL; r.d[1]=~0ULL; r.d[2]=~0ULL; r.d[3]=~0ULL; return r; }
static __glide_u256 __glide_u256_min(void) { return __glide_u256_from_u64(0); }
static __glide_u256 __glide_i256_max(void) { __glide_u256 r; r.d[0]=~0ULL; r.d[1]=~0ULL; r.d[2]=~0ULL; r.d[3]=0x7fffffffffffffffULL; return r; }
static __glide_u256 __glide_i256_min(void) { __glide_u256 r; r.d[0]=0; r.d[1]=0; r.d[2]=0; r.d[3]=0x8000000000000000ULL; return r; }
/* Unsigned <=64-bit to_string: the signed __glide_int_to_string truncates
   values above i64 max (u64::MAX would print as -1). */
static const char* __glide_uint_to_string(unsigned long long n) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu", n);
    char* out = __glide_str_alloc(len);
    memcpy(out, buf, (size_t)len);
    return out;
}
static unsigned long long __glide_uint_abs(unsigned long long n) { return n; }
/* Headered statics so the length header is valid after phase 2. The struct lays
   the int length at offset 0 and the chars at offset 4, so `d - 4` reads it. */
static const char* __glide_bool_to_string(bool b) {
    static const struct { int h; char d[5]; } bt = {4, "true"};
    static const struct { int h; char d[6]; } bf = {5, "false"};
    return b ? bt.d : bf.d;
}
#include <stdarg.h>
static const char* __glide_format(const char* fmt, ...) {
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    int n = vsnprintf(NULL, 0, fmt, ap1);
    va_end(ap1);
    char* out = __glide_str_alloc(n);
    vsnprintf(out, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return out;
}
static int __glide_argc_g = 0;
static char** __glide_argv_g = NULL;
static void __glide_args_init(int argc, char** argv) { __glide_argc_g = argc; __glide_argv_g = argv; }
static int args_count(void) { return __glide_argc_g; }
static const char* args_at(int i) {
    if (i < 0 || i >= __glide_argc_g) return "";
    return __glide_argv_g[i];
}
#ifdef _WIN32
#include <io.h>
#include <winsock2.h>     /* must precede windows.h on mingw-w64 */
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
__attribute__((constructor)) static void __glide_enable_vt(void) {
    HANDLE hs[2] = { GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    for (int i = 0; i < 2; i++) {
        DWORD m = 0;
        if (GetConsoleMode(hs[i], &m)) {
            SetConsoleMode(hs[i], m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
}
#endif
// Match Go: os.Stdout / os.Stderr are unbuffered. Without this, printf from a
// spawned coroutine (worker thread) sits in the stdio block buffer and is never
// drained when main loops forever, so the output silently disappears. _IONBF
// (not _IOLBF) because the Windows CRT treats line-buffering as full-buffering.
__attribute__((constructor)) static void __glide_unbuffer_stdio(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}
#ifdef _WIN32
#include <dbghelp.h>
#include <psapi.h>
static LONG WINAPI __glide_seh_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char* name = "unhandled exception";
    const char* hint = "";
    if (code == 0xC0000005) { name = "segmentation fault"; hint = "hint: dereferenced a null or invalid pointer"; }
    else if (code == 0xC0000094) { name = "integer divide by zero"; hint = "hint: divisor reached zero before the division"; }
    else if (code == 0xC00000FD) { name = "stack overflow"; hint = "hint: runtime recursion exceeded the stack limit"; }
    else if (code == 0x80000003) { name = "trap (likely null deref or undefined behavior)"; hint = "hint: the optimizer turned a null/invalid op into __builtin_trap; rebuild with `-O0` for clearer location"; }
    fflush(stdout);
    fprintf(stderr, "\n\x1b[1;31mfatal\x1b[0m: %s (code 0x%lx)\n", name, (unsigned long)code);
    if (hint[0]) fprintf(stderr, "  \x1b[1;36m=\x1b[0m %s\n", hint);
    // For access violations, the second ExceptionInformation entry is
    // the offending memory address. Telling the user "you tried to
    // read 0x0" is a big leg up vs raw stack frames.
    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
        ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
        const char* op_s = (op == 0) ? "read" : (op == 1) ? "write" : "execute";
        fprintf(stderr, "  \x1b[1;36m=\x1b[0m faulting %s of address %p\n", op_s, (void*)fault_addr);
    }
    // Symbolicate. SymInitialize + SymFromAddr resolves to function
    // names; SymGetLineFromAddr64 maps the IP back to the emitted C
    // file + line. Needs `-g` at link time and a populated symbol
    // search path — both default-on in glide's build pipeline.
    HANDLE proc = GetCurrentProcess();
    static int sym_inited = 0;
    if (!sym_inited) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        sym_inited = SymInitialize(proc, NULL, TRUE);
    }
    void* frames[32];
    USHORT n = CaptureStackBackTrace(1, 32, frames, NULL);
    char sym_buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)sym_buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    IMAGEHLP_LINE64 line;
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    // Resolve dbghelp output for every frame up-front. User-code
    // frames inside the EXE go through addr2line, system DLL frames
    // through dbghelp. By gathering both before we print we can lay
    // out one unified trace instead of two disjoint lists.
    typedef struct { char text[512]; } FrameLine;
    FrameLine lines[32];
    for (USHORT i = 0; i < 32; i++) { lines[i].text[0] = 0; }

    // Identify which frames came out of the running EXE — we'll
    // batch those through addr2line for source-level resolution.
    HMODULE hmod = GetModuleHandleA(NULL);
    ULONG_PTR runtime_base = (ULONG_PTR)hmod;
    ULONG_PTR runtime_end = runtime_base;
    MODULEINFO mi;
    if (GetModuleInformation(proc, hmod, &mi, sizeof(mi))) {
        runtime_end = runtime_base + mi.SizeOfImage;
    }
    int user_frame_idx[32];
    ULONG_PTR user_frame_addrs[32];  // ASLR-adjusted, for addr2line
    int n_user = 0;

    // Read on-disk ImageBase once — addr2line resolves against the
    // PE-baked address, not the runtime ASLR'd one.
    ULONG_PTR image_base = runtime_base;
    char exe[MAX_PATH] = {0};
    DWORD got = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (got > 0 && got < sizeof(exe)) {
        FILE* f = fopen(exe, "rb");
        if (f) {
            IMAGE_DOS_HEADER dos;
            if (fread(&dos, sizeof(dos), 1, f) == 1
                && dos.e_magic == IMAGE_DOS_SIGNATURE
                && fseek(f, dos.e_lfanew, SEEK_SET) == 0) {
                IMAGE_NT_HEADERS nt;
                if (fread(&nt, sizeof(nt), 1, f) == 1
                    && nt.Signature == IMAGE_NT_SIGNATURE) {
                    image_base = (ULONG_PTR)nt.OptionalHeader.ImageBase;
                }
            }
            fclose(f);
        }
    }

    for (USHORT i = 0; i < n; i++) {
        ULONG_PTR addr = (ULONG_PTR)frames[i];
        // User-code frame: stash for batch addr2line resolution.
        if (addr >= runtime_base && addr < runtime_end && got > 0) {
            user_frame_idx[n_user] = i;
            user_frame_addrs[n_user] = (addr - runtime_base) + image_base;
            n_user++;
            continue;
        }
        // System DLL frame: try dbghelp.
        const char* fn_name = NULL;
        if (sym_inited && SymFromAddr(proc, (DWORD64)addr, NULL, sym)) {
            fn_name = sym->Name;
        }
        DWORD displ = 0;
        if (sym_inited && fn_name
            && SymGetLineFromAddr64(proc, (DWORD64)addr, &displ, &line)) {
            snprintf(lines[i].text, sizeof(lines[i].text),
                "%s  at  %s:%lu", fn_name, line.FileName,
                (unsigned long)line.LineNumber);
        } else if (fn_name) {
            snprintf(lines[i].text, sizeof(lines[i].text),
                "%s", fn_name);
        } else {
            snprintf(lines[i].text, sizeof(lines[i].text),
                "0x%p", (void*)addr);
        }
    }

    // Spawn addr2line once for all user-code frames. The mingw/zig
    // pipeline emits DWARF, which dbghelp can't read; addr2line is
    // shipped with the same toolchain so it's on PATH for any glide
    // user. -fpC prints "func at file:line" on one line per address.
    int a2l_ok = 0;
    if (n_user > 0) {
        char addrs[32 * 24] = {0};
        int addrs_len = 0;
        for (int k = 0; k < n_user; k++) {
            addrs_len += snprintf(addrs + addrs_len,
                sizeof(addrs) - addrs_len,
                " 0x%llx", (unsigned long long)user_frame_addrs[k]);
        }
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "addr2line -e \"%s\" -fpsC%s 2>nul", exe, addrs);
        FILE* a2l = _popen(cmd, "r");
        if (a2l) {
            int k = 0;
            char line_buf[1024];
            while (k < n_user && fgets(line_buf, sizeof(line_buf), a2l)) {
                // Strip trailing newline.
                size_t L = strlen(line_buf);
                while (L > 0 && (line_buf[L-1] == '\n' || line_buf[L-1] == '\r')) {
                    line_buf[--L] = 0;
                }
                if (L == 0) continue;
                int idx = user_frame_idx[k++];
                snprintf(lines[idx].text, sizeof(lines[idx].text),
                    "%s", line_buf);
            }
            int rc = _pclose(a2l);
            a2l_ok = (k > 0 && rc == 0);
            // Frames addr2line didn't fill get a raw-address placeholder
            // (-s already gave us basenames; if we got nothing, fall
            // back to a copy-paste hint at the end).
            while (k < n_user) {
                int idx = user_frame_idx[k];
                snprintf(lines[idx].text, sizeof(lines[idx].text),
                    "0x%llx (in %s)",
                    (unsigned long long)user_frame_addrs[k],
                    exe);
                k++;
            }
        } else {
            for (int k = 0; k < n_user; k++) {
                int idx = user_frame_idx[k];
                snprintf(lines[idx].text, sizeof(lines[idx].text),
                    "0x%llx (in %s)",
                    (unsigned long long)user_frame_addrs[k],
                    exe);
            }
        }
    }

    // Single unified stack trace.
    fprintf(stderr, "stack trace (%u frames):\n", n);
    for (USHORT i = 0; i < n; i++) {
        fprintf(stderr, "  #%-2u  %s\n", i, lines[i].text);
    }
    if (n_user > 0 && !a2l_ok) {
        // addr2line didn't run or didn't produce output — print the
        // copy-paste hint so the user can resolve manually.
        char addrs[32 * 24] = {0};
        int addrs_len = 0;
        for (int k = 0; k < n_user; k++) {
            addrs_len += snprintf(addrs + addrs_len,
                sizeof(addrs) - addrs_len,
                " 0x%llx", (unsigned long long)user_frame_addrs[k]);
        }
        fprintf(stderr, "\n  \x1b[1;36m=\x1b[0m resolve user frames with:\n");
        fprintf(stderr, "      addr2line -e \"%s\" -fpsC%s\n", exe, addrs);
    }
    fflush(stderr);
    ExitProcess((UINT)code);
    return EXCEPTION_CONTINUE_SEARCH;
}
__attribute__((constructor)) static void __glide_install_trap(void) {
    if (getenv("GLIDE_NO_TRAP")) return;
    // VEH runs before any SEH/CRT-installed filter, so our handler stays in charge.
    AddVectoredExceptionHandler(1, __glide_seh_handler);
    SetUnhandledExceptionFilter(__glide_seh_handler);
}
#else
#include <signal.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <execinfo.h>
#define GLIDE_HAVE_BACKTRACE 1
#endif
static void __glide_sig_handler(int sig) {
    const char* name = "unknown";
    const char* hint = "";
    if (sig == SIGSEGV) { name = "segmentation fault"; hint = "hint: dereferenced a null or invalid pointer"; }
    else if (sig == SIGFPE) { name = "floating-point / arithmetic error"; hint = "hint: division by zero or invalid float operation"; }
    else if (sig == SIGABRT) { name = "aborted"; hint = "hint: runtime panic (e.g. arena oom)"; }
    fflush(stdout);
    fprintf(stderr, "\n\x1b[1;31mfatal\x1b[0m: %s (signal %d)\n", name, sig);
    if (hint[0]) fprintf(stderr, "  \x1b[1;36m=\x1b[0m %s\n", hint);
#ifdef GLIDE_HAVE_BACKTRACE
    void* frames[32]; int n = backtrace(frames, 32);
    fprintf(stderr, "stack trace (%d frames):\n", n);
    backtrace_symbols_fd(frames, n, 2);
#else
    fprintf(stderr, "stack trace: unavailable on this libc\n");
#endif
    _exit(128 + sig);
}
__attribute__((constructor)) static void __glide_install_trap(void) {
    if (getenv("GLIDE_NO_TRAP")) return;
    signal(SIGSEGV, __glide_sig_handler);
    signal(SIGABRT, __glide_sig_handler);
    signal(SIGFPE,  __glide_sig_handler);
}
#endif
typedef struct Arena { unsigned char* head; int cap; int used; } Arena;
static Arena* Arena_new(int cap) {
    Arena* a = (Arena*)malloc(sizeof(Arena));
    a->head = (unsigned char*)malloc((size_t)cap);
    a->cap = cap; a->used = 0;
    return a;
}
static void* Arena_alloc(Arena* a, int size) {
    int aligned = (size + 7) & ~7;
    if (a->used + aligned > a->cap) { fprintf(stderr, "arena oom\n"); exit(1); }
    void* p = (void*)(a->head + a->used);
    a->used += aligned;
    return p;
}
static void Arena_free(Arena* a) { free(a->head); free(a); }
static int Arena_used(Arena* a) { return a->used; }
static void Arena_reset(Arena* a) { a->used = 0; }



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
static int __glide_string_len(const char* s) { return (int)strlen(s); }
static bool __glide_string_eq(const char* a, const char* b) { return strcmp(a, b) == 0; }
static char __glide_string_at(const char* s, int i) { return s[i]; }
static const char* __glide_string_concat(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* out = (char*)malloc(la + lb + 1);
    memcpy(out, a, la); memcpy(out + la, b, lb); out[la + lb] = 0;
    return out;
}
static const char* __glide_string_substring(const char* s, int start, int end) {
    int n = (int)strlen(s);
    if (start < 0) start = 0;
    if (end > n) end = n;
    if (start > end) start = end;
    int len = end - start;
    char* out = (char*)malloc((size_t)len + 1);
    memcpy(out, s + start, (size_t)len); out[len] = 0;
    return out;
}
/* Take a raw byte buffer + length and produce a NUL-terminated Glide
   string in one allocation. Used by the HTTP server to skip the
   per-byte concat that would otherwise allocate `n` strings to
   convert one read buffer. */
const char* __glide_string_from_buf(void* buf, int n) {
    if (n < 0) n = 0;
    char* out = (char*)malloc((size_t)n + 1);
    if (n > 0) memcpy(out, buf, (size_t)n);
    out[n] = 0;
    return out;
}
static int __glide_char_to_int(char c) { return (int)(unsigned char)c; }
static bool __glide_char_is_digit(char c) { return c >= '0' && c <= '9'; }
static bool __glide_char_is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
/* Wrap a single byte into a fresh 1-char string so user code can push it into a
   builder (concat etc.). The byte is re-allocated each call — fine for the
   one-shot helper case; tight loops should buffer differently. */
static const char* __glide_char_to_string(char c) {
    char* out = (char*)malloc(2);
    out[0] = c; out[1] = 0;
    return out;
}
static int __glide_int_abs(int n) { return n < 0 ? -n : n; }
static const char* __glide_int_to_string(int n) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", n);
    char* out = (char*)malloc((size_t)len + 1);
    memcpy(out, buf, (size_t)len + 1);
    return out;
}
static const char* __glide_bool_to_string(bool b) { return b ? "true" : "false"; }
#include <stdarg.h>
static const char* __glide_format(const char* fmt, ...) {
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    int n = vsnprintf(NULL, 0, fmt, ap1);
    va_end(ap1);
    char* out = (char*)malloc((size_t)n + 1);
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
#ifdef _WIN32
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
    void* frames[32];
    USHORT n = CaptureStackBackTrace(1, 32, frames, NULL);
    fprintf(stderr, "stack trace (%u frames; pipe through addr2line for source lines):\n", n);
    for (USHORT i = 0; i < n; i++) fprintf(stderr, "  #%-2u  %p\n", i, frames[i]);
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
#include <execinfo.h>
#include <unistd.h>
static void __glide_sig_handler(int sig) {
    const char* name = "unknown";
    const char* hint = "";
    if (sig == SIGSEGV) { name = "segmentation fault"; hint = "hint: dereferenced a null or invalid pointer"; }
    else if (sig == SIGFPE) { name = "floating-point / arithmetic error"; hint = "hint: division by zero or invalid float operation"; }
    else if (sig == SIGABRT) { name = "aborted"; hint = "hint: runtime panic (e.g. arena oom)"; }
    fflush(stdout);
    fprintf(stderr, "\n\x1b[1;31mfatal\x1b[0m: %s (signal %d)\n", name, sig);
    if (hint[0]) fprintf(stderr, "  \x1b[1;36m=\x1b[0m %s\n", hint);
    void* frames[32]; int n = backtrace(frames, 32);
    fprintf(stderr, "stack trace (%d frames):\n", n);
    backtrace_symbols_fd(frames, n, 2);
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



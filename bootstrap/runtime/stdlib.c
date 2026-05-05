// ---- glide runtime ----
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
static const char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f); fclose(f); buf[got] = 0;
    return buf;
}
static bool write_file(const char* path, const char* content) {
    FILE* f = fopen(path, "wb"); if (!f) return false;
    size_t n = strlen(content);
    size_t wrote = fwrite(content, 1, n, f); fclose(f);
    return wrote == n;
}
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define __glide_dup _dup
#define __glide_dup2 _dup2
#define __glide_close _close
#define __glide_fileno _fileno
#else
#include <unistd.h>
#define __glide_dup dup
#define __glide_dup2 dup2
#define __glide_close close
#define __glide_fileno fileno
#endif
static int __glide_redirect_to(const char* path) {
    fflush(stdout);
    int saved = __glide_dup(1);
    if (saved < 0) return -1;
    FILE* f = fopen(path, "w");
    if (!f) { __glide_close(saved); return -1; }
    __glide_dup2(__glide_fileno(f), 1);
    fclose(f);
    return saved;
}
static void __glide_restore_stdout(int saved) {
    fflush(stdout);
    if (saved >= 0) { __glide_dup2(saved, 1); __glide_close(saved); }
}
static int __glide_shell(const char* cmd) { return system(cmd); }
static const char* __glide_getenv(const char* name) { const char* v = getenv(name); return v ? v : ""; }
static bool __glide_file_exists(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return false; fclose(f); return true;
}
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
#ifdef _WIN32
#include <fcntl.h>
static void __glide_set_binary_io(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}
#else
static void __glide_set_binary_io(void) {}
#endif
static const char* __glide_read_line(void) {
    static char buf[8192];
    if (!fgets(buf, sizeof(buf), stdin)) { buf[0] = 0; return buf; }
    return buf;
}
static const char* __glide_read_bytes(int n) {
    if (n <= 0) return "";
    char* buf = (char*)malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, stdin);
    buf[got] = 0;
    return buf;
}
static void __glide_write_str(const char* s) {
    fputs(s, stdout);
}
static void __glide_write_bytes(const char* s, int n) {
    if (n > 0) fwrite(s, 1, (size_t)n, stdout);
}
static void __glide_flush_stdout(void) { fflush(stdout); }
static void __glide_log(const char* s) {
    fputs(s, stderr); fputc('\n', stderr); fflush(stderr);
}
static int __glide_is_windows(void) {
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}
// `__glide_list_dir`: newline-separated, sorted listing of files under
// `path` whose name ends in `suffix` (pass "" to list everything). Returns
// "" on missing dir. Same body lives in main.glide as a c_raw block so the
// compiler can bootstrap from older runtimes — guard avoids dup definition.
#ifndef GLIDE_LIST_DIR_DEFINED
#define GLIDE_LIST_DIR_DEFINED
static int __glide_strcmp_qsort(const void* a, const void* b) {
    const char* x = *(const char* const*)a;
    const char* y = *(const char* const*)b;
    return strcmp(x, y);
}
#ifdef _WIN32
static const char* __glide_list_dir(const char* path, const char* suffix) {
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    char** names = NULL; int n = 0; int cap = 0;
    size_t suf_n = suffix ? strlen(suffix) : 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* nm = fd.cFileName;
        size_t nm_n = strlen(nm);
        if (suf_n > 0 && (nm_n < suf_n || strcmp(nm + nm_n - suf_n, suffix) != 0)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = (char**)realloc(names, (size_t)cap * sizeof(char*)); }
        char* dup = (char*)malloc(nm_n + 1); memcpy(dup, nm, nm_n + 1);
        names[n++] = dup;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (n == 0) { free(names); return ""; }
    qsort(names, (size_t)n, sizeof(char*), __glide_strcmp_qsort);
    size_t total = 0; for (int i = 0; i < n; i++) total += strlen(names[i]) + 1;
    char* out = (char*)malloc(total);
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(names[i]);
        memcpy(out + off, names[i], l); off += l;
        if (i + 1 < n) out[off++] = '\n';
        free(names[i]);
    }
    out[off] = 0;
    free(names);
    return out;
}
#else
#include <dirent.h>
static const char* __glide_list_dir(const char* path, const char* suffix) {
    DIR* d = opendir(path);
    if (!d) return "";
    char** names = NULL; int n = 0; int cap = 0;
    size_t suf_n = suffix ? strlen(suffix) : 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* nm = e->d_name;
        if (nm[0] == '.') continue;
        size_t nm_n = strlen(nm);
        if (suf_n > 0 && (nm_n < suf_n || strcmp(nm + nm_n - suf_n, suffix) != 0)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = (char**)realloc(names, (size_t)cap * sizeof(char*)); }
        char* dup = (char*)malloc(nm_n + 1); memcpy(dup, nm, nm_n + 1);
        names[n++] = dup;
    }
    closedir(d);
    if (n == 0) { free(names); return ""; }
    qsort(names, (size_t)n, sizeof(char*), __glide_strcmp_qsort);
    size_t total = 0; for (int i = 0; i < n; i++) total += strlen(names[i]) + 1;
    char* out = (char*)malloc(total);
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(names[i]);
        memcpy(out + off, names[i], l); off += l;
        if (i + 1 < n) out[off++] = '\n';
        free(names[i]);
    }
    out[off] = 0;
    free(names);
    return out;
}
#endif
#endif

#ifdef _WIN32
static const char* __glide_exe_path(void) {
    static char buf[1024];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) buf[0] = 0;
    return buf;
}
static const char* __glide_exe_dir(void) {
    static char buf[1024];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) { buf[0] = 0; return buf; }
    for (DWORD i = n; i > 0; i--) { if (buf[i-1] == '\\' || buf[i-1] == '/') { buf[i-1] = 0; return buf; } }
    return "";
}
#else
static const char* __glide_exe_path(void) {
    static char buf[1024];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) { buf[0] = 0; return buf; }
    buf[n] = 0;
    return buf;
}
static const char* __glide_exe_dir(void) {
    static char buf[1024];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) { buf[0] = 0; return buf; }
    buf[n] = 0;
    for (ssize_t i = n; i > 0; i--) { if (buf[i-1] == '/') { buf[i-1] = 0; return buf; } }
    return "";
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



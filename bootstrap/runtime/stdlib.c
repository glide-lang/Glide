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
static int __glide_string_len(const char* s) { return (int)strlen(s); }
static bool __glide_string_eq(const char* a, const char* b) { return strcmp(a, b) == 0; }
static char __glide_string_at(const char* s, int i) { return s[i]; }
static const char* __glide_string_concat(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* out = (char*)__glide_palloc((int)(la + lb + 1));
    memcpy(out, a, la); memcpy(out + la, b, lb); out[la + lb] = 0;
    return out;
}
static const char* __glide_string_substring(const char* s, int start, int end) {
    int n = (int)strlen(s);
    if (start < 0) start = 0;
    if (end > n) end = n;
    if (start > end) start = end;
    int len = end - start;
    char* out = (char*)__glide_palloc(len + 1);
    memcpy(out, s + start, (size_t)len); out[len] = 0;
    return out;
}
/* Take a raw byte buffer + length and produce a NUL-terminated Glide
   string in one allocation. Used by the HTTP server to skip the
   per-byte concat that would otherwise allocate `n` strings to
   convert one read buffer. */
const char* __glide_string_from_buf(void* buf, int n) {
    if (n < 0) n = 0;
    // Arena-aware: when an arena is active the result is reclaimed at
    // arena drop; otherwise __glide_palloc falls back to calloc and the
    // pointer is safely freed by __glide_pfree at scope exit. Using raw
    // malloc here detaches the string from the lifecycle Glide expects
    // and leaks N+1 bytes per call.
    char* out = (char*)__glide_palloc(n + 1);
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
    char* out = (char*)__glide_palloc(2);
    out[0] = c; out[1] = 0;
    return out;
}
/* 64-bit so every int width (i8..i64, isize) shares one routine; narrower
   types promote losslessly and the result is assigned back to the caller's
   own type. */
static long long __glide_int_abs(long long n) { return n < 0 ? -n : n; }
static const char* __glide_int_to_string(long long n) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", n);
    char* out = (char*)__glide_palloc(len + 1);
    memcpy(out, buf, (size_t)len + 1);
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
    char* out = (char*)__glide_palloc(t + 1);
    for (int i = 0; i < t; i++) out[i] = tmp[t - 1 - i];
    out[t] = '\0';
    return out;
}
static const char* __glide_i128_to_string(__int128 n) {
    if (n < 0) {
        unsigned __int128 m = (unsigned __int128)(-(n + 1)) + 1;
        const char* s = __glide_u128_to_string(m);
        size_t len = strlen(s);
        char* out = (char*)__glide_palloc(len + 2);
        out[0] = '-';
        memcpy(out + 1, s, len + 1);
        return out;
    }
    return __glide_u128_to_string((unsigned __int128)n);
}
static __int128 __glide_i128_abs(__int128 n) { return n < 0 ? -n : n; }
static unsigned __int128 __glide_u128_abs(unsigned __int128 n) { return n; }
static const char* __glide_bool_to_string(bool b) { return b ? "true" : "false"; }
#include <stdarg.h>
static const char* __glide_format(const char* fmt, ...) {
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    int n = vsnprintf(NULL, 0, fmt, ap1);
    va_end(ap1);
    char* out = (char*)__glide_palloc(n + 1);
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



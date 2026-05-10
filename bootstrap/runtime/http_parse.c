#ifndef HP_PARSE_C
#define HP_PARSE_C

#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { HP_INCOMPLETE = 0, HP_OK = 1, HP_ERROR = -1 };

typedef struct {
    const char* name;  size_t name_len;
    const char* value; size_t value_len;
} hp_header_t;

typedef struct {
    const char* method;   size_t method_len;
    const char* path;     size_t path_len;
    int         version;          /* 10 = HTTP/1.0, 11 = HTTP/1.1 */
    hp_header_t headers[64];
    int         n_headers;
    const char* body;
    size_t      body_len;
    size_t      total_consumed;
} hp_request_t;

static inline int hp_parse_request(const char* buf, size_t len, hp_request_t* out);

static inline int hp_is_tchar(unsigned char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }

    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return 1;
    default:
        return 0;
    }
}

static inline int hp_is_header_name(const char* p, size_t n)
{
    size_t i;

    if (n == 0) {
        return 0;
    }

    for (i = 0; i < n; i++) {
        if (!hp_is_tchar((unsigned char)p[i])) {
            return 0;
        }
    }

    return 1;
}

static inline int hp_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (int)(c - 'A' + 'a') : (int)c;
}

static inline int hp_name_eq(const char* p, size_t n, const char* lit)
{
    size_t i;

    for (i = 0; i < n && lit[i] != '\0'; i++) {
        if (hp_lower((unsigned char)p[i]) != hp_lower((unsigned char)lit[i])) {
            return 0;
        }
    }

    return i == n && lit[i] == '\0';
}

static inline const char* hp_find_crlf(const char* p, const char* end)
{
    while (p < end) {
        if (*p == '\n') {
            return 0;
        }
        if (*p == '\r') {
            if (p + 1 == end) {
                return 0;
            }
            return p[1] == '\n' ? p : 0;
        }
        p++;
    }

    return 0;
}

static inline int hp_line_needs_more(const char* p, const char* end)
{
    while (p < end) {
        if (*p == '\n') {
            return 0;
        }
        if (*p == '\r') {
            return p + 1 == end;
        }
        p++;
    }

    return 1;
}

static inline int hp_parse_content_length(const char* p, size_t n, size_t* value)
{
    size_t i = 0;
    size_t v = 0;
    size_t max = (size_t)-1;

    while (i < n && (p[i] == ' ' || p[i] == '\t')) {
        i++;
    }

    if (i == n || p[i] < '0' || p[i] > '9') {
        return 0;
    }

    for (; i < n && p[i] >= '0' && p[i] <= '9'; i++) {
        size_t d = (size_t)(p[i] - '0');
        if (v > (max - d) / 10) {
            return 0;
        }
        v = v * 10 + d;
    }

    while (i < n && (p[i] == ' ' || p[i] == '\t')) {
        i++;
    }

    if (i != n) {
        return 0;
    }

    *value = v;
    return 1;
}

static inline int hp_parse_request(const char* buf, size_t len, hp_request_t* out)
{
    const char* end = buf + len;
    const char* p = buf;
    const char* line_end;
    const char* sp1;
    const char* sp2;
    hp_request_t req;
    size_t content_length = 0;
    int have_content_length = 0;
    int i;

    if (!buf || !out) {
        return HP_ERROR;
    }

    req.method = 0;
    req.method_len = 0;
    req.path = 0;
    req.path_len = 0;
    req.version = 0;
    req.n_headers = 0;
    req.body = 0;
    req.body_len = 0;
    req.total_consumed = 0;

    line_end = hp_find_crlf(p, end);
    if (!line_end) {
        return hp_line_needs_more(p, end) ? HP_INCOMPLETE : HP_ERROR;
    }

    sp1 = p;
    while (sp1 < line_end && *sp1 != ' ') {
        sp1++;
    }
    if (sp1 == p || sp1 == line_end) {
        return HP_ERROR;
    }
    for (i = 0; p + i < sp1; i++) {
        if (!hp_is_tchar((unsigned char)p[i])) {
            return HP_ERROR;
        }
    }

    sp2 = sp1 + 1;
    while (sp2 < line_end && *sp2 != ' ') {
        sp2++;
    }
    if (sp2 == sp1 + 1 || sp2 == line_end || sp2 + 9 != line_end) {
        return HP_ERROR;
    }

    if (sp2[1] != 'H' || sp2[2] != 'T' || sp2[3] != 'T' ||
        sp2[4] != 'P' || sp2[5] != '/' ||
        (sp2[6] != '1' || sp2[7] != '.' ||
         (sp2[8] != '0' && sp2[8] != '1'))) {
        return HP_ERROR;
    }

    req.method = p;
    req.method_len = (size_t)(sp1 - p);
    req.path = sp1 + 1;
    req.path_len = (size_t)(sp2 - (sp1 + 1));
    req.version = sp2[8] == '0' ? 10 : 11;
    p = line_end + 2;

    while (1) {
        const char* colon;
        const char* value;
        const char* value_end;
        size_t name_len;
        size_t value_len;

        line_end = hp_find_crlf(p, end);
        if (!line_end) {
            return hp_line_needs_more(p, end) ? HP_INCOMPLETE : HP_ERROR;
        }

        if (line_end == p) {
            p = line_end + 2;
            break;
        }

        if (*p == ' ' || *p == '\t') {
            return HP_ERROR;
        }

        colon = p;
        while (colon < line_end && *colon != ':') {
            colon++;
        }
        if (colon == line_end) {
            return HP_ERROR;
        }

        name_len = (size_t)(colon - p);
        if (!hp_is_header_name(p, name_len)) {
            return HP_ERROR;
        }

        if (req.n_headers >= 64) {
            return HP_ERROR;
        }

        value = colon + 1;
        while (value < line_end && (*value == ' ' || *value == '\t')) {
            value++;
        }
        value_end = line_end;
        while (value_end > value &&
               (value_end[-1] == ' ' || value_end[-1] == '\t')) {
            value_end--;
        }
        value_len = (size_t)(value_end - value);

        req.headers[req.n_headers].name = p;
        req.headers[req.n_headers].name_len = name_len;
        req.headers[req.n_headers].value = value;
        req.headers[req.n_headers].value_len = value_len;
        req.n_headers++;

        if (hp_name_eq(p, name_len, "Content-Length")) {
            size_t parsed;
            if (!hp_parse_content_length(value, value_len, &parsed)) {
                return HP_ERROR;
            }
            if (have_content_length && parsed != content_length) {
                return HP_ERROR;
            }
            have_content_length = 1;
            content_length = parsed;
        }

        p = line_end + 2;
    }

    if ((size_t)(end - p) < content_length) {
        return HP_INCOMPLETE;
    }

    req.body = p;
    req.body_len = content_length;
    req.total_consumed = (size_t)(p - buf) + content_length;
    *out = req;
    return HP_OK;
}

/* ---------- Glide-callable adapter ----------------------------------- */

/* Owning struct returned to Glide. The five string fields are
   nul-terminated heap copies — Glide treats `string` as `const char*`
   and never inspects the storage, so plain malloc'd buffers are fine.
   Caller frees via hp_glide_free once the request is consumed. */
typedef struct {
    const char* method;
    const char* path;
    const char* version;       /* "HTTP/1.0" or "HTTP/1.1" */
    const char* headers_block; /* "Name: Value\r\n..." (no trailing \r\n) */
    const char* body;
    int         total_consumed;
} hp_glide_t;

static char* hp__nul_dup(const char* p, size_t n) {
    char* out = (char*)malloc(n + 1);
    if (n > 0) memcpy(out, p, n);
    out[n] = 0;
    return out;
}

/* Signatures use void* so they line up with what Glide's `*void` extern
   decl emits. The function bodies cast back internally.

   Single-allocation layout: one malloc holds both the hp_glide_t header
   and all five string copies back-to-back. Drops 7 mallocs/request to 1
   (perf showed malloc was 12 % inclusive of CPU on the hello-world hot
   path, almost all of it from here). The parse scratch lives in TLS so
   we don't allocate it per-call — safe because each worker thread runs
   only one coro at a time and the data is consumed before the next
   ctx switch (we copy out of it before returning). */
static __thread hp_request_t hp_tls_req;

void* hp_parse_glide(void* buf, int len)
{
    if (!buf || len <= 0) return NULL;
    hp_request_t* r = &hp_tls_req;
    if (hp_parse_request((const char*)buf, (size_t)len, r) != HP_OK) {
        return NULL;
    }

    /* Validate field lengths to prevent integer overflow and heap buffer overrun. */
    if (r->method_len > 64 || r->path_len > 8192 || r->body_len > (1 << 20))
        return NULL;
    for (int i = 0; i < r->n_headers; i++) {
        if (r->headers[i].name_len > 256 || r->headers[i].value_len > 8192)
            return NULL;
    }

    /* Compute the header block size first; the rest are known directly. */
    size_t hsz = 0;
    for (int i = 0; i < r->n_headers; i++) {
        hsz += r->headers[i].name_len + 2 + r->headers[i].value_len + 2;
    }
    if (hsz >= 2) hsz -= 2;  /* strip the trailing CRLF */

    size_t total = sizeof(hp_glide_t)
                 + r->method_len + 1
                 + r->path_len   + 1
                 + 8             + 1   /* "HTTP/1.x" */
                 + hsz           + 1
                 + r->body_len   + 1;

    char* mem = (char*)malloc(total);
    hp_glide_t* g = (hp_glide_t*)mem;
    char* p = mem + sizeof(hp_glide_t);

    g->method = p;
    if (r->method_len > 0) memcpy(p, r->method, r->method_len);
    p[r->method_len] = 0; p += r->method_len + 1;

    g->path = p;
    if (r->path_len > 0) memcpy(p, r->path, r->path_len);
    p[r->path_len] = 0; p += r->path_len + 1;

    g->version = p;
    memcpy(p, (r->version == 11) ? "HTTP/1.1" : "HTTP/1.0", 8);
    p[8] = 0; p += 9;

    g->headers_block = p;
    {
        size_t off = 0;
        for (int i = 0; i < r->n_headers; i++) {
            memcpy(p + off, r->headers[i].name, r->headers[i].name_len);
            off += r->headers[i].name_len;
            p[off++] = ':'; p[off++] = ' ';
            memcpy(p + off, r->headers[i].value, r->headers[i].value_len);
            off += r->headers[i].value_len;
            p[off++] = '\r'; p[off++] = '\n';
        }
        if (off >= 2) off -= 2;
        p[off] = 0;
        p += hsz + 1;
    }

    g->body = p;
    if (r->body_len > 0) memcpy(p, r->body, r->body_len);
    p[r->body_len] = 0;

    g->total_consumed = (int)r->total_consumed;
    return g;
}

const char* hp_glide_method(void* g)        { return ((hp_glide_t*)g)->method; }
const char* hp_glide_path(void* g)          { return ((hp_glide_t*)g)->path; }
const char* hp_glide_version(void* g)       { return ((hp_glide_t*)g)->version; }
const char* hp_glide_headers_block(void* g) { return ((hp_glide_t*)g)->headers_block; }
const char* hp_glide_body(void* g)          { return ((hp_glide_t*)g)->body; }
int         hp_glide_consumed(void* g)      { return ((hp_glide_t*)g)->total_consumed; }

/* True when the parsed request asks the server to close the conn.
   HTTP/1.1 keeps it alive by default, HTTP/1.0 closes by default.
   Direct C scan of the parsed headers — bypasses Glide's
   `req.header(...).to_lower()` chain that was costing ~12 % of CPU
   on the hot path (each call mallocs four times). */
int hp_glide_wants_close(void* gp)
{
    hp_glide_t* g = (hp_glide_t*)gp;
    /* The TLS hp_request_t was overwritten on the next parse, so we
       inspect the rebuilt headers_block directly. */
    const char* p = g->headers_block;
    int default_close = (g->version[7] == '0'); /* HTTP/1.0 */
    int found_close = 0, found_keep = 0;

    while (*p) {
        /* Match "Connection:" case-insensitive, anchored at line start. */
        const char* k = "connection";
        const char* q = p;
        int matched = 1;
        for (int i = 0; i < 10; i++) {
            unsigned char c = (unsigned char)*q;
            if (c == 0) { matched = 0; break; }
            unsigned char l = (c >= 'A' && c <= 'Z') ? (c | 0x20) : c;
            if (l != (unsigned char)k[i]) { matched = 0; break; }
            q++;
        }
        if (matched && *q == ':') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            /* Look for "close" / "keep-alive" tokens in the value, up to CRLF. */
            const char* v = q;
            while (*v && *v != '\r' && *v != '\n') v++;
            for (const char* s = q; s < v; s++) {
                /* simple substring scan */
                if (v - s >= 5) {
                    char c0 = s[0]; if (c0 >= 'A' && c0 <= 'Z') c0 |= 0x20;
                    if (c0 == 'c' && (s + 5 <= v)) {
                        char b1=s[1],b2=s[2],b3=s[3],b4=s[4];
                        if ((b1|0x20)=='l' && (b2|0x20)=='o' && (b3|0x20)=='s' && (b4|0x20)=='e') {
                            found_close = 1;
                        }
                    }
                    if (c0 == 'k' && (s + 10 <= v)) {
                        if ((s[1]|0x20)=='e' && (s[2]|0x20)=='e' && (s[3]|0x20)=='p'
                         &&  s[4]=='-' && (s[5]|0x20)=='a' && (s[6]|0x20)=='l'
                         && (s[7]|0x20)=='i' && (s[8]|0x20)=='v' && (s[9]|0x20)=='e') {
                            found_keep = 1;
                        }
                    }
                }
            }
            break;
        }
        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (found_close) return 1;
    if (default_close && !found_keep) return 1;
    return 0;
}

void hp_glide_free(void* gp)
{
    /* All strings live in the same allocation as the header now, so a
       single free reclaims everything. */
    if (gp) free(gp);
}

/* Reusable response buffer per OS thread. _handle_conn writes the
   wire bytes directly into here, then writes from this buf to the
   socket — no Glide string concat chain (was ~10 mallocs per
   response in the previous implementation). */
static __thread char*  hp_resp_buf = NULL;
static __thread size_t hp_resp_cap = 0;

enum { HP_DATE_LEN = 29 };

static atomic_uchar hp_date_cache[HP_DATE_LEN] = {
    'T', 'h', 'u', ',', ' ', '0', '1', ' ', 'J', 'a',
    'n', ' ', '1', '9', '7', '0', ' ', '0', '0', ':',
    '0', '0', ':', '0', '0', ' ', 'G', 'M', 'T'
};
static atomic_llong hp_date_epoch = 0;
static atomic_int hp_date_lock = 0;

static int hp_gmtime(time_t now, struct tm* out)
{
#ifdef _WIN32
    return gmtime_s(out, &now) == 0;
#else
    return gmtime_r(&now, out) != NULL;
#endif
}

static void hp_date_lock_acquire(void)
{
    int expected;

    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak_explicit(&hp_date_lock, &expected, 1,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            return;
        }
        while (atomic_load_explicit(&hp_date_lock, memory_order_relaxed) != 0) {
        }
    }
}

static void hp_date_lock_release(void)
{
    atomic_store_explicit(&hp_date_lock, 0, memory_order_release);
}

int hp_get_date(char* out_buf)
{
    time_t now = time(NULL);
    long long sec = (long long)now;
    int i;

    if (atomic_load_explicit(&hp_date_epoch, memory_order_acquire) != sec) {
        hp_date_lock_acquire();
        if (atomic_load_explicit(&hp_date_epoch, memory_order_relaxed) != sec) {
            struct tm tm_utc;
            char tmp[32];

            if (hp_gmtime(now, &tm_utc) &&
                strftime(tmp, sizeof(tmp), "%a, %d %b %Y %H:%M:%S GMT", &tm_utc) == HP_DATE_LEN) {
                for (i = 0; i < HP_DATE_LEN; i++) {
                    atomic_store_explicit(&hp_date_cache[i], (unsigned char)tmp[i],
                                          memory_order_relaxed);
                }
                atomic_store_explicit(&hp_date_epoch, sec, memory_order_release);
            }
        }
        hp_date_lock_release();
    }

    for (i = 0; i < HP_DATE_LEN; i++) {
        out_buf[i] = (char)atomic_load_explicit(&hp_date_cache[i], memory_order_relaxed);
    }
    return HP_DATE_LEN;
}

static int hp_itoa(char* p, int n) {
    if (n == 0) { p[0] = '0'; return 1; }
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    char tmp[12]; int k = 0;
    while (n > 0) { tmp[k++] = (char)('0' + (n % 10)); n /= 10; }
    int off = 0;
    if (neg) p[off++] = '-';
    for (int i = k - 1; i >= 0; i--) p[off++] = tmp[i];
    return off;
}

/* Build the response wire bytes into the per-thread buffer. status_txt
   is NUL-terminated. body is the byte buffer (NUL-terminated since it
   came from Glide's `string`). extra_headers may be NULL. Returns the
   total byte count written; the caller hands `hp_resp_buf` straight
   to write(2) without copying. */
int hp_build_response(int status, const char* status_txt,
                      void* body, int body_len,
                      const char* extra_headers, int keep_alive)
{
    size_t need = 64
                + (status_txt ? strlen(status_txt) : 2)
                + (extra_headers ? strlen(extra_headers) : 0)
                + (size_t)body_len + 32
                + 8 + HP_DATE_LEN;
    if (need > hp_resp_cap) {
        size_t cap = hp_resp_cap ? hp_resp_cap : 1024;
        while (cap < need) cap *= 2;
        free(hp_resp_buf);
        hp_resp_buf = (char*)malloc(cap);
        hp_resp_cap = cap;
    }
    char* p = hp_resp_buf;
    /* "HTTP/1.1 " */
    memcpy(p, "HTTP/1.1 ", 9); p += 9;
    /* status code */
    p += hp_itoa(p, status);
    *p++ = ' ';
    /* status text */
    if (status_txt) {
        size_t l = strlen(status_txt);
        memcpy(p, status_txt, l); p += l;
    } else {
        memcpy(p, "OK", 2); p += 2;
    }
    memcpy(p, "\r\nContent-Length: ", 18); p += 18;
    p += hp_itoa(p, body_len);
    if (keep_alive) {
        memcpy(p, "\r\nConnection: keep-alive\r\n", 26); p += 26;
    } else {
        memcpy(p, "\r\nConnection: close\r\n", 21); p += 21;
    }
    memcpy(p, "Date: ", 6); p += 6;
    p += hp_get_date(p);
    memcpy(p, "\r\n", 2); p += 2;
    if (extra_headers) {
        size_t l = strlen(extra_headers);
        if (l > 0) {
            memcpy(p, extra_headers, l); p += l;
            /* extra_headers does NOT carry a trailing \r\n; add it
               only if the block isn't already CRLF-terminated. */
            if (l < 2 || p[-2] != '\r' || p[-1] != '\n') {
                memcpy(p, "\r\n", 2); p += 2;
            }
        }
    }
    memcpy(p, "\r\n", 2); p += 2;
    if (body_len > 0) { memcpy(p, body, body_len); p += body_len; }
    return (int)(p - hp_resp_buf);
}

/* Hand the per-thread response buffer back to Glide as a (void*, len)
   pair. The buffer is owned by the runtime — Glide must not free it. */
void* hp_resp_buf_ptr(void) { return (void*)hp_resp_buf; }

#endif /* HP_PARSE_C */

#ifdef HP_PARSE_TEST
#include <assert.h>
#include <string.h>

static void hp_test_simple_get(void)
{
    const char* s = "GET /x HTTP/1.1\r\nHost: example\r\n\r\n";
    hp_request_t r;

    assert(hp_parse_request(s, strlen(s), &r) == HP_OK);
    assert(r.method_len == 3 && memcmp(r.method, "GET", 3) == 0);
    assert(r.path_len == 2 && memcmp(r.path, "/x", 2) == 0);
    assert(r.version == 11);
    assert(r.n_headers == 1);
    assert(r.headers[0].name_len == 4);
    assert(r.headers[0].value_len == 7);
    assert(r.body_len == 0);
    assert(r.total_consumed == strlen(s));
}

static void hp_test_post_body(void)
{
    const char* s = "POST /submit HTTP/1.0\r\nContent-Length: 5\r\n\r\nhelloextra";
    hp_request_t r;

    assert(hp_parse_request(s, strlen(s), &r) == HP_OK);
    assert(r.version == 10);
    assert(r.body_len == 5);
    assert(memcmp(r.body, "hello", 5) == 0);
    assert(r.total_consumed == strlen(s) - 5);
}

static void hp_test_incomplete(void)
{
    const char* partial_header = "GET / HTTP/1.1\r\nHost: x\r\n\r";
    const char* partial_body = "POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nabc";
    hp_request_t r;

    assert(hp_parse_request(partial_header, strlen(partial_header), &r) ==
           HP_INCOMPLETE);
    assert(hp_parse_request(partial_body, strlen(partial_body), &r) ==
           HP_INCOMPLETE);
}

static void hp_test_errors(void)
{
    hp_request_t r;
    char many[1400];
    size_t n = 0;
    int i;

    assert(hp_parse_request("GET / HTTP/1.2\r\n\r\n",
                            strlen("GET / HTTP/1.2\r\n\r\n"), &r) ==
           HP_ERROR);
    assert(hp_parse_request("GET / HTTP/1.1\n\n",
                            strlen("GET / HTTP/1.1\n\n"), &r) ==
           HP_ERROR);
    assert(hp_parse_request("GET / HTTP/1.1\r\nContent-Length: x\r\n\r\n",
                            strlen("GET / HTTP/1.1\r\nContent-Length: x\r\n\r\n"),
                            &r) == HP_ERROR);

    memcpy(many + n, "GET / HTTP/1.1\r\n", 16);
    n += 16;
    for (i = 0; i < 65; i++) {
        many[n++] = 'X';
        many[n++] = ':';
        many[n++] = ' ';
        many[n++] = '0';
        many[n++] = '\r';
        many[n++] = '\n';
    }
    many[n++] = '\r';
    many[n++] = '\n';
    assert(hp_parse_request(many, n, &r) == HP_ERROR);
}

int main(void)
{
    hp_test_simple_get();
    hp_test_post_body();
    hp_test_incomplete();
    hp_test_errors();
    return 0;
}
#endif

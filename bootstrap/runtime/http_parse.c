#ifndef HP_PARSE_C
#define HP_PARSE_C

#include <stddef.h>

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

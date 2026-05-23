/* Coroutine-free hello-world HTTP/1.1 server. Single epoll loop in the
   caller's thread; each conn lives in an sm_conn_t pointed at from
   epoll_event.data.ptr. Response body is hardcoded so the delta vs.
   http_listen with a Glide handler returning "hello!" measures the
   coro layer's overhead. Pipelining via hp_glide_consumed + memmove;
   no spawn, no chan, no park.

   Linux only. Windows / macOS / BSD fall through to a stub that
   returns -1 until IOCP / kqueue land. */

#ifndef GLIDE_HTTP_SM_DEFINED
#define GLIDE_HTTP_SM_DEFINED

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef __linux__

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

extern void* hp_parse_glide(void* buf, int len);
extern int   hp_glide_wants_close(void* g);
extern int   hp_glide_consumed(void* g);
extern void  hp_glide_free(void* g);
extern const char* hp_glide_method(void* g);
extern const char* hp_glide_path(void* g);

/* TLS flag the runtime's park guard checks. When non-zero, __glide_park
   knows it's running inside an SM HTTP handler and aborts instead of
   falling through to cond_wait (which would hang the worker pthread). */
extern _Thread_local int __glide_in_sm_handler;

/* User-supplied @leaf handler. Called inline on the worker's epoll
   thread when a request finishes parsing. Returns the response body
   as a NUL-terminated Glide string; the SM framing emits 200 OK +
   text/plain headers around it. Set via __glide_http_sm_run_with;
   when NULL the SM falls through to the hard-coded "hello!" path. */
typedef const char* (*sm_glide_handler_t)(const char* method,
                                           const char* path);
static sm_glide_handler_t g_sm_handler = NULL;

/* Rich-API dispatch: takes the parser glob + user handler ptr +
   keep_alive flag and returns the full pre-formatted response bytes
   (status line + headers + body). Glide-side trampoline builds the
   HttpRequest from hp_glide_* getters, invokes the user's @leaf
   handler, and formats the resulting HttpResponse. */
typedef const char* (*sm_dispatch_t)(void* g, void* user_handler, int keep_alive);
static sm_dispatch_t g_sm_dispatch = NULL;
static void*         g_sm_user_handler = NULL;

#define SM_BUF_SIZE 8192

typedef enum {
    SM_READING = 0,
    SM_WRITING = 1,
    SM_CLOSED  = 2,
} sm_state_t;

typedef struct {
    int        fd;
    sm_state_t state;
    int        keep_alive;
    int        read_len;
    int        write_len;
    int        write_off;
    char       read_buf[SM_BUF_SIZE];
    char       write_buf[SM_BUF_SIZE];
} sm_conn_t;

static void sm_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static const char SM_RESP_KEEP[] =
    "HTTP/1.1 200 OK\r\n"
    "Server: glide-sm\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 6\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "hello!";
static const char SM_RESP_CLOSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Server: glide-sm\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 6\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello!";

static sm_conn_t* sm_new_conn(int fd) {
    sm_conn_t* c = (sm_conn_t*)malloc(sizeof(sm_conn_t));
    if (!c) return NULL;
    c->fd = fd;
    c->state = SM_READING;
    c->keep_alive = 0;
    c->read_len = 0;
    c->write_len = 0;
    c->write_off = 0;
    return c;
}

static void sm_close(sm_conn_t* c, int ep) {
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

/* Returns 1 if a request parsed (state -> SM_WRITING), 0 if blocked on
   EAGAIN, -1 on hard error / EOF. */
static int sm_advance_read(sm_conn_t* c) {
    while (c->read_len < SM_BUF_SIZE) {
        ssize_t n = read(c->fd, c->read_buf + c->read_len,
                         (size_t)(SM_BUF_SIZE - c->read_len));
        if (n > 0) {
            c->read_len += (int)n;
        } else if (n == 0) {
            return -1;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    if (c->read_len == 0) return 0;

    void* g = hp_parse_glide(c->read_buf, c->read_len);
    if (!g) {
        if (c->read_len >= SM_BUF_SIZE) return -1;
        return 0;
    }

    c->keep_alive = !hp_glide_wants_close(g);
    int consumed  = hp_glide_consumed(g);

    /* Rich-API dispatch: Glide-side trampoline builds HttpRequest,
       invokes the user handler, returns pre-formatted response bytes.
       C side just copies + writes. The __glide_in_sm_handler flag
       guards against handlers that try to park (the runtime's park
       path aborts with a friendly message). */
    if (g_sm_dispatch != NULL) {
        __glide_in_sm_handler = 1;
        const char* resp = g_sm_dispatch(g, g_sm_user_handler, c->keep_alive);
        __glide_in_sm_handler = 0;
        hp_glide_free(g);
        int leftover = c->read_len - consumed;
        if (leftover < 0) leftover = 0;
        if (leftover > 0 && consumed > 0) {
            memmove(c->read_buf, c->read_buf + consumed, (size_t)leftover);
        }
        c->read_len = leftover;
        if (!resp) return -1;
        int resp_len = (int)strlen(resp);
        if (resp_len > SM_BUF_SIZE) return -1;
        memcpy(c->write_buf, resp, (size_t)resp_len);
        c->write_len = resp_len;
        c->write_off = 0;
        c->state = SM_WRITING;
        return 1;
    }

    /* Fast path: hardcoded "hello!" response (the original SM bench). */
    if (g_sm_handler == NULL) {
        hp_glide_free(g);
        int leftover = c->read_len - consumed;
        if (leftover < 0) leftover = 0;
        if (leftover > 0 && consumed > 0) {
            memmove(c->read_buf, c->read_buf + consumed, (size_t)leftover);
        }
        c->read_len = leftover;
        const char* src = c->keep_alive ? SM_RESP_KEEP  : SM_RESP_CLOSE;
        int rn          = c->keep_alive ? (int)sizeof(SM_RESP_KEEP) - 1
                                        : (int)sizeof(SM_RESP_CLOSE) - 1;
        memcpy(c->write_buf, src, (size_t)rn);
        c->write_len = rn;
        c->write_off = 0;
        c->state = SM_WRITING;
        return 1;
    }

    /* Handler path: invoke the user's @leaf function inline on this
       worker's epoll thread. The handler MUST not park (no chan, no
       sleep, no I/O); if it does, the runtime will abort it via the
       __glide_park guard. */
    const char* method = hp_glide_method(g);
    const char* path   = hp_glide_path(g);
    const char* body   = g_sm_handler(method, path);
    hp_glide_free(g);

    int leftover = c->read_len - consumed;
    if (leftover < 0) leftover = 0;
    if (leftover > 0 && consumed > 0) {
        memmove(c->read_buf, c->read_buf + consumed, (size_t)leftover);
    }
    c->read_len = leftover;

    int body_len = body ? (int)strlen(body) : 0;
    /* Two pre-baked header prefixes. Skipping snprintf saves ~80 ns
       per response on this hot path. */
    static const char HDR_KEEP[]  =
        "HTTP/1.1 200 OK\r\n"
        "Server: glide-sm\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: keep-alive\r\n"
        "Content-Length: ";
    static const char HDR_CLOSE[] =
        "HTTP/1.1 200 OK\r\n"
        "Server: glide-sm\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: ";
    const char* hdr = c->keep_alive ? HDR_KEEP : HDR_CLOSE;
    int hdr_len     = c->keep_alive ? (int)sizeof(HDR_KEEP)  - 1
                                    : (int)sizeof(HDR_CLOSE) - 1;
    if (hdr_len + 12 + 4 + body_len > SM_BUF_SIZE) {
        /* response wouldn't fit — bail. */
        return -1;
    }
    memcpy(c->write_buf, hdr, (size_t)hdr_len);
    char digits[12];
    int  d = 0;
    if (body_len == 0) { digits[d++] = '0'; }
    else {
        int v = body_len;
        char tmp[12]; int t = 0;
        while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t > 0) digits[d++] = tmp[--t];
    }
    memcpy(c->write_buf + hdr_len, digits, (size_t)d);
    c->write_buf[hdr_len + d    ] = '\r';
    c->write_buf[hdr_len + d + 1] = '\n';
    c->write_buf[hdr_len + d + 2] = '\r';
    c->write_buf[hdr_len + d + 3] = '\n';
    if (body_len > 0) {
        memcpy(c->write_buf + hdr_len + d + 4, body, (size_t)body_len);
    }
    c->write_len = hdr_len + d + 4 + body_len;
    c->write_off = 0;
    c->state = SM_WRITING;
    return 1;
}

static int sm_advance_write(sm_conn_t* c) {
    while (c->write_off < c->write_len) {
        ssize_t n = write(c->fd, c->write_buf + c->write_off,
                          (size_t)(c->write_len - c->write_off));
        if (n > 0) {
            c->write_off += (int)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 1;
}

static int sm_set_events(int ep, sm_conn_t* c, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = c;
    return epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
}

static int sm_listener_bind(int port) {
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return -1;
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(listener); return -1;
    }
    if (listen(listener, 1024) < 0) {
        close(listener); return -1;
    }
    sm_set_nonblock(listener);
    return listener;
}

static int sm_worker_loop(int port) {
    int listener = sm_listener_bind(port);
    if (listener < 0) return -1;

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) { close(listener); return -1; }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, listener, &ev) < 0) {
        close(ep); close(listener); return -1;
    }

    struct epoll_event evs[256];
    for (;;) {
        int n = epoll_wait(ep, evs, 256, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            void* p = evs[i].data.ptr;
            uint32_t m = evs[i].events;
            if (p == NULL) {
                for (;;) {
                    int cfd = accept4(listener, NULL, NULL,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    int yes = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                               &yes, sizeof(yes));
                    sm_conn_t* c = sm_new_conn(cfd);
                    if (!c) { close(cfd); continue; }
                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.ptr = c;
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        close(cfd); free(c);
                    }
                }
                continue;
            }
            sm_conn_t* c = (sm_conn_t*)p;
            if (m & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                if (c->state == SM_READING) (void)sm_advance_read(c);
                sm_close(c, ep);
                continue;
            }
            int closed = 0;
            int want_out = 0;
            for (;;) {
                if (c->state == SM_READING) {
                    int r = sm_advance_read(c);
                    if (r < 0) { closed = 1; break; }
                    if (r == 0) break;
                }
                if (c->state == SM_WRITING) {
                    int w = sm_advance_write(c);
                    if (w < 0) { closed = 1; break; }
                    if (w == 0) { want_out = 1; break; }
                    if (!c->keep_alive) { closed = 1; break; }
                    c->write_len = 0; c->write_off = 0;
                    c->state = SM_READING;
                }
            }
            if (closed) { sm_close(c, ep); continue; }
            if (want_out) {
                sm_set_events(ep, c, EPOLLOUT | EPOLLRDHUP);
            } else if (c->state == SM_READING) {
                sm_set_events(ep, c, EPOLLIN | EPOLLRDHUP);
            }
        }
    }
    close(ep);
    close(listener);
    return 0;
}

int __glide_http_sm_hello_run(int port) {
    return sm_worker_loop(port);
}

typedef struct { int port; } sm_worker_arg_t;
static void* sm_worker_thread(void* arg) {
    sm_worker_arg_t* a = (sm_worker_arg_t*)arg;
    int port = a->port;
    free(a);
    (void)sm_worker_loop(port);
    return NULL;
}

int __glide_http_sm_hello_run_n(int port, int n) {
    if (n < 1) n = 1;
    int probe = sm_listener_bind(port);
    if (probe < 0) return -1;
    close(probe);
    for (int i = 1; i < n; i++) {
        sm_worker_arg_t* a = (sm_worker_arg_t*)malloc(sizeof(*a));
        a->port = port;
        pthread_t tid;
        if (pthread_create(&tid, NULL, sm_worker_thread, a) != 0) {
            free(a);
            continue;
        }
        pthread_detach(tid);
    }
    return sm_worker_loop(port);
}

/* User-handler variant of the SM server. The handler param is typed
   void* because Glide's codegen emits fn-pointer externs that way —
   the forward decl in the .__glide.c file uses `void* h` and we have
   to match it. We cast back to sm_glide_handler_t internally; the
   Glide-side type system enforced fn(string,string)->string at the
   spawn site so the underlying ABI matches.

   The handler runs inline on each worker's epoll loop. It MUST be
   @leaf (no chan / sleep / I/O); the runtime aborts the worker if it
   reaches __glide_park. */
int __glide_http_sm_run_with(int port, int n, void* h) {
    g_sm_handler = (sm_glide_handler_t)h;
    return __glide_http_sm_hello_run_n(port, n);
}

/* Rich-API entry point: dispatch is the Glide trampoline that builds
   HttpRequest, invokes user_handler, and serializes the resulting
   HttpResponse. Both pointers are void* because Glide-emitted externs
   pass fn-pointers / heap pointers that way; we cast inside. */
int __glide_http_sm_run_dispatch(int port, int n, void* dispatch, void* user_handler) {
    g_sm_dispatch     = (sm_dispatch_t)dispatch;
    g_sm_user_handler = user_handler;
    return __glide_http_sm_hello_run_n(port, n);
}

#else

int __glide_http_sm_hello_run(int port) {
    (void)port;
    return -1;
}

int __glide_http_sm_hello_run_n(int port, int n) {
    (void)port; (void)n;
    return -1;
}

int __glide_http_sm_run_with(int port, int n, void* h) {
    (void)port; (void)n; (void)h;
    return -1;
}

int __glide_http_sm_run_dispatch(int port, int n, void* dispatch, void* user) {
    (void)port; (void)n; (void)dispatch; (void)user;
    return -1;
}

#endif

#endif

// ============================ I/O reactor ================================
//
// Async wrappers for accept / read / write that register the fd with the
// kernel's readiness primitive and park the calling coroutine until the
// kernel says the fd is ready. The worker thread is then free to pick up
// another task. A single dedicated `reactor` pthread owns the poll fd
// and drives the wakeup loop.
//
// Two backends share the file:
//   * Linux  -> epoll                    (level-triggered)
//   * Apple / *BSD -> kqueue              (level-triggered)
// On Windows we fall back to the blocking sync calls in socket.c. IOCP
// is a separate epic with its own completion-based shape. The Glide-side
// API (`accept_tcp_async` / `tcp_read_async` / `tcp_write_async`) is
// platform-portable so net.glide can call the async names everywhere
// without #ifdef.
//
// `__glide_reactor_active()` lets stdlib branch on "do we have async
// I/O or not" instead of guessing by OS - on macOS/BSD we DO have a
// reactor, so http_listen's `if win { inline } else { spawn }` would
// previously deadlock workers there.

#ifndef GLIDE_REACTOR_DEFINED
#define GLIDE_REACTOR_DEFINED

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>

#ifdef __linux__
# include <sys/epoll.h>
# include <fcntl.h>
# include <unistd.h>
# define GLIDE_REACTOR_USE_EPOLL 1
# define GLIDE_REACTOR_HAVE_REACTOR 1
# if defined(GLIDE_HAS_IO_URING) || __has_include(<liburing.h>)
#  include <liburing.h>
#  define GLIDE_REACTOR_HAS_IO_URING 1
# endif
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
# include <sys/types.h>
# include <sys/event.h>
# include <sys/time.h>
# include <fcntl.h>
# include <unistd.h>
# define GLIDE_REACTOR_USE_KQUEUE 1
# define GLIDE_REACTOR_HAVE_REACTOR 1
#elif defined(_WIN32)
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0601
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# include <mswsock.h>
# include <windows.h>
# define GLIDE_REACTOR_USE_IOCP 1
# define GLIDE_REACTOR_HAVE_REACTOR 1
#endif

/* Forward declarations from sched.c. */
struct __glide_task;
extern __thread struct __glide_task* __glide_cur_task;
extern int  __glide_park(pthread_mutex_t* lock, struct __glide_task** list);
extern void __glide_unpark_one(struct __glide_task** list);
extern void __glide_flush_main_buf(void);
extern void __glide_park_blocked(void);
extern void __glide_unpark_task(struct __glide_task* t);
extern void __glide_q_push_to(int wid, struct __glide_task* t);

/* Whether async I/O parking is wired on this build. The Glide side
   branches on this in `http_listen` to fall back to inline serial when
   the reactor isn't available (Windows today, BSD without us, etc).
   IMPORTANT: returns 0 when the IOCP backend is compiled with the
   sync-fallback toggle (no real async happens), so http_listen keeps
   handling conns inline instead of spawning workers that would block
   on sync recv. */
/* IOCP is built in but currently routed through sync fallbacks while
   the real WSARecv/WSASend path is being stabilized. Until that's done
   keep reporting "no async reactor" on Windows so http_listen runs
   inline (workers would otherwise block on the sync recv they get
   routed to). Flip the 0 below to 1 once tcp_read/write_async run
   real OVERLAPPED ops. */
#define __GLIDE_RUNTIME_HAS_REACTOR_ACTIVE 1
int __glide_reactor_active(void) {
#if defined(GLIDE_REACTOR_USE_EPOLL) || defined(GLIDE_REACTOR_USE_KQUEUE)
    return 1;
#elif defined(GLIDE_REACTOR_USE_IOCP)
    /* IOCP overlapped path isn't stable yet (parked reads never complete),
       so report "no reactor" — http_listen then runs inline serially with
       the blocking sync I/O selected by __GLIDE_IOCP_SYNC_FALLBACK. */
    return 0;
#else
    return 0;
#endif
}

/* ======================== io_uring backend ============================
 *
 * Linux 5.1+ completion-based I/O. submit (SQE) carries the buffer +
 * task pointer; the kernel performs the operation and posts a CQE
 * with the result. A single reactor thread drains the CQ and unparks
 * the matching task. Compared to epoll (readiness model) this saves
 * the syscall back into recv/write after the wake and lets the kernel
 * batch ops via SQPOLL — in steady state HTTP load there are zero
 * io_uring_enter syscalls because the kernel polls the SQ on its own
 * pthread. Enabled opt-in via GLIDE_REACTOR=uring; epoll stays the
 * default until io_uring proves stable across the suite.
 */
#ifdef GLIDE_REACTOR_HAS_IO_URING

static struct io_uring   __glide_ring;
static atomic_int        __glide_uring_ready = 0;
static __glide_spin_t    __glide_uring_sqe_lock = 0;
static pthread_t         __glide_uring_thr;
static atomic_int        __glide_uring_running = 0;

static void* __glide_uring_loop(void* arg) {
    (void)arg;
    while (atomic_load_explicit(&__glide_uring_running, memory_order_acquire)) {
        struct io_uring_cqe* cqe = NULL;
        /* io_uring_wait_cqe blocks until a CQE arrives or signal. */
        int r = io_uring_wait_cqe(&__glide_ring, &cqe);
        if (r < 0) {
            if (r == -EINTR) continue;
            break;
        }
        struct __glide_task* t = (struct __glide_task*)io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&__glide_ring, cqe);
        if (t) {
            t->io_result = res;
            __glide_unpark_task(t);
        }
    }
    return NULL;
}

static pthread_once_t __glide_uring_init_once = PTHREAD_ONCE_INIT;
static void __glide_uring_init_inner(void) {
    const char* env = getenv("GLIDE_REACTOR");
    if (!env || strcmp(env, "uring") != 0) {
        atomic_store_explicit(&__glide_uring_ready, -1, memory_order_release);
        return;
    }
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    /* Try SQPOLL first (kernel polls SQ → zero syscalls). On kernels
       or container configs that block IORING_SETUP_SQPOLL (needs
       CAP_SYS_NICE on <5.13), fall back to plain mode where each
       submit costs one io_uring_enter syscall. */
    const char* nosq = getenv("GLIDE_URING_NOSQPOLL");
    if (!nosq) {
        p.flags = IORING_SETUP_SQPOLL;
        p.sq_thread_idle = 2000;
    }
    int r = io_uring_queue_init_params(4096, &__glide_ring, &p);
    if (r < 0) {
        memset(&p, 0, sizeof(p));
        r = io_uring_queue_init_params(4096, &__glide_ring, &p);
        if (r < 0) {
            atomic_store_explicit(&__glide_uring_ready, -1, memory_order_release);
            return;
        }
    }
    atomic_store_explicit(&__glide_uring_running, 1, memory_order_release);
    pthread_create(&__glide_uring_thr, NULL, __glide_uring_loop, NULL);
    pthread_detach(__glide_uring_thr);
    atomic_store_explicit(&__glide_uring_ready, 1, memory_order_release);
}

static inline int __glide_uring_active(void) {
    int v = atomic_load_explicit(&__glide_uring_ready, memory_order_acquire);
    if (v == 0) {
        pthread_once(&__glide_uring_init_once, __glide_uring_init_inner);
        v = atomic_load_explicit(&__glide_uring_ready, memory_order_acquire);
    }
    return v > 0;
}

/* Submit an SQE + park current coro until completion. Returns the
   cqe->res (bytes count for recv/send, fd for accept, -errno on err).
   Returns -1 if not in a coro or SQE/submit fails. */
static int __glide_uring_submit_wait(void (*prep)(struct io_uring_sqe*, void*),
                                      void* prep_ctx) {
    struct __glide_task* t = __glide_cur_task;
    if (!t) return -1;
    __glide_spin_lock(&__glide_uring_sqe_lock);
    struct io_uring_sqe* sqe = io_uring_get_sqe(&__glide_ring);
    if (!sqe) {
        __glide_spin_unlock(&__glide_uring_sqe_lock);
        return -1;
    }
    prep(sqe, prep_ctx);
    io_uring_sqe_set_data(sqe, t);
    /* SQPOLL: kernel polls; this becomes a no-op. Non-SQPOLL: one
       io_uring_enter syscall. Either way we batch in flight. */
    io_uring_submit(&__glide_ring);
    __glide_spin_unlock(&__glide_uring_sqe_lock);
    t->io_result = 0;
    __glide_park_blocked();
    return t->io_result;
}

struct __glide_uring_recv_ctx { int fd; void* buf; unsigned len; };
static void __glide_uring_prep_recv(struct io_uring_sqe* sqe, void* p) {
    struct __glide_uring_recv_ctx* c = (struct __glide_uring_recv_ctx*)p;
    io_uring_prep_recv(sqe, c->fd, c->buf, c->len, 0);
}
struct __glide_uring_send_ctx { int fd; const void* buf; unsigned len; };
static void __glide_uring_prep_send(struct io_uring_sqe* sqe, void* p) {
    struct __glide_uring_send_ctx* c = (struct __glide_uring_send_ctx*)p;
    io_uring_prep_send(sqe, c->fd, c->buf, c->len, MSG_NOSIGNAL);
}
struct __glide_uring_accept_ctx { int fd; };
static void __glide_uring_prep_accept(struct io_uring_sqe* sqe, void* p) {
    struct __glide_uring_accept_ctx* c = (struct __glide_uring_accept_ctx*)p;
    io_uring_prep_accept(sqe, c->fd, NULL, NULL, 0);
}

#endif /* GLIDE_REACTOR_HAS_IO_URING */

#if defined(GLIDE_REACTOR_USE_EPOLL) || defined(GLIDE_REACTOR_USE_KQUEUE)

/* Per-fd waiter state. Stored in the kernel event's user-data slot so
   the reactor thread can recover it on a wakeup without a separate
   lookup. Two wait lists per fd because read and write may park
   independently. The lock is a spinlock because the critical sections
   - link or unlink one task on the wait list - are 5-10 ns. */
typedef struct __glide_io_waiter {
    int fd;
    __glide_spin_t spin;
    struct __glide_task* read_waiters;
    struct __glide_task* write_waiters;
    int registered;             /* 1 once added to reactor */
} __glide_io_waiter;

extern int  __glide_spin_park(__glide_spin_t* lock, struct __glide_task** list);

/* Tiny open-addressing fd -> waiter map. fds in a long-running server
   reuse low numbers, so a flat array is plenty (and faster than a
   hashmap). Grows on demand. */
static __glide_io_waiter** __glide_waiters = NULL;
static int                 __glide_waiters_cap = 0;
static pthread_mutex_t     __glide_waiters_mu = PTHREAD_MUTEX_INITIALIZER;

static int                 __glide_reactor_fd = -1;   /* epoll or kqueue fd */
static pthread_t           __glide_reactor_thr;
static atomic_int          __glide_reactor_inited = 0;
static atomic_int          __glide_reactor_running = 0;

static __glide_io_waiter* __glide_io_get_or_create(int fd) {
    pthread_mutex_lock(&__glide_waiters_mu);
    if (fd >= __glide_waiters_cap) {
        int new_cap = __glide_waiters_cap ? __glide_waiters_cap : 64;
        while (new_cap <= fd) new_cap *= 2;
        __glide_waiters = (__glide_io_waiter**)realloc(
            __glide_waiters, sizeof(__glide_io_waiter*) * (size_t)new_cap);
        for (int i = __glide_waiters_cap; i < new_cap; i++) {
            __glide_waiters[i] = NULL;
        }
        __glide_waiters_cap = new_cap;
    }
    __glide_io_waiter* w = __glide_waiters[fd];
    if (!w) {
        w = (__glide_io_waiter*)calloc(1, sizeof(__glide_io_waiter));
        w->fd = fd;
        __glide_waiters[fd] = w;
    }
    pthread_mutex_unlock(&__glide_waiters_mu);
    return w;
}

static void __glide_io_register(__glide_io_waiter* w) {
    if (w->registered) return;
#ifdef GLIDE_REACTOR_USE_EPOLL
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN | EPOLLOUT | EPOLLRDHUP;   /* level-triggered */
    ev.data.ptr = w;
    if (epoll_ctl(__glide_reactor_fd, EPOLL_CTL_ADD, w->fd, &ev) == 0) {
        w->registered = 1;
    } else if (errno == EEXIST) {
        w->registered = 1;
    }
#elif defined(GLIDE_REACTOR_USE_KQUEUE)
    /* kqueue needs separate kevent entries for READ and WRITE filters.
       Default is level-triggered (matches epoll). udata carries the
       waiter ptr the reactor loop dereferences on wakeup. */
    struct kevent ch[2];
    EV_SET(&ch[0], w->fd, EVFILT_READ,  EV_ADD, 0, 0, w);
    EV_SET(&ch[1], w->fd, EVFILT_WRITE, EV_ADD, 0, 0, w);
    if (kevent(__glide_reactor_fd, ch, 2, NULL, 0, NULL) >= 0) {
        w->registered = 1;
    } else if (errno == EEXIST) {
        w->registered = 1;
    }
#endif
}

static void* __glide_reactor_loop(void* arg) {
    (void)arg;
#ifdef GLIDE_REACTOR_USE_EPOLL
    struct epoll_event evs[256];
    while (atomic_load(&__glide_reactor_running)) {
        int n = epoll_wait(__glide_reactor_fd, evs, 256, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            __glide_io_waiter* w = (__glide_io_waiter*)evs[i].data.ptr;
            if (!w) continue;
            uint32_t m = evs[i].events;
            /* Skip the lock entirely when there is nobody to wake. The
               read carries a torn-list risk for one direction, but a
               level-triggered fd that still has data ready will trip
               the next poll cycle anyway, so a missed wake here just
               defers by one tick. */
            int rd_ready = (m & (EPOLLIN  | EPOLLERR | EPOLLHUP | EPOLLRDHUP))
                            && w->read_waiters;
            int wr_ready = (m & (EPOLLOUT | EPOLLERR | EPOLLHUP))
                            && w->write_waiters;
            if (!rd_ready && !wr_ready) continue;
            __glide_spin_lock(&w->spin);
            if (rd_ready) {
                while (w->read_waiters)  __glide_unpark_one(&w->read_waiters);
            }
            if (wr_ready) {
                while (w->write_waiters) __glide_unpark_one(&w->write_waiters);
            }
            __glide_spin_unlock(&w->spin);
        }
    }
#elif defined(GLIDE_REACTOR_USE_KQUEUE)
    struct kevent evs[256];
    struct timespec ts;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    while (atomic_load(&__glide_reactor_running)) {
        int n = kevent(__glide_reactor_fd, NULL, 0, evs, 256, &ts);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            __glide_io_waiter* w = (__glide_io_waiter*)evs[i].udata;
            if (!w) continue;
            int is_read  = (evs[i].filter == EVFILT_READ);
            int is_write = (evs[i].filter == EVFILT_WRITE);
            int eof_err  = (evs[i].flags & (EV_EOF | EV_ERROR)) != 0;
            int rd_ready = (is_read  || eof_err) && w->read_waiters;
            int wr_ready = (is_write || eof_err) && w->write_waiters;
            if (!rd_ready && !wr_ready) continue;
            __glide_spin_lock(&w->spin);
            if (rd_ready) {
                while (w->read_waiters)  __glide_unpark_one(&w->read_waiters);
            }
            if (wr_ready) {
                while (w->write_waiters) __glide_unpark_one(&w->write_waiters);
            }
            __glide_spin_unlock(&w->spin);
        }
    }
#endif
    return NULL;
}

static void __glide_reactor_ensure(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&__glide_reactor_inited,
                                        &expected, 1)) {
        while (__glide_reactor_fd < 0) { /* spin until other thread inits */ }
        return;
    }
#ifdef GLIDE_REACTOR_USE_EPOLL
    __glide_reactor_fd = epoll_create1(EPOLL_CLOEXEC);
#elif defined(GLIDE_REACTOR_USE_KQUEUE)
    __glide_reactor_fd = kqueue();
    if (__glide_reactor_fd >= 0) {
        /* kqueue() takes no flags; set CLOEXEC after the fact. */
        int fl = fcntl(__glide_reactor_fd, F_GETFD, 0);
        if (fl >= 0) fcntl(__glide_reactor_fd, F_SETFD, fl | FD_CLOEXEC);
    }
#endif
    if (__glide_reactor_fd < 0) {
        atomic_store(&__glide_reactor_inited, 0);
        return;
    }
    atomic_store(&__glide_reactor_running, 1);
    pthread_create(&__glide_reactor_thr, NULL, __glide_reactor_loop, NULL);
    pthread_detach(__glide_reactor_thr);
}

static void __glide_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Tear down our reactor state for `fd` before the caller calls close().
   The kernel implicitly drops a closed fd from the interest list, but
   we cache `registered` per-waiter - without this hook a fd that gets
   recycled to a new connection would never be re-added to the reactor,
   and the next read/write park on it would block forever. Also wake any
   coros still parked on the fd so they don't sit on a dead handle. */
void __glide_io_close(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&__glide_waiters_mu);
    __glide_io_waiter* w = (fd < __glide_waiters_cap) ? __glide_waiters[fd] : NULL;
    pthread_mutex_unlock(&__glide_waiters_mu);
    if (!w) return;
    __glide_spin_lock(&w->spin);
    if (w->registered && __glide_reactor_fd >= 0) {
#ifdef GLIDE_REACTOR_USE_EPOLL
        epoll_ctl(__glide_reactor_fd, EPOLL_CTL_DEL, fd, NULL);
#elif defined(GLIDE_REACTOR_USE_KQUEUE)
        struct kevent ch[2];
        EV_SET(&ch[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
        EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(__glide_reactor_fd, ch, 2, NULL, 0, NULL);
#endif
        w->registered = 0;
    }
    while (w->read_waiters)  __glide_unpark_one(&w->read_waiters);
    while (w->write_waiters) __glide_unpark_one(&w->write_waiters);
    __glide_spin_unlock(&w->spin);
}

static int __glide_io_park_read(int fd) {
    __glide_reactor_ensure();
    __glide_io_waiter* w = __glide_io_get_or_create(fd);
    __glide_spin_lock(&w->spin);
    __glide_io_register(w);
    return __glide_spin_park(&w->spin, &w->read_waiters);
}

static int __glide_io_park_write(int fd) {
    __glide_reactor_ensure();
    __glide_io_waiter* w = __glide_io_get_or_create(fd);
    __glide_spin_lock(&w->spin);
    __glide_io_register(w);
    return __glide_spin_park(&w->spin, &w->write_waiters);
}

/* ---- public async wrappers --------------------------------------- */

/* __glide_tcp_nodelay is `static` in socket.c. Both files share the
   same translation unit (codegen concats them) so we can call directly,
   but reactor.c is included after socket.c - no forward decl needed. */

int accept_tcp_async(int listener) {
#ifdef GLIDE_REACTOR_HAS_IO_URING
    if (__glide_uring_active() && __glide_cur_task) {
        struct __glide_uring_accept_ctx c = { listener };
        int fd = __glide_uring_submit_wait(__glide_uring_prep_accept, &c);
        if (fd >= 0) {
            __glide_set_nonblocking(fd);
            __glide_tcp_nodelay(fd);
        }
        return fd;
    }
#endif
    __glide_set_nonblocking(listener);
    while (1) {
        int c = accept(listener, NULL, NULL);
        if (c >= 0) {
            __glide_set_nonblocking(c);
            __glide_tcp_nodelay(c);
            return c;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!__glide_io_park_read(listener)) {
                /* not in a coro - flush any pending main-spawned coros so
                   they can run on the workers while main blocks here, then
                   fall back to blocking accept. */
                __glide_flush_main_buf();
                int flags = fcntl(listener, F_GETFL, 0);
                fcntl(listener, F_SETFL, flags & ~O_NONBLOCK);
                int c2 = accept(listener, NULL, NULL);
                fcntl(listener, F_SETFL, flags);
                if (c2 >= 0) {
                    __glide_set_nonblocking(c2);
                    __glide_tcp_nodelay(c2);
                }
                return c2;
            }
            continue;
        }
        return -1;
    }
}

/* tcp_*_async assume the fd was made non-blocking when it was accepted
   (accept_tcp_async does that once). Re-running fcntl(F_GETFL)+fcntl(F_SETFL)
   on every call costs two syscalls per read/write - measurable on the
   keep-alive hot path. */
int tcp_read_async(int fd, void* buf, int max) {
#ifdef GLIDE_REACTOR_HAS_IO_URING
    if (__glide_uring_active() && __glide_cur_task) {
        struct __glide_uring_recv_ctx c = { fd, buf, (unsigned)max };
        int n = __glide_uring_submit_wait(__glide_uring_prep_recv, &c);
        return n;
    }
#endif
    while (1) {
        int n = (int)read(fd, buf, (size_t)max);
        if (n >= 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!__glide_io_park_read(fd)) {
                /* Not on a coro worker (e.g. a raw spawn_thread pthread): there
                   is no reactor to park on, so block this OS thread on a
                   blocking read, exactly like accept_tcp_async's fallback.
                   Without this the call returns -1 on the first EAGAIN, which
                   breaks any socket loop run on a plain thread (e.g. the mail
                   tests' fake servers). */
                __glide_flush_main_buf();
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                int n2 = (int)read(fd, buf, (size_t)max);
                fcntl(fd, F_SETFL, flags);
                return n2;
            }
            continue;
        }
        return -1;
    }
}

int tcp_write_async(int fd, void* buf, int n) {
#ifdef GLIDE_REACTOR_HAS_IO_URING
    if (__glide_uring_active() && __glide_cur_task) {
        int sent = 0;
        while (sent < n) {
            struct __glide_uring_send_ctx c = { fd, (const char*)buf + sent, (unsigned)(n - sent) };
            int w = __glide_uring_submit_wait(__glide_uring_prep_send, &c);
            if (w > 0) { sent += w; continue; }
            return sent > 0 ? sent : (w < 0 ? -1 : sent);
        }
        return sent;
    }
#endif
    int sent = 0;
    while (sent < n) {
        int w = (int)write(fd, (const char*)buf + sent, (size_t)(n - sent));
        if (w > 0) { sent += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (!__glide_io_park_write(fd)) {
                /* Raw thread, no reactor to park on — block on a blocking
                   write for this chunk (mirrors tcp_read_async / accept). */
                __glide_flush_main_buf();
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                int w2 = (int)write(fd, (const char*)buf + sent, (size_t)(n - sent));
                fcntl(fd, F_SETFL, flags);
                if (w2 > 0) { sent += w2; continue; }
                return sent > 0 ? sent : -1;
            }
            continue;
        }
        return sent > 0 ? sent : -1;
    }
    return sent;
}

/* Async writev across two iovecs; same parking discipline as
   tcp_write_async. The HTTP server uses this to ship the response
   header + body in one syscall without first memcpy'ing the body
   into a combined buffer. */
int tcp_writev2_async(int fd, void* buf1, int n1, void* buf2, int n2) {
    if (n1 < 0) n1 = 0;
    if (n2 < 0) n2 = 0;
    int total = n1 + n2;
    if (total == 0) return 0;
    struct iovec iov[2];
    iov[0].iov_base = buf1; iov[0].iov_len = (size_t)n1;
    iov[1].iov_base = buf2; iov[1].iov_len = (size_t)n2;
    int sent = 0;
    while (sent < total) {
        struct iovec cur[2];
        int n_cur = 0;
        size_t skip = (size_t)sent;
        for (int i = 0; i < 2; i++) {
            if (iov[i].iov_len == 0) continue;
            if (skip >= iov[i].iov_len) { skip -= iov[i].iov_len; continue; }
            cur[n_cur].iov_base = (char*)iov[i].iov_base + skip;
            cur[n_cur].iov_len  = iov[i].iov_len - skip;
            n_cur++;
            skip = 0;
        }
        ssize_t w = writev(fd, cur, n_cur);
        if (w > 0) { sent += (int)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (!__glide_io_park_write(fd)) return sent > 0 ? sent : -1;
            continue;
        }
        return sent > 0 ? sent : -1;
    }
    return sent;
}

#elif defined(GLIDE_REACTOR_USE_IOCP)

/* ===================== Windows IOCP backend =====================
 *
 * Completion-based rather than readiness-based. Each async op carries
 * its own OVERLAPPED + waiter pointer; a single dedicated reactor
 * thread blocks on GetQueuedCompletionStatus and wakes the parked
 * coro when the kernel delivers a completion.
 *
 * The op state is heap-allocated (not stack) because the OVERLAPPED
 * must outlive the synchronous return from the WSARecv / WSASend call
 * - if the issuing coro is parked on a coro stack, the kernel still
 * writes through ov on completion, and we need that memory valid
 * until the reactor thread has consumed the entry.
 *
 * Sprint W1.2 scope:
 *   - tcp_read_async via WSARecv + IOCP
 *   - tcp_write_async via WSASend + IOCP
 *   - tcp_writev2_async via WSASend (gather) + IOCP
 *   - accept_tcp_async still blocks (AcceptEx is W2)
 *   - close path clears the assoc bitmap so reused fds reassociate
 */

typedef struct __glide_iocp_op {
    OVERLAPPED          ov;         /* MUST be first - cast from OVERLAPPED* */
    struct __glide_task* waiter;
    atomic_int          done;
    DWORD               bytes;
    DWORD               err;
} __glide_iocp_op;

static HANDLE              __glide_iocp = NULL;
static atomic_int          __glide_reactor_inited = 0;
static atomic_int          __glide_reactor_running = 0;
static int                 __glide_wsa_inited_iocp = 0;

/* fd -> "has been associated with IOCP" bitmap. Cleared on tcp_close
   so a reused fd re-associates the next time an async op fires. */
static unsigned char*      __glide_iocp_assoc_bits = NULL;
static int                 __glide_iocp_assoc_cap = 0;
static __glide_spin_t      __glide_iocp_assoc_spin = 0;

static void __glide_wsa_ensure_iocp(void) {
    if (__glide_wsa_inited_iocp) return;
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    __glide_wsa_inited_iocp = 1;
}

static DWORD WINAPI __glide_reactor_loop_iocp(LPVOID arg) {
    (void)arg;
    while (atomic_load(&__glide_reactor_running)) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(__glide_iocp, &bytes,
                                            &key, &ov, 1000);
        if (ov == NULL) continue;  /* timeout or unrelated wake */
        __glide_iocp_op* op = (__glide_iocp_op*)ov;
        op->bytes = bytes;
        op->err   = ok ? 0 : GetLastError();
        atomic_store_explicit(&op->done, 1, memory_order_release);
        if (op->waiter != NULL) {
            __glide_unpark_task(op->waiter);
        }
    }
    return 0;
}

static void __glide_reactor_ensure_iocp(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&__glide_reactor_inited,
                                        &expected, 1)) {
        while (__glide_iocp == NULL) { Sleep(0); }
        return;
    }
    __glide_wsa_ensure_iocp();
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (port == NULL) {
        atomic_store(&__glide_reactor_inited, 0);
        return;
    }
    __glide_iocp = port;
    atomic_store(&__glide_reactor_running, 1);
    HANDLE h = CreateThread(NULL, 0,
        __glide_reactor_loop_iocp, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

static void __glide_iocp_associate(SOCKET s) {
    int fd = (int)s;
    if (fd < 0) return;
    __glide_spin_lock(&__glide_iocp_assoc_spin);
    if (fd >= __glide_iocp_assoc_cap) {
        int new_cap = __glide_iocp_assoc_cap ? __glide_iocp_assoc_cap : 1024;
        while (new_cap <= fd) new_cap *= 2;
        __glide_iocp_assoc_bits = (unsigned char*)realloc(
            __glide_iocp_assoc_bits, (size_t)new_cap);
        for (int i = __glide_iocp_assoc_cap; i < new_cap; i++) {
            __glide_iocp_assoc_bits[i] = 0;
        }
        __glide_iocp_assoc_cap = new_cap;
    }
    int already = __glide_iocp_assoc_bits[fd];
    __glide_iocp_assoc_bits[fd] = 1;
    __glide_spin_unlock(&__glide_iocp_assoc_spin);
    if (already) return;
    CreateIoCompletionPort((HANDLE)s, __glide_iocp, (ULONG_PTR)s, 0);
}

void __glide_io_close(int fd) {
    if (fd < 0) return;
    __glide_spin_lock(&__glide_iocp_assoc_spin);
    if (fd < __glide_iocp_assoc_cap) {
        __glide_iocp_assoc_bits[fd] = 0;
    }
    __glide_spin_unlock(&__glide_iocp_assoc_spin);
}

/* Park current coro until op->done is set by the reactor. Falls back
   to Sleep(0) spin if invoked outside a coro (main thread or
   spawn_thread). The reactor writes op->done with release ordering;
   we read it with acquire. */
static void __glide_iocp_park(__glide_iocp_op* op) {
    if (__glide_cur_task == NULL) {
        while (!atomic_load_explicit(&op->done, memory_order_acquire)) {
            Sleep(0);
        }
        return;
    }
    /* Spin briefly in case the op completed synchronously - cheaper
       than a full ctx_switch for sub-microsecond ops. */
    for (int i = 0; i < 256; i++) {
        if (atomic_load_explicit(&op->done, memory_order_acquire)) return;
    }
    __glide_park_blocked();
    /* On wake the reactor has already set op->done. */
}

extern int  accept_tcp(int listener);
extern int  tcp_read(int fd, void* buf, int max);
extern int  tcp_write(int fd, void* buf, int n);
extern int  tcp_writev2(int fd, void* buf1, int n1, void* buf2, int n2);

/* IOCP async (WSARecv/WSASend + parked coro) is not yet stable: a parked
   read never completes, so an http_listen server accepts the connection but
   never sends a response. Until the overlapped path is fixed, fall back to
   blocking sync recv/send for every op and pair this with
   __glide_reactor_active() returning 0 on Windows, so http_listen runs the
   accept loop inline serially instead of spawning workers that would block on
   the sync recv. Flip back to 0 (and reactor_active to 1) once the IOCP path
   is verified end-to-end. */
#define __GLIDE_IOCP_SYNC_FALLBACK 1
#define __GLIDE_IOCP_TRACE 0
static void __glide_iocp_trace(const char* tag, int fd, int n) {
#if __GLIDE_IOCP_TRACE
    char line[128];
    int len = snprintf(line, sizeof(line),
        "[iocp %s] fd=%d n=%d t=%p\n", tag, fd, n,
        (void*)__glide_cur_task);
    if (len > 0) {
        DWORD wrote = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE),
                  line, (DWORD)len, &wrote, NULL);
    }
#else
    (void)tag; (void)fd; (void)n;
#endif
}

int accept_tcp_async(int listener) {
    /* AcceptEx wired in Sprint W2. For W1.2 we block on accept(),
       which pins the calling thread (typically the http_listen accept
       loop running on its own spawn_thread pthread) but doesn't affect
       per-conn read/write throughput. */
    return accept_tcp(listener);
}

int tcp_read_async(int fd, void* buf, int max) {
#if __GLIDE_IOCP_SYNC_FALLBACK
    return tcp_read(fd, buf, max);
#else
    __glide_iocp_trace("read_enter", fd, max);
    __glide_reactor_ensure_iocp();
    __glide_iocp_associate((SOCKET)fd);
    __glide_iocp_op op;
    memset(&op, 0, sizeof(op));
    op.waiter = __glide_cur_task;
    WSABUF wbuf;
    wbuf.buf = (CHAR*)buf;
    wbuf.len = (ULONG)max;
    DWORD got = 0, flags = 0;
    int rv = WSARecv((SOCKET)fd, &wbuf, 1, &got, &flags, &op.ov, NULL);
    int werr = (rv != 0) ? WSAGetLastError() : 0;
    __glide_iocp_trace("read_wsarecv", fd, werr);
    if (rv != 0 && werr != WSA_IO_PENDING) return -1;
    __glide_iocp_park(&op);
    __glide_iocp_trace("read_unparked", fd, (int)op.bytes);
    if (op.err != 0) return -1;
    return (int)op.bytes;
#endif
}

int tcp_write_async(int fd, void* buf, int n) {
#if __GLIDE_IOCP_SYNC_FALLBACK
    return tcp_write(fd, buf, n);
#else
    __glide_reactor_ensure_iocp();
    __glide_iocp_associate((SOCKET)fd);
    int sent = 0;
    while (sent < n) {
        __glide_iocp_op op;
        memset(&op, 0, sizeof(op));
        op.waiter = __glide_cur_task;
        WSABUF wbuf;
        wbuf.buf = (CHAR*)buf + sent;
        wbuf.len = (ULONG)(n - sent);
        DWORD wrote = 0;
        int rv = WSASend((SOCKET)fd, &wbuf, 1, &wrote, 0, &op.ov, NULL);
        if (rv != 0) {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) return sent > 0 ? sent : -1;
        }
        __glide_iocp_park(&op);
        if (op.err != 0) return sent > 0 ? sent : -1;
        if (op.bytes == 0) break;
        sent += (int)op.bytes;
    }
    return sent;
#endif
}

int tcp_writev2_async(int fd, void* buf1, int n1, void* buf2, int n2) {
#if __GLIDE_IOCP_SYNC_FALLBACK
    return tcp_writev2(fd, buf1, n1, buf2, n2);
#else
    if (n1 < 0) n1 = 0;
    if (n2 < 0) n2 = 0;
    if (n1 + n2 == 0) return 0;
    __glide_reactor_ensure_iocp();
    __glide_iocp_associate((SOCKET)fd);
    __glide_iocp_op op;
    memset(&op, 0, sizeof(op));
    op.waiter = __glide_cur_task;
    WSABUF wbufs[2];
    DWORD nbuf = 0;
    if (n1 > 0) { wbufs[nbuf].buf = (CHAR*)buf1; wbufs[nbuf].len = (ULONG)n1; nbuf++; }
    if (n2 > 0) { wbufs[nbuf].buf = (CHAR*)buf2; wbufs[nbuf].len = (ULONG)n2; nbuf++; }
    DWORD wrote = 0;
    int rv = WSASend((SOCKET)fd, wbufs, nbuf, &wrote, 0, &op.ov, NULL);
    if (rv != 0) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) return -1;
    }
    __glide_iocp_park(&op);
    if (op.err != 0) return -1;
    return (int)op.bytes;
#endif
}

#else  /* no reactor on this OS: sync I/O fallback. */

/* Forward decls from socket.c so the names link clean. */
extern int  accept_tcp(int listener);
extern int  tcp_read(int fd, void* buf, int max);
extern int  tcp_write(int fd, void* buf, int n);
extern int  tcp_writev2(int fd, void* buf1, int n1, void* buf2, int n2);

int accept_tcp_async(int listener) {
    return accept_tcp(listener);
}

int tcp_read_async(int fd, void* buf, int max) {
    return tcp_read(fd, buf, max);
}

int tcp_write_async(int fd, void* buf, int n) {
    return tcp_write(fd, buf, n);
}

int tcp_writev2_async(int fd, void* buf1, int n1, void* buf2, int n2) {
    return tcp_writev2(fd, buf1, n1, buf2, n2);
}

void __glide_io_close(int fd) { (void)fd; }

#endif  /* backend dispatch */

/* ---- foundation async wrappers (i64 fd) -------------------------------
   stdlib::net::sys binds these. They sit on the reactor's already-proven,
   fd-generic park primitives (no NEW concurrency: a plain read/write parks
   on a single wake source — the deadline double-wake the design flags is a
   later, separate concern). Correct on BOTH blocking and non-blocking fds:
     - blocking fd          : recv/send blocks, never EAGAIN, never parks.
     - non-block fd, in coro : EAGAIN -> park -> wake -> retry  (async).
     - non-block fd, no coro : EAGAIN -> temporarily block once (fallback,
                               mirrors accept_tcp_async).
   So routing every Socket read/write through these degrades gracefully:
   server conns (made non-blocking by accept_tcp_async) run async inside
   handler coroutines; everything else just blocks. On IOCP / the no-reactor
   fallback they are plain blocking recv/send (real IOCP overlap is deferred,
   per docs/net-upgrade-plan.md §4.3). */

int64_t gnet_read_async(int64_t fd64, void* buf, int max) {
#if defined(GLIDE_REACTOR_USE_EPOLL) || defined(GLIDE_REACTOR_USE_KQUEUE)
    int fd = (int)fd64;
    while (1) {
        int n = (int)recv(fd, (char*)buf, (size_t)max, 0);
        if (n >= 0) return (int64_t)n;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!__glide_io_park_read(fd)) {
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                int n2 = (int)recv(fd, (char*)buf, (size_t)max, 0);
                fcntl(fd, F_SETFL, flags);
                return (int64_t)n2;
            }
            continue;
        }
        return -1;
    }
#else
    return (int64_t)recv((__glide_sock_t)fd64, (char*)buf, max, 0);
#endif
}

int64_t gnet_write_async(int64_t fd64, void* buf, int n) {
#if defined(GLIDE_REACTOR_USE_EPOLL) || defined(GLIDE_REACTOR_USE_KQUEUE)
    int fd = (int)fd64;
    int sent = 0;
    while (sent < n) {
        int w = (int)send(fd, (const char*)buf + sent, (size_t)(n - sent), 0);
        if (w > 0) { sent += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (!__glide_io_park_write(fd)) {
                int flags = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                int w2 = (int)send(fd, (const char*)buf + sent, (size_t)(n - sent), 0);
                fcntl(fd, F_SETFL, flags);
                if (w2 > 0) { sent += w2; continue; }
                return sent > 0 ? (int64_t)sent : -1;
            }
            continue;
        }
        return sent > 0 ? (int64_t)sent : -1;
    }
    return (int64_t)sent;
#else
    int sent = 0;
    while (sent < n) {
        int w = (int)send((__glide_sock_t)fd64, (const char*)buf + sent, (n - sent), 0);
        if (w <= 0) return sent > 0 ? (int64_t)sent : -1;
        sent += w;
    }
    return (int64_t)sent;
#endif
}

/* Accept a connection through the reactor (parks the listener in a coro,
   blocks via fallback otherwise — accept_tcp_async owns that logic and also
   sets the accepted fd non-blocking + TCP_NODELAY). Fills the peer address
   via the netcore unpacker. */
int64_t gnet_accept_async(int64_t fd64, int* of, int64_t* ov4, int64_t* ohi,
                          int64_t* olo, int* op) {
    int c = accept_tcp_async((int)fd64);
    if (c < 0) return -1;
    struct sockaddr_storage ss; socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (getpeername((__glide_sock_t)c, (struct sockaddr*)&ss, &len) == 0) {
        __gnet_unpack(&ss, of, ov4, ohi, olo, op);
    }
    return (int64_t)c;
}

#endif  /* GLIDE_REACTOR_DEFINED */

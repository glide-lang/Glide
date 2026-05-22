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
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
# include <sys/types.h>
# include <sys/event.h>
# include <sys/time.h>
# include <fcntl.h>
# include <unistd.h>
# define GLIDE_REACTOR_USE_KQUEUE 1
# define GLIDE_REACTOR_HAVE_REACTOR 1
#endif

/* Forward declarations from sched.c. */
struct __glide_task;
extern __thread struct __glide_task* __glide_cur_task;
extern int  __glide_park(pthread_mutex_t* lock, struct __glide_task** list);
extern void __glide_unpark_one(struct __glide_task** list);
extern void __glide_flush_main_buf(void);

/* Whether async I/O parking is wired on this build. The Glide side
   branches on this in `http_listen` to fall back to inline serial when
   the reactor isn't available (Windows today, BSD without us, etc). */
int __glide_reactor_active(void) {
#ifdef GLIDE_REACTOR_HAVE_REACTOR
    return 1;
#else
    return 0;
#endif
}

#ifdef GLIDE_REACTOR_HAVE_REACTOR

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
    while (1) {
        int n = (int)read(fd, buf, (size_t)max);
        if (n >= 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (!__glide_io_park_read(fd)) return -1;
            continue;
        }
        return -1;
    }
}

int tcp_write_async(int fd, void* buf, int n) {
    int sent = 0;
    while (sent < n) {
        int w = (int)write(fd, (const char*)buf + sent, (size_t)(n - sent));
        if (w > 0) { sent += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (!__glide_io_park_write(fd)) return sent > 0 ? sent : -1;
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

#endif  /* GLIDE_REACTOR_HAVE_REACTOR */

#endif  /* GLIDE_REACTOR_DEFINED */

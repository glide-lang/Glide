// ============================ socket runtime =============================
#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
# include <mswsock.h>      /* TransmitFile */
# include <windows.h>      /* CreateFileA, HANDLE, GetFileSizeEx */
typedef SOCKET __glide_sock_t;
static int __glide_wsa_inited = 0;
static void __glide_wsa_ensure(void) {
    if (__glide_wsa_inited) return;
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    __glide_wsa_inited = 1;
}
#else
# include <sys/socket.h>
# include <sys/uio.h>       /* writev */
# include <sys/stat.h>      /* fstat */
# include <netinet/in.h>
# include <netinet/tcp.h>   /* TCP_NODELAY */
# include <unistd.h>
# include <fcntl.h>         /* open */
# include <arpa/inet.h>
# include <errno.h>
# ifdef __linux__
#  include <sys/sendfile.h>
# endif
typedef int __glide_sock_t;
static void __glide_wsa_ensure(void) {}
#endif

/* Whether SO_REUSEPORT on this OS actually load-balances accepts.
   Linux: yes (canonical implementation).
   FreeBSD: only with SO_REUSEPORT_LB (which we don't toggle today)
            - plain SO_REUSEPORT here is shared-bind, last-write wins.
   macOS / OpenBSD / NetBSD / DragonFly: SO_REUSEPORT exists but doesn't
   load-balance - last bind wins traffic.
   Windows: no SO_REUSEPORT at all (SO_REUSEADDR is unrelated).
   stdlib branches on this to pick between "each worker binds" (Linux
   path) and "one shared listener, N accept threads" (everyone else). */
#define __GLIDE_RUNTIME_HAS_REUSEPORT_BALANCE 1
int __glide_has_reuseport_balance(void) {
#ifdef __linux__
    return 1;
#else
    return 0;
#endif
}

/* Turn off Nagle on a freshly-accepted conn. With keep-alive +
   small responses, Nagle would otherwise hold a write back up
   to 40 ms waiting for an ACK to coalesce, dropping req/s by an
   order of magnitude under wrk. */
static void __glide_tcp_nodelay(int fd) {
#ifndef _WIN32
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
#else
    int yes = 1;
    setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
#endif
}

int listen_tcp(int port) {
    __glide_wsa_ensure();
    __glide_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#ifdef TCP_DEFER_ACCEPT
    int defer = 1;
    setsockopt(s, IPPROTO_TCP, TCP_DEFER_ACCEPT, (const char*)&defer, sizeof(defer));
#endif
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    if (listen(s, 128) < 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    return (int)s;
}

int listen_tcp_reuseport(int port) {
    __glide_wsa_ensure();
    __glide_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&yes, sizeof(yes));
#endif
#ifdef TCP_DEFER_ACCEPT
    /* Linux only. Kernel holds the accept until the client actually
       writes data (or a 1 s timeout). With this on, accept returns an
       fd that already has the request bytes ready, so the very first
       read avoids the EAGAIN → reactor park → wake round-trip. Big win
       for HTTP-style "connect, send, close" workloads. */
    int defer = 1;
    setsockopt(s, IPPROTO_TCP, TCP_DEFER_ACCEPT, (const char*)&defer, sizeof(defer));
#endif
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    if (listen(s, 128) < 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    return (int)s;
}

int accept_tcp(int listener) {
#ifdef _WIN32
    SOCKET c = accept((SOCKET)listener, NULL, NULL);
    if (c == INVALID_SOCKET) return -1;
    __glide_tcp_nodelay((int)c);
    return (int)c;
#else
    int c = accept(listener, NULL, NULL);
    if (c < 0) return -1;
    __glide_tcp_nodelay(c);
    return c;
#endif
}

int tcp_read(int fd, void* buf, int max) {
#ifdef _WIN32
    int n = recv((SOCKET)fd, (char*)buf, max, 0);
    return n < 0 ? -1 : n;
#else
    int n = (int)read(fd, buf, (size_t)max);
    return n < 0 ? -1 : n;
#endif
}

int tcp_write(int fd, void* buf, int n) {
#ifdef _WIN32
    int w = send((SOCKET)fd, (const char*)buf, n, 0);
    return w < 0 ? -1 : w;
#else
    int w = (int)write(fd, buf, (size_t)n);
    return w < 0 ? -1 : w;
#endif
}

/* Zero-copy file transfer to a socket. On Linux: sendfile() pulls
   bytes from the file's page cache straight into the socket buffer.
   On Windows: TransmitFile() does the same DMA path. On macOS / BSD:
   sendfile() with a slightly different signature. Returns total
   bytes sent, or -1 on error. */
int tcp_sendfile_from_path(int sock_fd, const char* path) {
    if (path == NULL || path[0] == 0) return -1;
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return -1; }
    BOOL ok = TransmitFile((SOCKET)sock_fd, h, (DWORD)sz.QuadPart, 0,
                            NULL, NULL, 0);
    CloseHandle(h);
    return ok ? (int)sz.QuadPart : -1;
#elif defined(__linux__)
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) return -1;
    struct stat st;
    if (fstat(file_fd, &st) != 0) { close(file_fd); return -1; }
    off_t offset = 0;
    size_t remaining = (size_t)st.st_size;
    int total = 0;
    while (remaining > 0) {
        ssize_t s = sendfile(sock_fd, file_fd, &offset, remaining);
        if (s > 0) { total += (int)s; remaining -= (size_t)s; continue; }
        if (s < 0 && errno == EINTR) continue;
        break;
    }
    close(file_fd);
    return total > 0 ? total : -1;
#else
    /* macOS / BSD: sendfile signature differs across BSD flavours;
       fall back to read+write for now. */
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) return -1;
    char buf[65536];
    int total = 0;
    for (;;) {
        ssize_t r = read(file_fd, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            close(file_fd);
            return total > 0 ? total : -1;
        }
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(sock_fd, buf + off, (size_t)(r - off));
            if (w > 0) { off += w; continue; }
            if (w < 0 && errno == EINTR) continue;
            close(file_fd);
            return total > 0 ? total : -1;
        }
        total += (int)r;
    }
    close(file_fd);
    return total;
#endif
}

/* Gather-write two buffers in one syscall (writev / WSASend). The HTTP
   server uses this so the response's header + body ship in one TCP
   segment without the kernel having to coalesce two writes, and
   without us memcpy'ing the body into a combined buffer. Returns
   total bytes sent or -1 on hard error before any byte landed. */
int tcp_writev2(int fd, void* buf1, int n1, void* buf2, int n2) {
    if (n1 < 0) n1 = 0;
    if (n2 < 0) n2 = 0;
    int total = n1 + n2;
    if (total == 0) return 0;
#ifdef _WIN32
    WSABUF bufs[2];
    bufs[0].buf = (CHAR*)buf1; bufs[0].len = (ULONG)n1;
    bufs[1].buf = (CHAR*)buf2; bufs[1].len = (ULONG)n2;
    DWORD nbufs = (DWORD)((n1 > 0 ? 1u : 0u) + (n2 > 0 ? 1u : 0u));
    int start = (n1 > 0 ? 0 : 1);
    DWORD sent = 0;
    int rv = WSASend((SOCKET)fd, &bufs[start], nbufs, &sent, 0, NULL, NULL);
    if (rv != 0) return -1;
    return (int)sent;
#else
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
        if (w < 0 && (errno == EINTR)) continue;
        return sent > 0 ? sent : -1;
    }
    return sent;
#endif
}

/* Defined in reactor.c (same translation unit). Tears down per-fd
   waiter state so a recycled fd is not seen as already-registered
   the next time tcp_read_async/write_async parks on it. */
#ifndef _WIN32
void __glide_io_close(int fd);
#endif

void tcp_close(int fd) {
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    __glide_io_close(fd);
    close(fd);
#endif
}

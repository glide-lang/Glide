// ============================ socket runtime =============================
#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
typedef SOCKET __glide_sock_t;
static int __glide_wsa_inited = 0;
static void __glide_wsa_ensure(void) {
    if (__glide_wsa_inited) return;
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    __glide_wsa_inited = 1;
}
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <netinet/tcp.h>   /* TCP_NODELAY */
# include <unistd.h>
# include <arpa/inet.h>
typedef int __glide_sock_t;
static void __glide_wsa_ensure(void) {}
#endif

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

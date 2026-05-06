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
# include <unistd.h>
# include <arpa/inet.h>
typedef int __glide_sock_t;
static void __glide_wsa_ensure(void) {}
#endif

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
    return (c == INVALID_SOCKET) ? -1 : (int)c;
#else
    int c = accept(listener, NULL, NULL);
    return c < 0 ? -1 : c;
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

void tcp_close(int fd) {
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

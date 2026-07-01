// ===================== netcore: shared socket syscall surface ===========
//
// The single owner of the BSD-socket syscalls, behind the one winsock/posix
// split already established in socket.c (emitted immediately ABOVE this file,
// same translation unit — so __glide_sock_t and __glide_wsa_ensure() are in
// scope and are NOT redeclared here). stdlib::net::sys binds these gnet_*
// symbols so tcp/udp/tls/raw/icmp/etc. become pure Glide instead of each
// re-deriving socket()/bind()/connect() in its own c_raw block.
//
// ABI: addresses travel as the flat tuple (family, v4, v6_hi, v6_lo, port):
//   family : 4 = IPv4, 6 = IPv6   (Glide tag, NOT the OS AF_* value)
//   v4     : IPv4 address as a host-order u32 carried in an i64
//   v6_hi  : bytes 0..7  of the v6 address, MSB-first, packed into an i64
//   v6_lo  : bytes 8..15 of the v6 address, MSB-first
//   port   : host-order port
// This matches the byte order the old udp.glide c_raw used (udp.glide:67-87),
// so migrated modules are wire-compatible.
//
// See docs/net-upgrade-plan.md §2.

#include <stdint.h>
#include <string.h>   /* memset, strerror */
#include <stdio.h>    /* snprintf */
#ifndef _WIN32
#include <sys/time.h> /* struct timeval (gnet_set_timeout) */
#endif

#define GNET_AF_V4 4
#define GNET_AF_V6 6

/* Glide socket-type tags -> OS SOCK_* (1=STREAM 2=DGRAM 3=RAW). */
static int __gnet_os_type(int ty) {
    if (ty == 2) return SOCK_DGRAM;
    if (ty == 3) return SOCK_RAW;
    return SOCK_STREAM;
}
static int __gnet_os_family(int family) {
    return (family == GNET_AF_V6) ? AF_INET6 : AF_INET;
}

/* Flat ABI -> sockaddr_storage. Returns the sockaddr byte length. */
static socklen_t __gnet_pack(int family, int64_t v4, int64_t v6_hi,
                             int64_t v6_lo, int port,
                             struct sockaddr_storage* out) {
    memset(out, 0, sizeof(*out));
    if (family == GNET_AF_V6) {
        struct sockaddr_in6* a = (struct sockaddr_in6*)out;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons((uint16_t)port);
        unsigned char* p = (unsigned char*)&a->sin6_addr;
        for (int i = 0; i < 8; i++) p[i]   = (unsigned char)((v6_hi >> (56 - i*8)) & 0xff);
        for (int i = 0; i < 8; i++) p[8+i] = (unsigned char)((v6_lo >> (56 - i*8)) & 0xff);
        return (socklen_t)sizeof(struct sockaddr_in6);
    }
    struct sockaddr_in* a = (struct sockaddr_in*)out;
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    a->sin_addr.s_addr = htonl((uint32_t)v4);
    return (socklen_t)sizeof(struct sockaddr_in);
}

/* sockaddr_storage -> flat ABI via out-params (any may be NULL). */
static void __gnet_unpack(const struct sockaddr_storage* sa,
                          int* of, int64_t* ov4, int64_t* ohi,
                          int64_t* olo, int* op) {
    if (sa->ss_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)sa;
        const unsigned char* p = (const unsigned char*)&a->sin6_addr;
        int64_t hi = 0, lo = 0;
        for (int i = 0; i < 8; i++) hi = (hi << 8) | p[i];
        for (int i = 0; i < 8; i++) lo = (lo << 8) | p[8+i];
        if (of)  *of  = GNET_AF_V6;
        if (ov4) *ov4 = 0;
        if (ohi) *ohi = hi;
        if (olo) *olo = lo;
        if (op)  *op  = ntohs(a->sin6_port);
        return;
    }
    const struct sockaddr_in* a = (const struct sockaddr_in*)sa;
    if (of)  *of  = GNET_AF_V4;
    if (ov4) *ov4 = (int64_t)ntohl(a->sin_addr.s_addr);
    if (ohi) *ohi = 0;
    if (olo) *olo = 0;
    if (op)  *op  = ntohs(a->sin_port);
}

/* ---- the syscall surface (external linkage: Glide externs bind these) ---- */

int64_t gnet_socket(int family, int ty, int proto) {
    __glide_wsa_ensure();
    __glide_sock_t s = socket(__gnet_os_family(family), __gnet_os_type(ty), proto);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif
    return (int64_t)s;
}

int64_t gnet_bind(int64_t fd, int family, int64_t v4, int64_t hi, int64_t lo, int port) {
    struct sockaddr_storage ss;
    socklen_t len = __gnet_pack(family, v4, hi, lo, port, &ss);
    if (bind((__glide_sock_t)fd, (struct sockaddr*)&ss, len) != 0) return -1;
    return 0;
}

int64_t gnet_connect(int64_t fd, int family, int64_t v4, int64_t hi, int64_t lo, int port) {
    struct sockaddr_storage ss;
    socklen_t len = __gnet_pack(family, v4, hi, lo, port, &ss);
    if (connect((__glide_sock_t)fd, (struct sockaddr*)&ss, len) != 0) return -1;
    return 0;
}

int64_t gnet_listen(int64_t fd, int backlog) {
    if (listen((__glide_sock_t)fd, backlog) != 0) return -1;
    return 0;
}

int64_t gnet_accept(int64_t fd, int* of, int64_t* ov4, int64_t* ohi, int64_t* olo, int* op) {
    struct sockaddr_storage ss; socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    __glide_sock_t c = accept((__glide_sock_t)fd, (struct sockaddr*)&ss, &len);
#ifdef _WIN32
    if (c == INVALID_SOCKET) return -1;
#else
    if (c < 0) return -1;
#endif
    __gnet_unpack(&ss, of, ov4, ohi, olo, op);
    return (int64_t)c;
}

int64_t gnet_send(int64_t fd, void* buf, int n) {
    int r = (int)send((__glide_sock_t)fd, (const char*)buf, n, 0);
    return (int64_t)r;
}

int64_t gnet_recv(int64_t fd, void* buf, int max) {
    int r = (int)recv((__glide_sock_t)fd, (char*)buf, max, 0);
    return (int64_t)r;
}

int64_t gnet_sendto(int64_t fd, void* buf, int n,
                    int family, int64_t v4, int64_t hi, int64_t lo, int port) {
    struct sockaddr_storage ss;
    socklen_t len = __gnet_pack(family, v4, hi, lo, port, &ss);
    int r = (int)sendto((__glide_sock_t)fd, (const char*)buf, n, 0,
                        (struct sockaddr*)&ss, len);
    return (int64_t)r;
}

int64_t gnet_recvfrom(int64_t fd, void* buf, int max,
                      int* of, int64_t* ov4, int64_t* ohi, int64_t* olo, int* op) {
    struct sockaddr_storage ss; socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    int r = (int)recvfrom((__glide_sock_t)fd, (char*)buf, max, 0,
                          (struct sockaddr*)&ss, &len);
    if (r >= 0) __gnet_unpack(&ss, of, ov4, ohi, olo, op);
    return (int64_t)r;
}

int64_t gnet_getsockname(int64_t fd, int* of, int64_t* ov4, int64_t* ohi, int64_t* olo, int* op) {
    struct sockaddr_storage ss; socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (getsockname((__glide_sock_t)fd, (struct sockaddr*)&ss, &len) != 0) return -1;
    __gnet_unpack(&ss, of, ov4, ohi, olo, op);
    return 0;
}

int64_t gnet_getpeername(int64_t fd, int* of, int64_t* ov4, int64_t* ohi, int64_t* olo, int* op) {
    struct sockaddr_storage ss; socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (getpeername((__glide_sock_t)fd, (struct sockaddr*)&ss, &len) != 0) return -1;
    __gnet_unpack(&ss, of, ov4, ohi, olo, op);
    return 0;
}

int64_t gnet_setsockopt_int(int64_t fd, int level, int opt, int val) {
    if (setsockopt((__glide_sock_t)fd, level, opt, (const char*)&val, sizeof(val)) != 0) return -1;
    return 0;
}

int64_t gnet_setsockopt_buf(int64_t fd, int level, int opt, void* buf, int len) {
    if (setsockopt((__glide_sock_t)fd, level, opt, (const char*)buf, (socklen_t)len) != 0) return -1;
    return 0;
}

/* Send + receive timeout. The timeval-vs-DWORD platform split that was
   copied into 5 stdlib modules lives here once. `ms` of 0 disables. */
int64_t gnet_set_timeout(int64_t fd, int ms) {
    __glide_sock_t s = (__glide_sock_t)fd;
#ifdef _WIN32
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    return 0;
}

int64_t gnet_shutdown(int64_t fd, int how) {
    if (shutdown((__glide_sock_t)fd, how) != 0) return -1;
    return 0;
}

void gnet_close(int64_t fd) {
    if (fd < 0) return;
#ifdef _WIN32
    closesocket((__glide_sock_t)fd);
#else
    close((__glide_sock_t)fd);
#endif
}

/* Platform-correct constant numbers, queried by tag so Glide never hardcodes
   per-OS socket-option / protocol / level values. Extend as caps land. */
int gnet_const(int which) {
    switch (which) {
        case 0:  return SOL_SOCKET;
        case 1:  return SO_REUSEADDR;
        case 2:  return IPPROTO_TCP;
        case 3:  return TCP_NODELAY;
        case 4:  return SO_BROADCAST;
#ifdef SO_REUSEPORT
        case 5:  return SO_REUSEPORT;
#else
        case 5:  return -1;
#endif
        case 6:  return SO_RCVBUF;
        case 7:  return SO_SNDBUF;
        case 8:  return IPPROTO_IP;
        case 9:  return IP_TTL;
        case 10: return IP_MULTICAST_TTL;
        case 11: return IP_MULTICAST_LOOP;
        case 12: return SO_KEEPALIVE;
        default: return -1;
    }
}

/* IPv4 multicast group membership. The network-byte-order ip_mreq packing
   (which the design flags as bug-prone if hand-rolled in Glide) lives here:
   htonl the host-order group + interface addresses once, in C where the
   struct and byte-swap intrinsics are in scope. iface_v4 == 0 means
   INADDR_ANY (the default interface). */
static int64_t __gnet_mreq(int64_t fd, int64_t group_v4, int64_t iface_v4, int add) {
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = htonl((uint32_t)group_v4);
    mreq.imr_interface.s_addr = htonl((uint32_t)iface_v4);
    int opt = add ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;
    if (setsockopt((__glide_sock_t)fd, IPPROTO_IP, opt,
                   (const char*)&mreq, sizeof(mreq)) != 0) return -1;
    return 0;
}
int64_t gnet_join_group_v4(int64_t fd, int64_t group_v4, int64_t iface_v4) {
    return __gnet_mreq(fd, group_v4, iface_v4, 1);
}
int64_t gnet_leave_group_v4(int64_t fd, int64_t group_v4, int64_t iface_v4) {
    return __gnet_mreq(fd, group_v4, iface_v4, 0);
}

/* Zero-copy file -> socket, i64-fd wrapper over socket.c's sync sendfile
   (TransmitFile on Windows / sendfile on Linux / read-write on BSD). The
   legacy async server also calls this synchronously, so there is no reactor
   parking to integrate. Returns total bytes sent (file size) or -1. */
extern int tcp_sendfile_from_path(int sock_fd, const char* path);
int64_t gnet_sendfile_from_path(int64_t fd, const char* path) {
    return (int64_t)tcp_sendfile_from_path((int)fd, path);
}

/* Live errno/WSAGetLastError snapshot as "<code>:<message>" so callers can
   machine-distinguish (e.g. EPERM) per the @handler status convention. Glide
   MUST call this immediately after a failed gnet_*, before any other syscall —
   it reads the live error, not a passed code. */
static char __gnet_errbuf[192];
const char* gnet_last_error(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    char msg[128]; msg[0] = 0;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, (DWORD)e, 0, msg, (DWORD)sizeof(msg), NULL);
    for (int i = 0; msg[i]; i++) { if (msg[i] == '\r' || msg[i] == '\n') { msg[i] = 0; break; } }
    snprintf(__gnet_errbuf, sizeof(__gnet_errbuf), "%d:%s", e, msg);
#else
    int e = errno;
    snprintf(__gnet_errbuf, sizeof(__gnet_errbuf), "%d:%s", e, strerror(e));
#endif
    return __gnet_errbuf;
}

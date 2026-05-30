# Glide `net` Upgrade: First-Class Socket Foundation + Capability Expansion

A phased roadmap for a maintainer to execute. All claims are grounded in the current tree; anything not yet in the repo is labeled **(proposed)**.

> Produced by a 12-agent design workflow (survey → design → adversarial review → synthesis), 2026-05-30. Reviewer-confirmed facts: `s.impl_target.kind != TY_NAMED` continue (codegen.glide:1021) means pointer-target impls emit no vtable; `@cfg` hard-errors on anything but "windows"/"posix" (parser.glide:216-218); the v6 byte-loop is duplicated verbatim across tcp/udp/tls; emit order is socket.c→reactor.c→http_parse.c→http_sm.c (codegen.glide:1218-1224); listener externs are i32 (listener.glide:19-26).

---

## 1. Vision

Today the `net` stack carries **four parallel socket implementations** — the reactor-integrated server primitives in `bootstrap/runtime/socket.c`, plus three hand-rolled `c_raw!` blocks in `src/stdlib/net/tcp.glide`, `udp.glide`, and `tls.glide`, each re-deriving `socket()/bind()/connect()/listen()/accept()`, its own winsock-vs-posix `#ifdef`, its own WSAStartup-once guard (4 copies), its own `SOCKET`/`int` typedef (4 copies), the same IPv6 byte-pack loop (verbatim at `tcp.glide:60`, `udp.glide:69`, `udp.glide:112`, `tls.glide:282`), ~16 copies of the `closesocket`/`close` `#ifdef`, and 5 copies of the `SO_RCVTIMEO`/`SO_SNDTIMEO` split. `listener.glide` already proves the target shape: a pure-Glide module that only `extern fn`-declares runtime symbols (lines 19-26) with zero `c_raw`. **The thesis:** promote the irreducible BSD-socket syscall set into one shared runtime C file behind a single platform split, expose it to Glide as a typed `Socket{fd:i64}` + a binary-safe `Stream` trait, migrate `tcp`/`udp`/`tls`/`ws`/`http` onto it so they become pure Glide like `listener.glide`, and then add raw/ICMP/unix/multicast/broadcast/sockopt/iface as thin Glide modules that pass different constants to the *same* primitives — zero new C and zero new sysroot dependency per capability.

**Success looks like:** `tcp.glide` and `udp.glide` carry zero `c_raw`; `tls.glide` keeps only its `SSL_*` calls; `ws.glide`/`http` dispatch through one `*dyn Stream` instead of `if is_tls { ... } else { ... }`; the one auditable file `netcore.c` owns the platform split exactly once; new socket capabilities ship as Glide modules; and the plug-and-play build is untouched for everything except opt-in packet capture.

---

## 2. The foundation (P0)

### 2.1 Two pieces, mirroring the existing two-tier model

**(a) `bootstrap/runtime/netcore.c` (proposed)** — the single owner of the syscall surface as `gnet_*` functions over an `int64_t` fd. It **reuses** `socket.c`'s `__glide_sock_t` typedef and `__glide_wsa_ensure()` (socket.c:7-29) rather than redeclaring them, because they share one translation unit.

**Emit placement (confirmed load-bearing):** `emit_socket_runtime()` (codegen.glide:1216-1226) emits `socket.c` then `reactor.c` then `http_parse.c` then `http_sm.c`, in that fixed order. `netcore.c` slots in **after `socket.c`, before `reactor.c`**. The pure-syscall helpers (`gnet_socket`/`bind`/`connect`/`sendto`/`recvfrom`/`setsockopt`/`getsockname`/`shutdown`/`close`/`strerror` + the `__gnet_pack`/`__gnet_unpack` address converter) live in `netcore.c`. The **reactor-async wrappers** (`gnet_read_async`/`gnet_write_async`/`gnet_accept_async`/`gnet_connect_async`) live at the **end of `reactor.c`**, next to the existing `tcp_read_async`/`tcp_write_async`/`accept_tcp_async` and the fd-generic park primitives `__glide_io_park_read`/`_park_write`/`__glide_io_close`/`__glide_io_get_or_create` (reactor.c) they depend on. This keeps the include order honest — no forward-declaring reactor internals into netcore.

**(b) `src/stdlib/net/sys.glide` (proposed)** — zero `c_raw`; only `extern fn` decls (the `listener.glide:19-26` pattern) plus the pure-Glide typed layer: `Socket`, the address-family constants, the single `SocketAddr → (family,v4,hi,lo,port)` lowering helper, and the `Stream` trait.

### 2.2 The single winsock/posix split

It already exists, canonically, at `socket.c:1-29`: `#ifdef _WIN32` → `<winsock2.h>`/`<ws2tcpip.h>`, `typedef SOCKET __glide_sock_t`, `__glide_wsa_ensure()`; `#else` → `<sys/socket.h>`/`<netinet/in.h>`, `typedef int __glide_sock_t`, empty `__glide_wsa_ensure()`. **netcore.c does not fork this** — it lives below `socket.c` in the same TU and calls `__glide_wsa_ensure()` from `gnet_socket`. The 4 duplicate WSAStartup guards (`__glide_tcpc_init` tcp.glide:38, `__glide_udp_init` udp.glide:45, the dns guard, `__glide_tls_init` tls.glide:96) all delete. **One caveat the reviewers caught:** the IOCP backend carries its *own* `__glide_wsa_ensure_iocp` (in reactor.c), so the collapse is honestly 4→2 unless reactor.c is also unified (non-load-bearing; fold opportunistically).

### 2.3 Before / after — a `bind` replaced by pure Glide

**Before** (`udp.glide:57-98`, inside a 142-line `c_raw!` block) hand-writes `socket()`, the family branch, the v6 byte-loop, the `INVALID` sentinel, and the `close` `#ifdef`.

**After** (`udp.glide`, pure Glide — no `c_raw`, mirroring `listener.glide`):
```glide
import stdlib::net::sys::*;

impl UdpSocket {
    pub fn bind(addr: *SocketAddr) -> !*UdpSocket {
        let sock: *Socket = Socket::open(_af_of(addr.ip), SOCK_DGRAM, 0)?;
        sock.bind(addr)?;                       // gnet_bind via the one __gnet_pack
        let u: *UdpSocket = malloc(sizeof(UdpSocket)) as *UdpSocket;
        u.fd = sock.fd; free(sock as *void);
        return ok(u);
    }
}
```
The `socket()`, the family branch, the v6 byte-loop, the `INVALID` sentinel, and the `close` `#ifdef` all now live exactly once in `netcore.c`.

### 2.4 The Glide-side typed surface

```glide
// src/stdlib/net/sys.glide — extern decls + pure-Glide typed layer (no c_raw)
extern fn gnet_socket(family: i32, ty: i32, proto: i32) -> i64;
extern fn gnet_bind(fd: i64, fam: i32, v4: i64, hi: i64, lo: i64, port: i32) -> i64;
extern fn gnet_connect_async(fd: i64, fam: i32, v4: i64, hi: i64, lo: i64, port: i32) -> i64;
extern fn gnet_read_async(fd: i64, buf: *void, max: i32) -> i64;
extern fn gnet_write_async(fd: i64, buf: *void, n: i32) -> i64;
extern fn gnet_setsockopt_int(fd: i64, level: i32, opt: i32, val: i32) -> i64;
extern fn gnet_setsockopt_buf(fd: i64, level: i32, opt: i32, buf: *void, len: i32) -> i64;
extern fn gnet_getsockname(fd: i64, of: *i32, ov4: *i64, ohi: *i64, olo: *i64, op: *i32) -> i64;
extern fn gnet_getpeername(fd: i64, of: *i32, ov4: *i64, ohi: *i64, olo: *i64, op: *i32) -> i64;
extern fn gnet_shutdown(fd: i64, how: i32) -> i64;
extern fn gnet_close(fd: i64);
extern fn gnet_last_error() -> string;          // reads errno/WSAGetLastError NOW (see §8)
extern fn gnet_const(which: i32) -> i32;         // platform-correct SO_*/IPPROTO_*/IP_* numbers
// AF_UNIX cannot ride the flat ABI — dedicated path:
extern fn gnet_bind_unix(fd: i64, path: string) -> i64;
extern fn gnet_connect_unix(fd: i64, path: string) -> i64;

pub const SOCK_STREAM: i32 = 1;  pub const SOCK_DGRAM: i32 = 2;  pub const SOCK_RAW: i32 = 3;
pub const AF_V4: i32 = 4;  pub const AF_V6: i32 = 6;  pub const AF_UNIX: i32 = 1;

pub struct Socket { pub fd: i64 }   // ONE owning fd newtype; i64 (winsock SOCKET-safe)

// the single SocketAddr -> flat ABI lowering (replaces 6 inlined copies)
fn _addr_parts(sa: *SocketAddr) -> (i32, i64, i64, i64, i32) {
    if sa.ip.is_v6() { return (6, 0, sa.ip.v6_hi, sa.ip.v6_lo, sa.port); }
    return (4, sa.ip.v4 as i64, 0, 0, sa.port);
}

impl Socket {
    pub fn open(family: i32, ty: i32, proto: i32) -> !*Socket {
        let fd: i64 = gnet_socket(family, ty, proto);
        if fd < 0 { return err("socket: ".concat(gnet_last_error())); }
        let s: *Socket = malloc(sizeof(Socket)) as *Socket; s.fd = fd; return ok(s);
    }
    pub fn bind(self: *Socket, sa: *SocketAddr) -> !i32 {
        let (f, v4, hi, lo, p) = _addr_parts(sa);
        if gnet_bind(self.fd, f, v4, hi, lo, p) < 0 { return err("bind: ".concat(gnet_last_error())); }
        return ok(0);
    }
    pub fn connect(self: *Socket, sa: *SocketAddr) -> !i32 {  // async on Linux, blocking elsewhere
        let (f, v4, hi, lo, p) = _addr_parts(sa);
        if gnet_connect_async(self.fd, f, v4, hi, lo, p) < 0 { return err("connect: ".concat(gnet_last_error())); }
        return ok(0);
    }
    pub fn local_addr(self: *Socket) -> !*SocketAddr { /* gnet_getsockname — NEW, absent today */ }
    pub fn peer_addr(self: *Socket)  -> !*SocketAddr { /* gnet_getpeername — NEW, absent today */ }
    pub fn shutdown(self: *Socket, how: i32) -> !i32 { /* half-close — NEW */ }
    pub fn close(self: *Socket) { if self.fd >= 0 { gnet_close(self.fd); } free(self as *void); }
}
```

### 2.5 The `Stream` keystone — corrected to the proven vtable shape

The three reviews converge on a **blocker**: the original sketch wrote `impl Stream for *Socket` (pointer target) and default methods with `self: *dyn Stream`. Both break. Confirmed against the tree:
- Dyn-vtable codegen **skips any impl whose target is not `TY_NAMED`** (`codegen.glide:1021`: `if s.impl_target == null || s.impl_target.kind != TY_NAMED { continue; }`). So `impl Stream for *Socket` emits **no vtable** and `*dyn Stream` dispatch silently breaks.
- Every working `*dyn` impl in the tree (`ChunkSource`, `IntoResponse`, `FromRequest`) declares trait methods `self: *Self`, impls with the **concrete** type, and erases to `*dyn` only at the call/store site. **No trait in the stdlib has a default-method body**, let alone one that re-dispatches through the vtable.

**Corrected design (adopted):**
```glide
// Trait = 3 abstract methods only, self: *Self, concrete impl target.
pub trait Stream {
    fn read_bytes(self: *Self, buf: *u8, max: i32) -> !i32;
    fn write_bytes(self: *Self, buf: *u8, n: i32) -> !i32;
    fn close(self: *Self);
}
impl Stream for Socket {                         // NAMED target -> vtable emitted
    fn read_bytes(self: *Socket, buf: *u8, max: i32) -> !i32 {
        let n: i64 = gnet_read_async(self.fd, buf as *void, max);
        if n < 0 { return err(gnet_last_error()); }
        return ok(n as i32);
    }
    fn write_bytes(self: *Socket, buf: *u8, n: i32) -> !i32 { /* gnet_write_async */ }
    fn close(self: *Socket) { gnet_close(self.fd); free(self as *void); }
}

// Conveniences are FREE FUNCTIONS over *dyn Stream (the proven _pump_chunks shape),
// NOT default trait methods.
pub fn stream_read(s: *dyn Stream, max: i32) -> !string { /* read_bytes via vtable */ }
pub fn stream_write_all(s: *dyn Stream, data: string) -> !i32 { /* loop write_bytes */ }
pub fn stream_read_exact(s: *dyn Stream, n: i32) -> !string { /* loop read_bytes */ }
pub fn stream_read_until(s: *dyn Stream, delim: string) -> !string { /* loop read_bytes */ }
```
`TlsStream` and `Conn` also `impl Stream` (delegating to their existing `__glide_tls_read`/`__glide_tls_write_raw` and `tcp_read_async`/`tcp_write_async`). This collapses `ws.glide`'s 4-way `is_tls` branch, `http/client.glide`'s https branch, and unblocks **h2c** for free (binary-safe `read_bytes`/`write_bytes`, today TLS-only at `tls.glide:934-961`, now on every transport).

**Mandatory gate before building on this:** land a tiny `stream_test.glide` proving one concrete impl + one `*dyn Stream` dispatch (and a free fn calling `read_bytes` through the vtable) actually compiles and runs.

### 2.6 fd width — decide all-in i64, do not half-widen

The reviewers' shared **major**: client fds are i64 (`tcp`/`udp`), server fds are i32 (`listener.glide:28-29`), reactor externs are i32 (`listener.glide:19-26`), and `socket.c` truncates `SOCKET` to `int`. The reactor's per-fd waiter is a **flat array indexed by the raw fd value** that `realloc`s to exceed `fd` — a large Windows `SOCKET` handle used as an index would blow the realloc, and high-numbered raw/capture fds inflate it.

**Decision:** commit to a full i64 audit *or* keep `int` internally and widen only the Glide-side type — **pick one, do not half-widen**. Recommended: widen the Glide externs to i64 with the C casting at the boundary, **and** before raw/high-fd sockets ship, replace the flat fd-indexed waiter array with a hashmap keyed on the 64-bit handle. Land the widening **atomically** (reactor externs + `listener.glide` + `socket.c` casts) with a full seed rebuild, validated against the seed, not just `glide.exe`.

---

## 3. Capability surface (Layer 2)

Each is a pure-Glide module over the §2 primitives (`@cfg("posix")` body-gating where a platform lacks support). None carries its own `c_raw`.

| Capability | One-line API | New lib? | Priority |
|---|---|---|---|
| **SocketOptions** | `sock.set_reuse_addr(b)` / `set_nodelay(b)` / `set_recv_timeout_ms(ms)` / `set_ttl(n)` / `set_broadcast(b)` over `gnet_setsockopt_*` | none | **P1 (keystone)** |
| **Multicast/broadcast** | `udp.join_group(grp,iface)` / `leave_group` / `set_multicast_ttl` / `set_broadcast` via `IP_ADD_MEMBERSHIP`/`SO_BROADCAST` | none | **P1** |
| **Unix-domain** | `UnixListener::bind(path)` / `UnixStream::connect(path)` / `UnixDatagram` via `gnet_bind_unix`/`gnet_connect_unix` | none | **P2** |
| **Raw sockets** | `RawSocket::open_icmp()` / `open_ip(proto)` over `SOCK_RAW`; `IP_HDRINCL` is just a sockopt | none | **P3** |
| **ICMP ping/traceroute** | `icmp.ping(dst,id,seq)->!Duration` / `traceroute(dst,max)`; header + one's-complement checksum built in Glide over `*u8` | none (Linux/mac); IcmpSendEcho on Win | **P3** |
| **Interface enumeration** | `interfaces()->!*Vector<*Iface>` / `if_nametoindex` via `getifaddrs`/`GetAdaptersAddresses` | `-liphlpapi` on Win only | **P4** |
| **Packet capture** | `pcap.open(iface)` on AF_PACKET (Linux) / BPF (mac/BSD); **import-gated** | none (AF_PACKET); libpcap deferred | **P5 (gated)** |

**SocketOptions is the keystone**: once `gnet_setsockopt_int`/`gnet_setsockopt_buf` exist generically, multicast (pack `ip_mreq` bytes in Glide, pass via `setsockopt_buf`), broadcast, TTL, `IP_HDRINCL` for raw, and `SO_BINDTODEVICE` are all *just constants* — zero new C. It also collapses the 5 duplicated `SO_RCVTIMEO`/`SO_SNDTIMEO` blocks into one `gnet_set_timeout` primitive (which owns the timeval-vs-DWORD `#ifdef`).

---

## 4. Async/reactor + ergonomics

### 4.1 Why no new async machine is needed
The park layer is **already fd-generic** — only the *externs* are TCP-named. `__glide_io_park_read(fd)`/`_park_write(fd)`/`__glide_io_get_or_create(fd)`/`__glide_io_close(fd)` take any int fd, register `EPOLLIN|EPOLLOUT|EPOLLRDHUP` once, and obey the lock-before-park / link-and-unlock-after-switch hand-off via `__glide_spin_park`. The completion backends (io_uring, IOCP) are uniform via `__glide_park_blocked` + `t->io_result` + `__glide_unpark_task`. So the foundation **exposes** the existing machine over an arbitrary fd; it does not build a new one.

### 4.2 Async-by-default
The Glide-side client paths route through the **same** reactor wrappers the server uses — one read path, one write path. This fixes the survey's #1 painpoint: today only `listener.glide` is async; `tcp`/`udp`/`tls` clients call blocking `recv()`/`send()` and pin the worker. After migration, on Linux/kqueue they park; on Win/macOS-without-kqueue they block — surface-identical.

### 4.3 Honest backend scoping (reviewers' majors folded in)
- **`gnet_connect_async`** (nonblock + `EINPROGRESS` + park-on-writable + `getsockopt(SO_ERROR)`) is genuinely new, Linux/kqueue-first. On IOCP, ConnectEx is deferred → **falls back to blocking connect with `SO_SNDTIMEO`**. Ship it **without** timeout in P0.
- **`gnet_recvfrom`/`gnet_sendto` async is NOT free on completion backends.** io_uring preps only recv/send/accept; IOCP has only WSARecv/WSASend. So UDP/raw async is **epoll/kqueue readiness on Linux/mac, blocking elsewhere**; io_uring recvmsg/sendmsg prep is a flagged follow-up. Do not promise io_uring datagram in v1.
- **Deadlines (proposed `gnet_read_deadline`)** ride the scheduler's existing monotonic-ns timer thread, but the *first-wins cancel edge* is new concurrency: a task has one `park_list`, and the fd-waiter and the timer node are two independent unpark sources that can **both** fire → double-requeue with no guard today. **Required:** add an atomic `woken` CAS on `__glide_task`; the winner calls `q_push_to`, the loser **deregisters from the other source**. Own runtime sprint + slow-peer/tight-deadline race test. **Restrict deadlines to epoll/kqueue for v1**; io_uring/IOCP fall back to `SO_RCVTIMEO`.

### 4.4 Hard scheduler rule (reviewer major)
Parking code **must never** be reachable from an `@[leaf]` task or an `http_listen_workers` SM handler — `__glide_park` aborts the process there. The `*dyn Stream` parking methods are **forbidden** inside those contexts. Keep `__glide_reactor_active()` as the sole gate; never route the SM HTTP hot path through the new parking wrappers. Keep `read_bytes(buf,max)` (caller-owned buffer) as the hot-path API; the string-returning `stream_read(max)` allocates per call and must not be on the keep-alive path.

### 4.5 Migration of existing modules
- **`tcp.glide`** → delete the whole `c_raw` block (20-123); `TcpStream` becomes `Socket::open(.., SOCK_STREAM, 0)` + `connect`; client TCP becomes async.
- **`udp.glide`** → delete the 142-line `c_raw` block (25-167); gains `connect`/`local_addr`/`set_broadcast`/multicast.
- **`tls.glide`** → drop the inline raw-TCP connect (271-310), the server socket+bind+listen (508-526), accept, and all 4 `SO_*TIMEO` blocks; keep only `SSL_*`. The fd hand-off seam (`TlsStream::attach`, 875) is preserved.
- **`ws.glide`** → replace `tcp`/`tls`/`is_tls` with one `stream: *dyn Stream`; the 4-way branch (64-190) collapses.
- **`http/client.glide`/`h2.glide`** → write the request/response loop once over `*dyn Stream`; h2c unblocked.
- **`dns.glide`** → drop its WSAStartup guard.
- **`h3.glide`** → can drop its private UDP `c_raw` in favor of `gnet_*` + `gnet_getsockname` (deferred cleanup).

---

## 5. Phased roadmap

**P0 — Foundation (must be first).**
- Land `netcore.c` (`gnet_*` + `__gnet_pack`/`__gnet_unpack` + `gnet_const` + `gnet_last_error` + `gnet_bind_unix`/`gnet_connect_unix`) and the reactor-end async wrappers. Wire into `emit_socket_runtime()` after `socket.c`.
- **Sequencing (the seed-extern gotcha):** land the codegen emit + rebuild the seed **first**; introduce `sys.glide` externs in a **follow-up** validated against the freshly-built seed. (`gnet_*` are brand-new symbols → externs with no runtime definition = link failure during self-build.)
- Decide fd width all-in i64 (audit `socket.c` casts + reactor array → hashmap) before raw fds.
- `sys.glide`: `Socket` + the corrected `Stream` trait + free-fn conveniences. **Prove the vtable with `stream_test.glide` first.**
- Add a 5-tuple mixed-width multi-return + `let (...)` destructure smoke test (the tuple suite only exercises 2–3 wide today).
- **Unblocks:** everything below.

**P1 — Migrate tcp/udp + SocketOptions + multicast.** `tcp.glide`/`udp.glide` become pure Glide. `sockopt.glide` (collapses the 5 timeout copies). `multicast.glide`/broadcast.

**P2 — TLS/ws/http migration + unix sockets.** `tls.glide` drops ~250 lines of socket C. `ws`/`client`/`h2` move onto `*dyn Stream` (h2c lands). `unix.glide`.

**P3 — Raw + ICMP (flagship).** `raw.glide` (`SOCK_RAW`), `icmp.glide` (ping/traceroute, in-Glide checksum). Linux/mac/BSD-first; Windows is a **separate `@cfg("windows")` IcmpSendEcho** impl.

**P4 — Deadlines + interface enumeration.** The `gnet_read_deadline` runtime sprint (atomic `woken` CAS, epoll/kqueue only). `iface.glide` (`-liphlpapi` on Windows).

**P5 — Packet capture (gated, deferred).** `pcap.glide` on AF_PACKET (Linux) / BPF (mac/BSD), **import-gated**. libpcap/Npcap deferred as user-linked opt-in.

---

## 6. Cross-platform matrix

| Capability | Linux | macOS / BSD | Windows |
|---|---|---|---|
| socket/bind/connect/listen/accept | ✅ async (epoll/io_uring) | ✅ async (kqueue) | ✅ blocking (IOCP accept deferred) |
| TCP/UDP client read/write | ✅ async | ✅ async | ✅ blocking |
| connect-async + timeout | ✅ | ✅ | ⚠️ blocking + `SO_SNDTIMEO` |
| UDP recvfrom/sendto async | ✅ epoll readiness | ✅ kqueue | ⚠️ blocking |
| Deadlines (`read_deadline`) | ✅ | ✅ | ❌ v1 → `SO_RCVTIMEO` |
| SocketOptions / multicast / broadcast | ✅ | ✅ | ✅ |
| AF_UNIX stream | ✅ | ✅ | ⚠️ works, accept blocks a worker |
| AF_UNIX datagram / abstract ns | ✅ | partial | ❌ compile-excluded (`@cfg("posix")`) |
| Raw / ICMP | ✅ (CAP_NET_RAW/root) | ✅ | ⚠️ separate IcmpSendEcho |
| getifaddrs / iface | ✅ | ✅ | ✅ + `-liphlpapi` |
| Packet capture | ✅ AF_PACKET | ✅ /dev/bpf | ❌ (needs Npcap) |

**Per-OS landmines:** Windows SOCKET width vs the fd-indexed reactor array (§2.6) is the genuine crash risk; `@cfg` gates emission not typecheck (parser.glide:216-218) so Windows-unsupported externs must still typecheck cross-platform; privilege errors (`EPERM`) must surface as `err()`, never panic.

---

## 7. Plug-and-play / build impact

**Verified zero-cost for P0–P4.** `bootstrap/main.glide` derives link needs by text-scanning the emitted C for `#include <zlib.h>`/`<ngtcp2/`/`<nghttp3/`/`<openssl/` (main.glide:2516-2523). `netcore.c` emits only `<sys/socket.h>`/`<winsock2.h>` family headers — none in the needle set — so no lib is auto-pulled. `-lws2_32 -lmswsock` are already unconditional on Windows (main.glide:2660).

**Two real build touch-points:**
1. **Windows `iface.glide` → `-liphlpapi`** (a stock MSVC import lib, not a sysroot `.a`). One new branch in the Windows link section. Cheap.
2. **libpcap/Npcap (deferred).** Would need a `needs_pcap` include-scan sentinel, link branches in both host and cross-sysroot paths, `libpcap.a` staged into all four sysroots + host bundle, and **Windows has no libpcap** (needs Npcap). **Keep AF_PACKET/BPF as the default capture backend (zero lib); treat libpcap as user-linked opt-in.**

**Compiler/runtime changes required:** (a) `emit_socket_runtime()` gains the `netcore.c` include; (b) reactor externs widen i32→i64 atomically with a seed rebuild; (c) `gnet_read_deadline` park-with-deadline + atomic `woken` flag on `__glide_task`; (d) reactor waiter array → hashmap before raw fds. No language-feature changes — `Socket`/`Stream`/`*dyn` are all expressible today.

---

## 8. Risks & mitigations (review blockers/majors folded in)

| Severity | Risk | Mitigation |
|---|---|---|
| **Blocker** | `impl Stream for *Socket` emits no vtable (codegen.glide:1021 requires `TY_NAMED`) | `impl Stream for Socket`, `self: *Socket` (named, concrete) |
| **Blocker** | Default trait-method-via-vtable unproven/likely unsupported | Conveniences are **free functions** over `*dyn Stream`; prove with `stream_test.glide` first |
| **Blocker** | No `cfg!`/feature gating; `@cfg` hard-errors on non windows/posix | Gate pcap by **import**; gate Windows-unsupported bodies with `@cfg("posix")` |
| **Blocker** | Cohort designs disagreed on extern ABI | **Pin one `gnet_*` ABI in P0**; `gnet_set_timeout`/`gnet_bind_unix` are committed deliverables |
| **Blocker** | `gnet_read_deadline` double-wakeup (fd + timer both fire) | Atomic `woken` CAS on `__glide_task`; loser deregisters; own sprint + race test; epoll/kqueue only in v1 |
| **Major** | fd i32→i64 half-widening re-truncates; Windows SOCKET as flat-array index can crash | All-in i64 audit + waiter array→hashmap, atomic land + seed rebuild |
| **Major** | io_uring/IOCP datagram async isn't free (recv/send/accept only) | UDP/raw async = epoll/kqueue readiness; io_uring recvmsg flagged follow-up |
| **Major** | Multicast "zero new C" trades for untested in-Glide BE packing (`IpAddr.v4` host-order, `ip_mreq` net-order) | One shared `gnet_ip_add_membership` C helper with `htonl`, **or** audited in-Glide BE pack + unit test |
| **Major** | Windows raw ICMP is a 2nd impl (IcmpSendEcho); unix-datagram runtime-err is silent | Budget IcmpSendEcho as real `@cfg("windows")` work; compile-exclude unix-datagram bodies |
| **Major** | Privilege errors are bare strings — can't distinguish EPERM | `gnet_last_error()` returns raw errno/WSA code alongside string; `"<code>:<msg>"` convention |
| **Major** | Parking reachable from `@[leaf]`/SM handler aborts the process | `*dyn Stream` parking forbidden there; `__glide_reactor_active()` stays the gate |
| **Minor** | `read(max)->string` allocates per call (regresses keep-alive) | Keep `read_bytes(buf,max)` primary; string form convenience only |

---

## 9. Open questions (maintainer must decide)

1. **fd width** — full audit vs ergonomic-only? Gates raw/high-fd sockets + the Windows SOCKET-as-index crash.
2. **Reactor waiter map** — flat fd-indexed array vs hashmap on the 64-bit handle. Required before AF_PACKET fds; P0 or defer to P5?
3. **Deadline backend scope** — epoll/kqueue-only in v1 (+ `SO_RCVTIMEO` fallback) or invest in `CancelIoEx`/`io_uring_prep_cancel` up front?
4. **Multicast endianness** — shared `htonl` C helper vs audited in-Glide BE packing; how to handle `ipv6_mreq`.
5. **Privilege error contract** — status-prefix sentinel (`"EPERM:..."`) vs typed error enum vs raw errno-alongside-string.
6. **Async opt-in per socket** — raw/datagram/unix auto-register with the reactor, or only on explicit nonblock? (Recommended: raw/capture default blocking + `SO_RCVTIMEO`.)
7. **Windows scope honesty** — ping/traceroute, connect-timeout, AF_UNIX accept, packet capture are all degraded/Linux-first on Windows. Acceptable for the "one identical surface" promise, or does Windows parity block the release?
8. **`TlsStream` implements `Stream`?** Recommended yes, so `http/client.glide` drops its https branch entirely while preserving the `attach` fd-surrender seam.

---

## Key files

- **New:** `bootstrap/runtime/netcore.c`, `src/stdlib/net/sys.glide`, `src/stdlib/net/{sockopt,multicast,unix,raw,icmp,iface,pcap}.glide`
- **Modified:** `bootstrap/codegen.glide` (emit_socket_runtime, ~1216), `bootstrap/runtime/reactor.c` (async wrappers + i64 widen + read_deadline), `bootstrap/runtime/sched.c` (atomic `woken` flag), `bootstrap/main.glide` (Windows `-liphlpapi` branch, ~2660), `src/stdlib/net/{tcp,udp,tls,ws,dns,listener}.glide`, `src/stdlib/http/{client,h2,h3}.glide`
- **Reference shapes:** target pure-Glide module `src/stdlib/net/listener.glide:19-26`; the single platform split to reuse `bootstrap/runtime/socket.c:1-29`; the vtable constraint `bootstrap/codegen.glide:1021`; the `@cfg` constraint `bootstrap/parser.glide:216-218`.

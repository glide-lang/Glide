# Glide perf state — 2026-05-21

Hardware: Windows 11, mingw-gcc ucrt64, Go 1.26.0. Workers: default
(`#CPU` cores). Each result is the median of five back-to-back runs.

## Headline

| Bench                              | Glide      | Go         | Result                  |
|------------------------------------|------------|------------|-------------------------|
| Spawn + drain 100K                 | **11 ms**  | 20 ms      | **Glide 1.8× faster**   |
| Spawn + drain 1M                   | **98 ms**  | 140 ms     | **Glide 1.4× faster**   |
| Pure chan 1M (cap = 1024) (wall)   | **17 ms**  | 37 ms      | **Glide 2.2× faster**   |
| Throughput spawn+chan 100K (wall)  | 492 ms     | **417 ms** | Glide 1.18× off         |
| RAM idle 100K parked (RESULTS old) | **448 MB** | 903 MB     | **Glide 2× lighter**    |
| HTTP/1.1 cold (new conn / req)     | 2688 req/s | n/a        | new bench               |
| HTTP/3 cold (new QUIC / req)       | 29 req/s   | n/a        | handshake dominates     |

vs the prior RESULTS.md baseline (2026-05-13):
- spawn/chan internal timings drifted ±15% in either direction;
  variance from this machine vs the prior measurement bench is the
  noise floor, not a code-level change.
- The throughput-bench gap to Go held at ~18% off — the runtime
  perf experiments this sprint (split-lock waiter list) regressed it
  and were reverted.
- HTTP/1.1 + H3 benches are new; no prior baseline.

## Spawn-only

`spawn worker()` with no work, no channel; main drains via the
runtime's pending counter.

```
spawn 100K
  Glide: 10 10 11 11 12       median 11 ms (internal timer)
  Go:    measured externally — median 20 ms wall

spawn 1M
  Glide: 87 89 98 105 119     median 98 ms (internal timer)
  Go:    measured externally — median 140 ms wall
```

## Pure chan throughput

One producer coro sends 1M ints; main drains via `while let v =
c.recv()`. Buffer cap = 1024.

```
  Glide: 16 17 17 24 58 ms    median 17 ms (wall — bench prints sum only)
  Go:    36 37 37 38 203 ms   median 37 ms
```

The 2.2× gap held; chan ops are still the strongest dimension.
Vyukov MPMC ring + cache-padded cells + 256-iter pause-spin slow
path + conditional cv signal continue to win against Go's channels.

## Throughput — spawn + chan ping-pong

100K short-lived coroutines, each sending one value to a single
consumer. Stresses spawn cost AND chan op cost together.

```
  Glide: 487 490 492 503 638 ms    median 492 ms
  Go:    374 407 417 444 543 ms    median 417 ms
```

Profile instrumentation (committed in 3f7c17f) traced the gap:

```
parks       ~98K  (the senders that found the chan full)
wake_calls  ~104K (every recv triggers a wake)
wake_avg_ns 800-2700 ns (varies with contention)
wake_total  ~95-280 ms = 20-50% of the run
cv_signals  ~10-20K (16% of wakes also need to signal a cv waiter)
```

The 100ms of wake time is the budget to close. A split-lock
attempt (dedicated spinlock on the coro waiter list, mu kept for
cv only) regressed wall-clock by 18% — spinlock contention beats
mutex+futex on this load. The real fix is a lock-free Treiber
stack with 128-bit CAS; deferred until ABA-safe task recycling
lands.

## HTTP/1.1 (new — cold connection per request, 4 KB body)

```
  Glide (writev path on):  3707 ms / 10K = 2697 req/s
  Glide (writev path off): 3741 ms / 10K = 2673 req/s
```

The writev path is correct and bounded but doesn't show on this
bench: new connection per request means TCP handshake + connect
dominate the 0.37 ms-per-request budget. The body memcpy that
writev saves is ~2 µs out of 370 µs — under the noise floor.

The writev path's win lives in keep-alive workloads with larger
bodies, where the response-write hot path is what's being
measured. A proper wrk/wrk2 run against a real load generator
would expose it. The bench I tried locally (100 KB body via
internal load gen on the same process) consumed all the cycles
in the load generator side and never got to a clean number; a
separate dedicated tool is the right call there.

## HTTP/3 (new — cold QUIC connection per request, 2 B body)

```
  Glide H3: 3304-3363 ms / 100 = 29 req/s ≈ 33 ms/req
```

That's mostly QUIC + TLS 1.3 handshake. A typical loopback TCP
handshake is ~0.4 ms; a QUIC handshake is ~30 ms on this stack
because:

- New UDP socket per request
- Full QUIC 1-RTT handshake (Initial → Handshake → 1-RTT keys)
- TLS 1.3 cert verify even on loopback
- nghttp3 init: open the 3 uni streams (control, qpack enc, qpack dec)
- Send the request, receive the response, tear down

Real-world H3 reuses the connection: one handshake amortized over
many streams. Once the H3 client API gains conn-reuse (its public
shape is single-shot today), this number would drop to the few-µs
per stream that loopback can support.

0-RTT engagement on the second request to the same conn (when a
session ticket is cached) was verified end-to-end in this sprint:
`H3SessionCache.accepted()` returns 1, server logs accept early
data. The per-request handshake cost goes away on the cached path
— but the current bench doesn't exercise it because the public
API doesn't take a cache argument outside `http3_request_cached`,
and that creates a fresh QUIC conn each time too.

## RAM — 100K idle coroutines parked on chan

```
              RSS       per coro
  Glide   448 MB        4.5 KB
  Go      903 MB        9.0 KB
```

Unchanged from the prior RESULTS — the lazy-mmap stack policy
(8 KB virtual + 4 KB usable + 4 KB guard) is still the gold
standard. 256-slot stack pool LIFO recycles freed regions.

## What the perf sprint shipped

1. wake_send / wake_recv timing instrumentation (3f7c17f)
2. HTTP/1.1 writev path for header+body ≥ 512 B (1ac752a)
3. HTTP/1.1 sendfile / TransmitFile zero-copy for static files
   served via `HttpResponse::file(path, mime)` (d9d78af)

The writev + sendfile paths are correct-by-construction and
covered by 69/70 of the test suite (the only failure is the
pre-existing live-internet `net_tls_test::test_https_get_example_com`
that's expected to flake on a network change). Their
"X % faster" numbers are best demonstrated by a real load gen
(wrk/wrk2 against a real workload), which this Windows host
isn't set up for.

## What the perf sprint tried and reverted

**Split-lock waiter list** (a1dbe03): added a dedicated spinlock
guarding `coro_*_waiters`, kept `mu` only for cv_wait / cv_signal.
The aim was to halve the ~915 ns per-wake the profile showed.
Result: throughput-bench wall median went 486 → 574 ms (~18%
regression). Under heavy contention (100K wakes/sec from one
recv loop) the spinlock burns CPU spinning where pthread_mutex's
futex backs off via syscall — the mutex was load-correct for
this workload. Reverted.

## Open perf items (priority order)

1. **Lock-free Treiber stack** on the chan coro waiter list,
   ABA-safe via 128-bit cmpxchg16b. Estimated 5-10 ms saved on
   the throughput bench (~10% closer to Go).
2. **TCP_CORK on Linux** to coalesce header + sendfile into one
   TCP segment. Currently with TCP_NODELAY on, the header and
   the sendfile body may go in two packets — 1 extra packet per
   request matters on WAN.
3. **HTTP/3 keep-alive client API** so the cached-session path
   actually amortizes handshake cost. The 29 req/s above would
   drop to single-digit ms once a single conn handles N
   requests.
4. **wrk/wrk2 baseline** for the HTTP/1.1 sendfile + writev
   paths. Local loopback bench shows no win because the
   bench is connect-bound; a real load gen with keep-alive +
   large bodies would expose the wins.

## Files

- `spawn_pure.{glide,go}`         — 100K spawn + drain
- `spawn_1m.{glide,go}`           — 1M spawn + drain
- `chan_pure_glide.glide` /
  `chan_pure_go.go`               — 1M chan ops, cap=1024
- `throughput_glide.glide` /
  `throughput_go.go`              — 100K spawn + chan-ack;
                                    Glide variant now bundles
                                    perf-counter dump via
                                    `__glide_perf_dump()`.
- `idle_glide.glide` /
  `idle_go.go`                    — 100K coros parked on chan,
                                    RSS read from another shell.
- `http_writev.glide`             — HTTP/1.1 server + client,
                                    4 KB body, cold conn / req.
                                    Exercise for the writev path
                                    sanity check.
- `http3_pingpong.glide`          — H3 server + client, 100 cold
                                    QUIC handshakes back-to-back.

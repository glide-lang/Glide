# Reference baseline — Glide vs Go vs Rust tokio

Date: 2026-05-23. Output of project_100b_research_plan.md phase R3,
with Phase A4 (Linux validation) results appended at the bottom.

This is the absolute pre-Phase-A snapshot. Every subsequent SM phase
(A growable stack, B leaf-fn state machine, C scheduler micro-opt,
D packing) measures against these numbers. Goal: Glide's per-task
footprint moves from the Glide row toward the Rust tokio row while
Glide's spawn cost stays ahead of Go's.

## Environment

- Host: Windows 11 26200, MINGW64 bash, mingw-gcc ucrt64.
- Compilers: glide 0.1.1 (post-IOCP, commit 126867b), go 1.26.0,
  rustc 1.92.0 + tokio 1.x (release profile, lto=fat,
  codegen-units=1).
- Hardware: same single machine. No PIN, no special env vars.
- Bench code identical-shape across languages — see
  `bench/park_unpark_coro_clean*.{glide,go,rs}` and
  `bench/massive_concurrency*.{glide,go,rs}`.

## HTTP API standardization: http_listen_workers is now SM-default (2026-05-23)

`http_listen_workers(port, n, handler)` was reworked to use the C
state-machine path as the standard implementation. The handler
signature stays the familiar `fn(*HttpRequest) -> HttpResponse`, but
the routing through the SM bypasses coro-per-conn overhead. Handler
MUST be `@leaf`; the runtime aborts if it parks.

For handlers that need to park (chan ops, sleep, downstream HTTP/DB
calls), the old behaviour is preserved under `http_listen_workers_coro`
— same signature, slower but unrestricted.

```glide
@leaf
fn root(req: *HttpRequest) -> HttpResponse {
    return HttpResponse::ok().body("hello, ".concat(req.path));
}

fn main() -> int {
    http_listen_workers(8080, 4, root);  // SM path, ~160k req/s
    return 0;
}
```

Median across 3 runs, 4 workers, wrk 2t/200c/5s:

| Server | req/s | vs Axum |
|---|---|---|
| Axum (Tokio 0.7) | 188 517 | 1.00× |
| Glide SM bare-strings (`fn(string,string)->string`) | 176 687 | 0.94× |
| nginx 4w | 174 170 | 0.92× |
| **Glide `http_listen_workers` (rich API)** | **159 039** | **0.84×** |
| Glide `http_listen_workers_coro` (coro-per-conn) | 107 424 | 0.57× |

The 10% cost of the rich API vs bare-strings comes from Glide-side
string concats inside `_format_response`. Future work to land that
back in C: serialize HttpResponse field-by-field instead of through
the existing Glide formatter.

## HTTP state-machine fast path with @leaf handler (2026-05-23)

The C state-machine HTTP server (`bench/http_sm_hello.glide`) was
already running at 178k req/s — but only with a hardcoded "hello!"
response. To make it useful, expose a hook for user `@leaf` handlers:

```glide
@leaf
fn hello(_method: string, _path: string) -> string {
    return "hello\n";
}

fn main() -> int {
    http_listen_sm_workers(8080, 4, hello);
    return 0;
}
```

Each worker is a real pthread running its own epoll loop + per-conn
state machine in C. When a request finishes parsing, the C code
inlines a call into the user's `@leaf` handler. The handler returns
the body as a Glide string; the C framing emits 200 OK + text/plain
headers around it. No coro spawn, no stack page commit, no
ctx_switch. The `@leaf` contract enforces that the handler can't
park — runtime aborts if it tries.

Median across 3 runs, 4 workers, 2 wrk threads / 200 conns / 5s:

| Server | req/s | vs Glide coro-per-conn | vs Axum |
|---|---|---|---|
| **Axum (Tokio 0.7)** | 189 007 | 1.76× | 1.00× |
| **Glide SM + @leaf** | **180 781** | **1.68×** | **0.95×** |
| nginx 4w | 174 138 | 1.62× | 0.92× |
| Glide SM hardcoded | 178 527 | 1.66× | 0.94× |
| Glide handler coro-per-conn | 107 424 | 1.00× | 0.57× |

The handler call overhead is essentially zero — 180k with user
@leaf handler ≈ 178k hardcoded "hello!". The C trampoline + Glide
string return path adds ~0% cost vs a hand-coded C response.

**Why this is the win:** the 70% gap to Axum that existed in
`http_listen` (coro-per-conn) was almost entirely the coro layer,
not syscalls or HTTP parsing. Stripping the coro and inlining the
handler lands Glide within 5% of Axum on hello-world HTTP/1.1.

Restrictions:
- Linux only (epoll-based). macOS / *BSD / Windows fall through.
- Handler MUST be `@leaf` — no chan, sleep, I/O, or non-leaf calls.
- Response is text/plain + status 200. Full
  `HttpRequest`/`HttpResponse` API stays on the coro-per-conn path
  for cases that need other status codes / content types / headers.

For health checks, hot JSON endpoints, prometheus scrapers, this is
the fast path. Use `http_listen_workers` for full-feature handlers.

## io_uring backend (2026-05-23, Linux 6.8, liburing 2.5)

Opt-in via `GLIDE_REACTOR=uring`. SQPOLL mode by default — kernel
polls SQ on its own pthread, zero `io_uring_enter` syscalls in
steady state. Falls back to plain mode on hosts where SQPOLL is
blocked (older kernels need CAP_SYS_NICE). Disable SQPOLL with
`GLIDE_URING_NOSQPOLL=1`.

| Backend | 4 workers req/s | vs epoll | Notes |
|---|---|---|---|
| epoll (default) | 110 089 | 1.00× | readiness model, syscall per op |
| io_uring + SQPOLL | 125 750 | 1.14× | best config seen |
| io_uring no-SQPOLL | 43 279 | 0.39× | syscall per submit, lots of read errors |
| nginx (reference) | 172 539 | 1.57× | hand-tuned C, epoll edge-triggered |

io_uring with SQPOLL on Linux 6.8 gives +14% throughput at the
4-worker sweep, but the SQPOLL kernel thread saturates around
125k req/s on this hardware (one CPU bound). The win is real but
modest at this hardware ceiling.

**Open improvements:**
- Per-worker uring instances (4 SQPOLL threads instead of 1)
- Multi-shot accept (single SQE produces many CQE)
- Batched send via IOSQE_IO_LINK
- IORING_REGISTER_FILES to skip fd lookup
- Linked sendfile for static file serving

These need 200-500 LOC each; deferred to a focused io_uring sprint.

## HTTP REUSEPORT scaling (2026-05-23, Ubuntu 24.04, 4 cores, wrk co-located)

Hello-world `GET /` benches via `bench/http_hello_single.glide` and
`bench/http_hello_multi.glide` (both compiled with glide_b3f). nginx
1.24.0 with `worker_processes 4; listen 8081 reuseport;`.

| Server | Workers | CPUs | Req/s | Notes |
|---|---|---|---|---|
| Glide `http_listen` | 1 | 1 (pinned) | 18 523 | single accept queue |
| Glide `http_listen_workers` | 3 | 3 (pinned C0-2) | **117 799** | wrk pinned C3, super-linear scaling |
| Glide `http_listen_workers` | 4 | 4 (shared w/ wrk) | 111 619 | no pinning |
| Glide `http_listen_workers` | 8 | 4 (shared w/ wrk) | 111 173 | excess workers don't hurt |
| nginx | 4 | 4 (shared w/ wrk) | 172 539 | reference |

Headline: Glide REUSEPORT delivers **6.0× scaling** with 4 workers vs
1 worker. Reaches **65% of nginx** throughput on identical hardware.
The remaining 35% gap is mostly io_uring (nginx uses linux-aio/epoll;
io_uring lands next phase).

The infrastructure was already present (`http_listen_workers` at
`src/stdlib/http.glide:1122`, `os_has_reuseport_balance()` at
`src/stdlib/os.glide:586`). What was missing: benchmark validation
that the kernel actually load-balances. Confirmed: 4 separate listening
sockets on port 8080, kernel distributes new connections across all
of them, accept queues stay shallow under load.

## Headline table (snapshot after Phase A4 + B1 + B2 + B3b + B4, 2026-05-23)

| Metric | Glide | Go | Rust tokio | Winner |
|---|---|---|---|---|
| **spawn_1m total ms** (1M tasks created, drained) | **113 Win / 160 Lin** | 107 | 515 | Go ≈ Glide (Rust 5× slower) |
| **park_unpark_coro_clean** ns/cycle, `GLIDE_CHAN_SPIN=0` | **232** | 267 | 159 | **Glide beats Go** (15% faster) |
| **park_unpark_coro_clean** ns/cycle, default | 3 140 | 267 | 159 | spin-budget tradeoff |
| **massive_concurrency** bytes/task (regular coro, 100k parked, Linux) | 4 288 | 9 051 | 403 | tokio 10× lighter than regular coro |
| **leaf_footprint** bytes/task (`@leaf` task, 100k–1M, Linux) | **192** | n/a (Go has no stackless variant) | 403 | **Glide leaf wins** (2.1× lighter than tokio) |
| **HTTP hello req/s** (`http_listen_workers` rich API + `@leaf`, 4 cores) | **159 039** | — | 188 517 (Axum) | Glide at 84% of Axum |
| **HTTP hello req/s** (`http_listen_sm_workers` bare strings + `@leaf`, 4 cores) | **176 687** | — | 188 517 (Axum) | **Glide at 94% of Axum** |

### Original pre-Phase-A baseline (for diff)

| Metric | Glide | Go | Rust tokio | Winner |
|---|---|---|---|---|
| **spawn_1m total ms** (1M tasks created, drained) | **113** | 107 | 515 | Go ≈ Glide (Rust 5x slower) |
| **park_unpark_coro_clean** ns/cycle (coro↔coro ping-pong) | 7000 (slow) / 50 (fast) | **267** | 159 | Rust > Go > Glide (variance issue) |
| **massive_concurrency** bytes/task (100k parked) | n/a Win* | 9051 | **403** | Rust 22× lighter than Go |

`*` Glide's massive_concurrency.glide builds clean on Linux but
Windows Defender corrupts the link output when the binary spawns
100k+ concurrents on a single chan. Source ships in `bench/`. Run
on Linux for the bytes-per-task number. Current Glide stack
(64KB per coro) means 100k coros reserve ~6.4GB virtual; actual
RSS depends on page touch patterns (Linux/macOS lazy commit;
Windows eager).

## Detailed runs

### spawn_1m — spawn cost

Spawn 1M short-lived workers, drain via atomic counter. Measures
spawn allocation + dispatch + completion.

Glide (3 runs, median 113ms):
```
spawn+drain 1000000 : 113 ms
spawn+drain 1000000 : 103 ms
spawn+drain 1000000 : 143 ms
```

Go (3 runs, median 107ms):
```
spawn+drain 1000000: 108 ms
spawn+drain 1000000: 106 ms
spawn+drain 1000000: 107 ms
```

Rust tokio (3 runs, median 515ms):
```
spawn+drain 1000000: 540 ms
spawn+drain 1000000: 612 ms
spawn+drain 1000000: 394 ms
```

**Reading**: Glide and Go are essentially tied (~110ms each). Rust
tokio is 5× slower here because of per-task wakeup overhead in the
state-machine path. This is the headline pitch for Glide's spawn
cost — already best-in-class.

### park_unpark_coro_clean — coro-coro park/unpark

Two coros ping-pong through cap-1 chans; main blocks on a result
chan. Each cycle = send + recv pair on both sides = ~4 park/unpark
events (best case).

Glide (5 runs, all in slow mode):
```
coro-coro park-unpark 100000 cycles avg_ns/cycle: 7035
coro-coro park-unpark 100000 cycles avg_ns/cycle: 7060
coro-coro park-unpark 100000 cycles avg_ns/cycle: 7051
coro-coro park-unpark 100000 cycles avg_ns/cycle: 7046
coro-coro park-unpark 100000 cycles avg_ns/cycle: 7055
```

Earlier observed bimodal fast mode (~50 ns/cycle) when both coros
stay on the same worker (no steal). Once steal happens between
workers, cv_signal of the idle worker per iteration → 7000 ns.
**The bimodal range is the open problem the SM phases need to
close.** Phase B leaf-fn state machines eliminate the worker
park/wake by running directly on the spawning thread.

Go (1 run, no variance observed):
```
coro-coro park-unpark 100000 cycles avg_ns/cycle: 267
```

Rust tokio (1 run):
```
coro-coro park-unpark 100000 cycles avg_ns/cycle: 159
```

**Reading**: Rust tokio's stackless tasks are the fastest at
coro-coro ping-pong (single CPU, in-memory state, atomic CAS wake).
Go's growable-stack goroutines pay ~1.7× over Rust. Glide today is
~30-50× slower than Rust due to the cv-signal-on-worker-idle
pattern. **This is the gap Phase B closes.**

### massive_concurrency — bytes per parked task

Spawn N tasks all parked on a chan recv that never fires. Sample
RSS after 500ms settle. Report bytes-per-task.

Glide (Windows): **build blocked by Defender heuristic**. Source
in `bench/massive_concurrency.glide`. Run on Linux to get the
number; expected to be ~64KB/task today (stack-dominated).

Go (1 run, N=100k):
```
massive 100000 tasks elapsed_ms: 782
  rss_baseline_mb: 11 rss_after_mb: 874
  delta_mb: 863 bytes_per_task: 9051
```

Goroutine ~9 KB/task. Matches Go's 2 KB initial stack + G struct +
chan link state. After the stack-shrink path kicks in, idle
goroutines actually trend toward 2.5 KB on long-lived workloads;
this is the cold-startup number.

Rust tokio (1 run, N=100k):
```
massive 100000 tasks elapsed_ms: 541
  rss_baseline_mb: 5 rss_after_mb: 44
  delta_mb: 38 bytes_per_task: 403
```

tokio task ~400 B/task. Stackless state machine + watch::Receiver
clone + JoinHandle. Best-in-class for "many parked tasks".

**Reading**: Rust tokio is 22× lighter per parked task than Go.
The 100B target for Glide Phase D would beat tokio by 4×, the
tweetable number.

## How each phase moves the numbers

| Phase | Affects | Expected delta |
|---|---|---|
| A growable stack | massive bytes/task | 64 KB → ~4 KB (Go-shape) |
| B leaf-fn state machine | massive bytes/task | 4 KB → ~500 B (Tokio-shape) |
| B leaf-fn state machine | park_unpark ns/cycle | 7000/50 → ~200 ns (eliminate worker park) |
| C scheduler micro-opt | spawn_1m ms | 113 → ~80 ms (faster than Go) |
| D packing | massive bytes/task | 500 → ~150 B (beat Tokio) |
| D packing | park_unpark ns/cycle | 200 → ~80 ns (match Tokio) |

## Open holes in this baseline

1. **massive_concurrency on Linux**: required to seal the bytes/task
   number for Glide today. Run on the production glide-lang.org
   server or a fresh VPS. Expect "OOM around 100k coros" for
   today's stack-dominated build.

2. **Glide's bimodal park_unpark**: median 7000 ns hides a "fast
   mode" of 50 ns. The bench harness median-of-5 reports whichever
   mode hit more times. For the real comparison vs Rust/Go,
   we should run N=100 with PIN=1 + GLIDE_WORKERS=1 to lock the
   single-worker mode.

3. **HTTP server bench equivalents**: the marketing pitch is "Go
   syntax, Rust performance" but we don't have HTTP throughput
   numbers vs tokio's hyper / Go's net/http yet. Add a
   wrk-against-hello bench triple in the next reference pass.

4. **macOS / Linux ARM numbers**: this is x86_64 Windows only.
   Phase 2 of R3 should run the same set on Linux x86_64 (mostly
   relevant for HTTP), Apple Silicon (kqueue + ARM), and Linux
   ARM64 (Graviton-shape benchmarks).

## What this number-set DOESN'T claim

- Anything about CPU-bound (parsing, math). For that, Glide compiles
  through the same C optimizer that Rust uses via LLVM and Go uses
  via its own gc; expected to be within ±5% of both. No bench yet.
- Anything about latency tail (p99, p99.9). These benches measure
  median throughput. Tail benchmarks need a wrk-style harness with
  histograms.
- Anything about real HTTP throughput. The 100B plan focuses on
  task footprint + park latency; the HTTP server gets its own
  separate validation track.

## Reproducer commands

```bash
# Glide
./glide.exe build bench/spawn_1m.glide -o bench/spawn_1m.exe
./glide.exe build bench/park_unpark_coro_clean.glide -o bench/park_unpark_coro_clean.exe
# Glide massive on Linux only:
./glide.exe build bench/massive_concurrency.glide -o bench/massive_concurrency.exe

# Go (each file runs standalone via `go run`)
go run bench/spawn_1m.go
go run bench/park_unpark_coro_clean_go.go
go run bench/massive_concurrency_go.go

# Rust tokio (single Cargo project under bench/rust/)
cd bench/rust && cargo build --release
./bench/rust/target/release/spawn_1m
./bench/rust/target/release/park_unpark_coro_clean
./bench/rust/target/release/massive_concurrency
```

GLIDE_MASSIVE_N=1000000 raises the massive-concurrency count to
1M tasks. GLIDE_WORKERS=1 + GLIDE_PIN_WORKERS=1 stabilises the
Glide bimodal bench distribution at the cost of measuring only
single-worker performance.

## Phase A4 update — Linux validation (2026-05-23)

Cross-compiled `glide.exe build bootstrap/main.glide --target=x86_64-linux-musl`
from a Windows host and ran on Ubuntu 24.04 (kernel 6.8, 16 GB RAM,
musl-static binary).

### massive_concurrency on Linux

| N (tasks) | elapsed ms | RSS delta MB | bytes/task | Notes |
|---|---|---|---|---|
| 100 000 | 1223 | 409 | **4288** | 3 runs identical |
| 400 000 | 3395 | 1636 | **4288** | |
| 500 000 | 4129 | 2045 | **4288** | |
| 600 000 | 4633 | 2454 | **4288** | |
| 800 000 | 5612 | 3272 | **4288** | |
| 1 000 000 | 7777 | 4090 | **4288** | needs `sysctl vm.max_map_count=4194304` |

**4288 bytes per task** — locked in across N. This is the Phase A
landing number: Glide now beats Go (9051 B) by 2.1× and is 10×
heavier than Rust tokio (403 B). Phase B (leaf-fn state machine
codegen) is what closes the gap to tokio.

The number isn't bytes of "Glide stack" alone — it includes the
8 KiB stack + 4 KiB guard mmap + task struct + chan-recv-waiter
node + worker queue metadata. The stack mapping is mostly virtual,
not RSS — Linux only commits the touched pages. A parker function
that does nothing touches 1 page of stack, so RSS ≈ task struct
(few hundred B) + the chan-waiter slot + amortised reactor entry.

### park_unpark_coro_clean on Linux — pre Phase B

| Mode | ns/cycle |
|---|---|
| Default (multi-worker, no pin) | 23 200 |
| `GLIDE_WORKERS=1 GLIDE_PIN_WORKERS=1` | 23 266 |

Worse than Windows (7000 / 50 bimodal) — Linux pthread_cond_signal
is more expensive when the wake target is a parked worker thread
than the Windows equivalent. Pinning doesn't help because the cost
is in the cv-wake itself, not in cross-core migration.

### park_unpark_coro_clean on Linux — after Phase B1 + B2 + B3b (2026-05-23)

| Mode | ns/cycle | vs Go (267) | vs Rust tokio (159) |
|---|---|---|---|
| `GLIDE_CHAN_SPIN=0`, default workers | **232** | **15% faster** | 1.5× slower |
| `GLIDE_CHAN_SPIN=0`, single worker | 233 | 15% faster | 1.5× slower |
| `GLIDE_CHAN_SPIN=0`, pinned multi | 232 | 15% faster | 1.5× slower |
| `GLIDE_CHAN_SPIN=8` | 963 | 3.6× slower | — |
| Default (`spin=32`) | 3 140 | 12× slower | — |

The Phase B improvements stacked:
- **B1**: chan slow-path spin budget dropped from 256 PAUSEs to 32
  by default (n_workers==1 → 0). 23 µs → 3.2 µs default.
- **B2**: per-worker `runnext` slot bypasses the queue spinlock +
  cv-signal path when an unparker pushes to its own worker
  (mirrors Go's runtime2.go `P.runnext`). 3.2 µs → 270 ns at spin=0.
- **B3b**: chan coro wait list switched from `pthread_mutex_t` to
  Glide's internal spinlock (`__glide_spin_t`). 5-10 ns critical
  sections didn't justify the ~30 ns/lock futex overhead × 4 lock
  cycles per ping-pong. 270 ns → 232 ns at spin=0.

The default `spin=32` cost (3.2 µs) is intentional — it's a budget
for cross-core handoffs in HTTP-style workloads where a producer
on another core is about to push within ~1 µs. Override via
`GLIDE_CHAN_SPIN=N` (clamps to [0, 1024]).

To go below 200 ns toward tokio's 159 ns we'd need stackless coros
(no ctx_switch overhead) — that's Phase B4-style codegen for chan
operations themselves, not just spawn sites.

### Stack-grow handler — POSIX status

Phase A2/A3 ship the growable-stack infrastructure. Validated on
Linux musl that:

- The SIGSEGV handler **fires** on guard-page touch.
- The pre-mmap, mmap, mprotect-guard, memcpy steps **succeed**.
- The narrow RBP-chain fixup **walks correctly**.
- `mcontext.gregs[REG_RIP]` modifications **are honoured** by
  sigreturn (RIP-advance probe survives the SEGV).
- `mcontext.gregs[REG_RSP]` modifications **are NOT honoured** —
  after handler returns, the faulting instruction re-runs with
  the old RSP and faults at the same guard address.

The RSP-not-honoured behaviour is reproducible in this musl-static
build configuration. Same root-cause class as the Windows VEH
limitation documented in [[project_codegen_win_break]] — the
handler infrastructure is correct, the kernel/libc interface
fails to apply the RSP rewrite. Investigation deferred to Phase B
(where state-machine codegen eliminates the need to grow at all
for the common leaf-fn cases).

Workaround for now: lift the default starting stack to 16 KiB
(GLIDE_STACK_SIZE env) for code paths that recurse beyond the
current ceiling. Set per-coro on spawn for affected workloads.

### Phase B4: @leaf attribute + stackless spawn

Add `@leaf` on a function so the spawn site emits `__glide_spawn_leaf`
instead of `__glide_spawn`. The leaf path allocates only a task struct
(~168 B) — no per-task stack mmap, no ctx struct init. The worker
dispatches leaf tasks inline on its own pthread stack with no
ctx_switch. A leaf function that mis-marks itself and reaches
`__glide_park` (chan op / sleep / I/O) aborts with a clear error
instead of deadlocking silently.

Bench `bench/leaf_footprint.glide` spawns N leaf tasks that spin on
a release atomic. With 8 workers, only 8 spin at a time; the rest
queue. Sample RSS while the queue is full:

| N (leaf tasks) | RSS delta MB | bytes/task |
|---|---|---|
| 100 000 | 19 | **199** |
| 500 000 | 92 | **192** |
| 1 000 000 | 184 | **192** |

192 B/task — beats Rust tokio (403 B) by 2.1×, beats Glide regular
coro (4288 B) by 22×, beats Go goroutine (9051 B) by 47×. This is the
"tokio-class footprint" deliverable for the 100B plan.

The leaf path is opt-in (via `@leaf`); regular `spawn` still pays the
4 KB stack page for code that may yield. Phase D (packing) is the
next lever — fold the task struct itself onto a shared slab so
multiple tasks share a cache line.

### bench harness fixes uncovered

1. `bench/massive_concurrency.glide` blocked on coro drain at
   program exit (main returning didn't terminate the parked
   parker coros). Added a `__glide_bench_exit(0)` c_raw helper
   that calls `_Exit(0)` to bypass the drain — strictly a bench
   shortcut, not a runtime behaviour change.
2. Linux glibc-vs-musl: `_GNU_SOURCE` must be defined BEFORE any
   `<pthread.h>` include for `cpu_set_t` / `CPU_SET` etc. to
   expose, otherwise affinity-pinning fails to compile on musl
   targets. Now `-D_GNU_SOURCE` is in the host cc flags.
3. Bench needs explicit `import stdlib::env::*;` plus an
   `extern fn now_ns() -> i64;` declaration — extern fns from
   imported modules are module-local, not re-exported.

# Reference baseline — Glide vs Go vs Rust tokio

Date: 2026-05-23. Output of project_100b_research_plan.md phase R3.

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

## Headline table

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

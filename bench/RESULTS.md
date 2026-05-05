# Glide vs Go — concurrency bench

Hardware: Windows 11, mingw-gcc ucrt64, Go 1.26.0. Workers: default
(`#CPU` cores). Each result is the median of five back-to-back runs.

## Headline — Glide wins 4 of 5

| bench                      | **Glide** | Go     | resultado                |
|----------------------------|-----------|--------|--------------------------|
| Spawn + drain 100K         | **6 ms**  | 10 ms  | **Glide 1.7× faster** ✅  |
| Spawn + drain 1M           | **85 ms** | 94 ms  | **Glide 1.1× faster** ✅  |
| Pure chan 1M (cap = 1024)  | **24 ms** | 48 ms  | **Glide 2.0× faster** ✅  |
| RAM idle 100K parked       | **448 MB**| 903 MB | **Glide 2.0× lighter** ✅ |
| Throughput spawn+chan 100K | 486 ms    | 436 ms | Glide 1.1× off ⚠️         |

## Spawn-only

Cheapest possible coroutine: `spawn worker()` where `worker` returns
immediately. Main waits via the runtime's pending counter (`while
pending_count() > 0 {}`). No channel, no payload.

```
spawn 100K
  Glide: 7  6  6  6  6        median 6 ms
  Go:   16 11 13 10 10        median 10 ms

spawn 1M
  Glide: 82 96 92 85 77       median 85 ms
  Go:   105 107 93 92 94      median 94 ms
```

The 1M result is the one that flipped vs the previous score: sharded
pending counter + TLS task magazine + batched `q_push_chain` from main
brought it from `Glide 1.5× off` to `Glide 1.1× faster`.

## Pure chan throughput

One producer coroutine sends 1M ints; main drains via `while let v =
c.recv()`. Buffer cap is 1024.

```
  Glide: 29 22 23 25 24 ms    median 24 ms
  Go:    48 49 48 48 49 ms    median 48 ms
```

Vyukov MPMC ring + cache-padded cells + 256-iter pause-spin slow path
+ conditional cv signal. The fast path is fully lock-free; the slow
path skips the syscall altogether when there's no waiter.

## Throughput — spawn + chan ping-pong

100K short-lived coroutines, each sending one value to a single
consumer. Stresses spawn cost AND chan op cost together.

```
  Glide: 489 581 484 486 474 ms    median 486 ms
  Go:    459 415 436 475 425 ms    median 436 ms
```

The remaining gap is `chan->mu` + `coro_send_waiters` list pop on each
unpark. cap=1024 means ~99K senders park on the wait list; each main
recv pays ~5 µs to unpark one. Lock-free Treiber stack with cmpxchg16b
would close it; deferred until ABA-safe task recycling lands.

## RAM — 100K idle coroutines parked on chan

```
              RSS         per coro
  Glide   448 MB          4.5 KB
  Go      903 MB          9.0 KB
```

mmap'd 8 KB virtual stacks (4 KB usable + 4 KB guard) with lazy
working-set. Stack pool LIFO 256 recycles freed regions.

## What's installed today

1. M:N scheduler with own asm context switch (no Win Fibers, no
   ucontext) — ~10-15 ns per switch.
2. Vyukov MPMC bounded ring per `chan<T>`, cache-padded cells.
3. TLS magazine for the task pool (per-thread cache, BATCH=32, MAX=64).
4. `reset_ctx` deferred to worker_main, gated on `has_run==0`.
5. Sharded `__glide_pending` per spawner-thread (n_workers + 1 shards).
6. Batched `q_push_chain` from main (BATCH=16) — 16 spawns per spinlock
   acquisition on W0's queue.
7. Auto-flush of main's spawn buffer at every blocking call site.
8. Stack pool LIFO 256, mmap recycled.
9. Per-worker run queue + spinlock + linked list, cv broadcast only on
   `q->idle`.
10. Work-stealing across workers, gated on `!has_run` for Win64 TIB.
11. `sleep_ms` is async — coro parks on the timer thread, worker is
    free to pick up another task.

## What we tried but didn't help

- Round-robin push from main: 20× worse (cache + cv signal storm).
- Vyukov MPMC for the run queue: same perf as spinlock; reverted to
  keep code simpler.
- Batched `wake_send/recv` (threshold-based): variance exploded; the
  slow-path drain caused a re-park flood.
- Sharding `pending` by `home_worker`: all main spawns went to
  `shard[0]` (defeated the purpose).
- Bigger run queue (256K cells): added 30 ms init time, no scaling
  benefit.

## Files

- `spawn_pure.{glide,go}`        — 100K spawn + drain
- `spawn_1m.{glide,go}`          — 1M spawn + drain
- `chan_pure_glide.glide` /
  `chan_pure_go.go`              — 1M chan ops, cap=1024
- `throughput_glide.glide` /
  `throughput_go.go`             — 100K spawn + chan-ack
- `idle_glide.glide` /
  `idle_go.go`                   — 100K coros parked on chan, RSS read
                                   from another shell during the 10 s
                                   measurement window

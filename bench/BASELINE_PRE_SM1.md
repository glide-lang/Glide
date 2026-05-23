# Concurrency micro-bench

- Runs: 3 per bench, in order, no rebuild between runs.
- Glide: `./glide.exe` (PE32+ executable for MS Windows 5.02 (console), x86-64, 19 sections)
- Host: MINGW64_NT-10.0-26200 3.6.4-b9f03e96.x86_64 x86_64
- Date: 2026-05-22T22:52:59Z

## spawn_only_glide

```
```

## spawn_1m

```
spawn+drain 1000000 : 98 ms
spawn+drain 1000000 : 96 ms
spawn+drain 1000000 : 97 ms
```

## send_only_glide

```
```

## chan_pure_glide

```
done sum= 1783293664
done sum= 1783293664
done sum= 1783293664
```

## throughput_glide

```
[glide perf] parks=98976 spin_parks=0 unparks=0 q_pushes=98976 cv_signals=19414
             wake_calls=104038 wake_total_ns=89290400 wake_avg_ns=858
sum 704982704
[glide perf] parks=98975 spin_parks=0 unparks=0 q_pushes=98975 cv_signals=18323
             wake_calls=103595 wake_total_ns=95871000 wake_avg_ns=925
sum 704982704
[glide perf] parks=98976 spin_parks=0 unparks=0 q_pushes=98976 cv_signals=15068
             wake_calls=103690 wake_total_ns=106114200 wake_avg_ns=1023
sum 704982704
```

## park_unpark

```
park-unpark 100000 cycles total_ns: 5759900 avg_ns/cycle: 57
park-unpark 100000 cycles total_ns: 5616400 avg_ns/cycle: 56
park-unpark 100000 cycles total_ns: 6075000 avg_ns/cycle: 60
```

## Key signals to watch in SM-1

- `park_unpark avg_ns/cycle` — baseline ~57 ns. Expect 25-35 ns post-SM-1.
- `throughput_glide wake_avg_ns` — baseline ~850-1000 ns. Expect 200-400 ns post-SM-1.
- `spawn_1m` — baseline ~97 ms. Expect ~80-90 ms post-SM-1 (small win, spawn cost dominated by malloc not park).
- `chan_pure_glide sum` — should stay constant (correctness check, not a perf metric).

Any commit that touches scheduler / chan park/wake should re-run this
harness and append results below as `## park_unpark (post-SMN)` etc.

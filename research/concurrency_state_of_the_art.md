# Concurrency state of the art — research notes for Glide's 100B push

Date drafted: 2026-05-23. Author: research session R1 of the 100B
master plan.

This is a written research dump assembled from prior knowledge of the
relevant runtimes and papers. Specific byte counts and code shapes
are flagged with `[~]` when approximate / from memory rather than
verified by reading current source. Every such mark is something we
should validate by opening the upstream source when we approach the
implementation phase that depends on it.

## Why this document exists

Glide commits to:

- `spawn`, `chan<T>`, `select!` stay the user-facing API. No
  `async fn` / `.await` keywords.
- Performance target: a leaf-shape concurrent task weighs ~100B
  (reference: LWIP `tcp_pcb` 120B, Pony actor 232B, Tokio task
  ~256-500B). Today: 64KB stack per coro. Reduction target ~500x.
- The user never sees the choice between stackful and stateless;
  the compiler picks per spawn site.

The implementation plan in `project_100b_research_plan.md` has 5
phases. This file is the input to all of them. Each subsequent phase
references back here.

## 1 — Taxonomy of concurrency primitives

The runtime design space splits along three axes:

| Axis | Options |
|---|---|
| Stack | stackful (own stack) vs stackless (state struct only) |
| Stack growth | fixed (pthread) vs growable (Go) |
| Scheduler model | 1:1 kernel (pthread) vs M:N user (Go, Tokio, Glide) |
| Lowering | hand-rolled (pthread, FreeRTOS) vs compiler-lowered (Rust async, Kotlin) |
| Yields | preemptive (pthread, Go 1.14+) vs cooperative (Tokio, Glide) |

Plotting popular systems:

```
                           STACK?
                          /      \
                      YES /        \ NO
                         /          \
                    STACKFUL      STACKLESS
                      /  \           /    \
                FIXED  GROWABLE  EXPLICIT  COMPILER
                  |      |        |          |
              pthread   Go      callback   Rust async
              FreeRTOS Erlang   chains     Kotlin suspend
              cilk*    Pony*    Node.js    C# async/await
                                           C++20 coroutines
```

`*` cilk and Pony are hybrids: cilk has cactus stacks (forked
sub-stacks), Pony has stackful actors with per-actor message queues.

**Glide today**: stackful, fixed 64KB, M:N user, hand-rolled, cooperative.

**Glide post-100B plan**: stackful AND stackless coexist. Stackful
growable (Phase A). Stackless via compiler lowering for leaf-fn
(Phase B). User-facing API unchanged.

## 2 — Memory footprint reference points

These numbers are the BASELINE against which Glide should be measured.

### Linux pthread

- `pthread_t`: 8 bytes (just an ID/handle).
- Kernel TCB (`task_struct` in kernel): ~8KB+ [~] — many fields:
  PID, parent, file descriptors, memory descriptor, signal masks,
  scheduler state, namespace data, cgroup links.
- Userspace stack: defaults 8MB virtual (lazily committed). [~]
- Total min commit: ~16-32KB even for an idle pthread.
- **Doesn't scale to 1M.** 1M pthreads = 8TB virtual address space
  (impossible on 64-bit Linux's 47-bit user space).

### Go goroutine

[~] from training; runtime/runtime2.go contains the `g` struct.

`g` (the goroutine struct) ~600B:
- `stack` (lo, hi pointers): 16B
- `stackguard0`, `stackguard1`: 16B
- `_panic`, `_defer`: 16B
- `m` (machine pointer): 8B
- `sched` (gobuf: saved PC, SP, BP, regs, ctxt): ~80B
- `syscallsp`, `syscallpc`, `stktopsp`: 24B
- `param`: 8B
- `atomicstatus` (uint32): 4B
- `goid` (uint64): 8B
- `waitsince` (int64): 8B
- `waitreason`: 4B
- `preempt`, `preemptStop`, `preemptShrink`: bits
- `gcAssistBytes`, `gcscanvalid`: profiling
- `sudog` waiters list head: 8B
- `traceback` cache: ~50B
- `cgoCtxt`, `labels`: pointers
- `timer`: 8B
- `selectDone`: 8B
- ... plus ~30 more small fields

Initial stack: 2KB (allocated as a contiguous region with guard at
start). Grows up to 1GB max [~].

**Per-goroutine min total: ~2.6KB.**

Stack growth via `runtime.morestack` + `runtime.copystack`. Triggered
by prologue check in every fn. Cost of growth: O(stack size in use)
for the copy + relocation walk.

### Tokio task (Rust async)

From Tokio v2 design notes (2021 rewrite blog) and `tokio/src/runtime/task/`:

Task layout (approximate, varies by config):
- Header: state (atomic u32), refcount, queue links, vtable ptr
  → ~32-48B [~]
- Waker storage: 16B (Arc<TaskWaker>) [~]
- The future itself: depends on async fn (the state struct).
  - Empty future: ~0B
  - Simple `async fn(): T`: ~size(largest live-var set) + tag
  - 1-await fn with one local: ~64B
  - 10-await fn with several locals: ~200-500B

JoinHandle adds ~16B (Arc to result slot).

**Realistic Tokio task min: 64-128B header + future ~32-256B = 100-400B total.**

Source: `tokio::task::core::Cell<T>` definition. We should re-read
before B-phase to update numbers.

### Pony actor

From Clebsch et al "Deny Capabilities for Safe, Fast Actors" (ECOOP 2015)
and ponyc source `src/libponyrt/actor/actor.c`:

`pony_actor_t` (~232B [~]):
- Type descriptor pointer: 8B
- Heap (per-actor heap struct): tagged
- Message queue head/tail: 16B
- GC bookkeeping (rc, mark, view counts): ~24B
- Continuation/state machine: pointer
- Padding to cache line: ~16B
- Plus padding to TWO cache lines (one for "hot" fields, one for
  cross-thread messages)

Plus initial heap for actor's private data: optional, depends on
actor body.

Pony's actor count benchmarks: 10M+ actors in single GB.

Notable choices:
- Per-actor heap (like Erlang) → GC is local, no stop-the-world.
- Message queue uses Hoare-like baton-pass via CAS.
- No stack per actor — actors are state machines run by scheduler.

**Pony is the cleanest "actor as ~232B state struct" reference. Closest to what Glide's Phase B should produce.**

### LWIP tcp_pcb

From `lwip/src/include/lwip/tcp.h` (approximate layout):

```c
struct tcp_pcb {
    /* common pcb header */
    struct ip_addr local_ip;       // 16B (IPv6) or 4B (IPv4)
    struct ip_addr remote_ip;      // 16B or 4B
    u16_t local_port;              // 2B
    u16_t remote_port;             // 2B
    u8_t  state;                   // 1B
    u8_t  flags;                   // 1B
    /* tcp specific */
    u32_t rcv_nxt, rcv_wnd, rcv_ann_wnd, rcv_ann_right_edge; // 16B
    u32_t snd_nxt, snd_wl1, snd_wl2; // 12B
    u32_t snd_max, snd_lbb;        // 8B
    u16_t snd_wnd, snd_wnd_max;    // 4B
    u16_t mss, advtsd_mss;         // 4B
    u8_t  rtt, rttest, rtseq;      // 3B
    u8_t  dupacks, nrtx;           // 2B
    u8_t  flags2, polltmr;         // 2B
    struct tcp_seg *unacked, *unsent; // 16B
    /* callbacks: 4-6 fn pointers */     // 40B
    void *callback_arg;                  // 8B
    u32_t keepalive;                     // 4B
    u8_t  ka_probes_sent;                // 1B
};
```

Total: ~120-160B [~]. Designed for embedded; commonly runs 100s-1000s of TCP conns in 1MB of RAM.

**This is the absolute floor for "pure state, no execution" tasks.** Glide can't beat this without abandoning the cooperative model.

### Erlang process

From BEAM internals (`erts/emulator/beam/erl_process.h`):

`Process` struct ~340B [~]:
- ID + flags: 32B
- Heap pointers (htop, hend, stop): 24B
- Initial heap: 233 words = ~1.8KB [~]
- Mailbox: list head/tail + lock
- Monitor refs, link list
- Group leader, dictionary
- Stack trace info
- Reduction counter

Per-process: ~2KB total (PCB + initial heap).

Per-process heap → process GC. Tail latency wins; allocation cost per process.

Erlang has been benchmarked at 1M+ processes per node (production WhatsApp servers).

### FreeRTOS TCB

From FreeRTOS source (`tasks.c`):

`TCB_t` struct:
- StackPointer + StackBase: 8-16B
- Priority info: 2-4B
- Event list links: 16-32B
- Mutex held list: 16B
- Task name: ~16B (configurable)
- Runtime counters (if enabled): 8-16B

Total: ~100-200B + stack (256B minimum, often 1-2KB).

FreeRTOS routinely runs 50-100 tasks in ~64KB total RAM.

### Summary table

| System | Per-task footprint | Stack? | Notes |
|---|---|---|---|
| Linux pthread | 8MB virt + 8KB kernel | yes, fixed | Doesn't scale |
| Go goroutine | 2.6KB total (~600B G + 2KB stack) | yes, growable | Tail latency from GC |
| Tokio task | 100-400B (header + future) | no | Function coloring |
| Erlang process | ~2KB (PCB + heap) | yes, copyable | Per-proc GC |
| Pony actor | 232B | no (state machine) | Per-actor heap |
| LWIP tcp_pcb | 120-160B | no (event-driven) | Embedded-only |
| FreeRTOS TCB | 100-200B + stack | yes, fixed | Small embedded |
| **Glide TODAY** | **64KB stack + ~280B task** | yes, fixed | Phase A: 2KB |
| **Glide PHASE B** | **~150-300B (leaf)** | varies | Target |
| **Glide PHASE D** | **~100-150B (leaf)** | varies | World-class |

## 3 — Stack growth strategies

### Go's morestack + copystack

Source: Go's `runtime/stack.go` and `runtime/asm_amd64.s`.

Function prologue (emitted by compiler for every Go fn that needs more
than a few bytes of stack):

```asm
; x86-64 prologue check
MOVQ TLS, AX                  ; get TLS base
MOVQ runtime·g(AX), CX        ; load current g pointer
MOVQ g_stackguard0(CX), DX    ; load stackguard
CMPQ SP, DX                   ; compare SP to guard
JLS  growstack                ; if SP <= guard, grow
SUBQ $framesize, SP           ; allocate frame
```

Cost: ~3-5 instructions per fn entry [~]. Measurable but not dominant.

`runtime.morestack` (called when prologue check fails):
1. Save current frame to old stack.
2. Switch to `g0`'s stack (system stack).
3. Call `runtime.newstack`.

`runtime.newstack`:
1. Allocate new stack region: `2 * oldsize`, capped at `MaxStack` (1GB by default in Go).
2. Memcpy old contents to new.
3. **Pointer adjustment**: walk new stack frames, find any 8-byte
   value that lies in `[old_lo, old_hi)`, add `delta = new_lo - old_lo`
   to it. This handles:
   - Return addresses (saved RIP).
   - Frame pointers (saved RBP).
   - Pointers from heap into stack — Go's GC marks these as
     "stack-pointing" and walks them too.
4. Update `g.stack = {new_lo, new_hi}`, `g.stackguard0 = new_lo + StackGuard`.
5. Resume execution on new stack with adjusted SP.

**Edge cases**:
- Pointers passed to C via cgo: Go forbids them by default. Or
  caller must pin.
- `unsafe.Pointer` from heap to stack: detected by GC, fixed up.
- Inline asm doing weird things: prohibited.

For Glide:
- We compile to C. The C compiler controls function prologue, NOT
  us. So we can't insert the stackguard check directly.
- Workaround 1: emit our own prologue via inline asm in every fn.
  Heavy.
- Workaround 2: insert a manual check at each "long-running" point
  (loops, calls). Like Go's old approach pre-1.4.
- Workaround 3: use the C compiler's stack-checking option:
  `gcc -fstack-check` or per-fn pragma. But that aborts on overflow,
  doesn't grow. So no good.
- Workaround 4: catch SIGSEGV on guard page, grow stack in handler.
  This is what we'll use. Trickier than Go's compiler-emitted
  approach but doesn't require a custom compiler pass.

**Decision for Phase A**: SIGSEGV-on-guard-page approach. Stack
starts 2KB, grows on fault.

### Split / segmented stacks (abandoned)

GCC's `-fsplit-stack` (~2011): each fn calls
`__morestack` which allocates new chunk if stack full. Chunks not
contiguous.

Problems:
- Calling non-split-stack code (libc) requires switching to a "large"
  stack. Trampolines everywhere.
- Cache misses when stack split across multiple chunks.
- Hard to debug (backtrace must walk chunks).

Abandoned. Go moved to monolithic growable stacks. Rust moved to
async/await + fixed pthread stacks.

### Stacklets (compiler-known bound)

For functions where compiler can prove max stack size (no recursion,
no dynamic allocation, no variadic), pre-allocate EXACTLY that much.

Applicable to: leaf-fn (Phase B target). If compiler knows the bound,
state struct allocation = exactly that size.

This is what Phase B does — leaf-fn state struct replaces stack
entirely.

## 4 — Stackless / state machine codegen

### Rust async fn lowering

Source: rustc generator/coroutine lowering pass.

Input:
```rust
async fn foo(x: i32) -> i32 {
    let a = bar(x).await;
    let b = baz(a).await;
    a + b
}
```

Output (conceptually):
```rust
enum FooState {
    Initial { x: i32 },
    AwaitingBar { bar_fut: BarFuture },
    AwaitingBaz { a: i32, baz_fut: BazFuture },
    Done,
}

struct Foo { state: FooState }

impl Future for Foo {
    type Output = i32;
    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<i32> {
        loop {
            match &mut self.state {
                Initial { x } => {
                    let bar_fut = bar(*x);
                    self.state = AwaitingBar { bar_fut };
                }
                AwaitingBar { bar_fut } => {
                    match Pin::new_unchecked(bar_fut).poll(cx) {
                        Pending => return Pending,
                        Ready(a) => {
                            let baz_fut = baz(a);
                            self.state = AwaitingBaz { a, baz_fut };
                        }
                    }
                }
                AwaitingBaz { a, baz_fut } => {
                    match Pin::new_unchecked(baz_fut).poll(cx) {
                        Pending => return Pending,
                        Ready(b) => {
                            self.state = Done;
                            return Ready(*a + b);
                        }
                    }
                }
                Done => panic!("polled after Ready"),
            }
        }
    }
}
```

Total size: `sizeof(largest variant) + discriminant byte + padding`.

For Glide, similar lowering BUT:
- No `Pin` needed (we control allocation; arena/pool stable).
- No waker via Context (scheduler tracks task directly).
- State machine driven by `task->step(task)` callback, not `poll(cx)`.

### Kotlin coroutines suspend lowering

Source: Kotlin compiler `CoroutineCompilerExtension`.

Input (Kotlin):
```kotlin
suspend fun foo(x: Int): Int {
    val a = bar(x)
    val b = baz(a)
    return a + b
}
```

Output (conceptual JVM bytecode shape):

A `Continuation<Int>` interface with `resumeWith(Result<Int>)`.

The fn body becomes a class:
```java
class Foo extends ContinuationImpl {
    int label;  // current state
    int x, a;   // captured locals
    @Override Object invokeSuspend(Object result) {
        switch (label) {
            case 0:
                label = 1;
                Object bar_result = bar(x, this);  // 'this' = continuation
                if (bar_result == COROUTINE_SUSPENDED) return COROUTINE_SUSPENDED;
                result = bar_result;
                // fall through
            case 1:
                a = (Integer) result;
                label = 2;
                Object baz_result = baz(a, this);
                if (baz_result == COROUTINE_SUSPENDED) return COROUTINE_SUSPENDED;
                result = baz_result;
            case 2:
                int b = (Integer) result;
                return a + b;
            default:
                throw new IllegalStateException();
        }
    }
}
```

Key insight: `suspend fun X()` becomes `X(Continuation)` — the
continuation is an EXTRA parameter. Synchronous callers from Kotlin
side don't see this; the compiler hides it.

For Glide: this is the syntax-invisible pattern we want. User writes
`fn foo(x: int) -> int { ... chan.recv() ... }`. Compiler decides
"this is a leaf fn at every spawn site that calls it" → lowers to
state machine. The user-facing fn signature stays the same.

### C++20 coroutines

`co_await`, `co_yield`, `co_return`. Compiler-generated state struct
(called "coroutine frame"). Heap-allocated by default; can be elided
to caller stack if escape analysis proves it doesn't outlive caller.

For Glide: elision is interesting. If `spawn leaf_fn(args)` is the
only place leaf_fn is called AND we can prove the spawn site outlives
the spawned task, we could allocate the state struct on the spawn
site's frame. Rare case but worth knowing.

### C# async/await

Compiler-generated state machine as `struct` (value type) wrapped in
a `Task<T>`. State struct lives on heap (boxed) if Task is awaited
across a suspend; lives on stack if all-synchronous-completion path.

Less applicable to Glide (we don't have boxed-vs-stack distinction
like .NET).

## 5 — Scheduler designs

### Work-stealing (Cilk, Go, Tokio)

Foundational paper: Blumofe & Leiserson, "Scheduling Multithreaded
Computations by Work Stealing", FOCS 1994.

Each worker has a local deque. Spawn pushes to local-bottom. Pop
from local-bottom (LIFO for the owner, FIFO for stealers).

When local empty, pick random victim, steal from victim-top.

Theoretical bound: with P workers, schedules an n-strand multithreaded
computation in O(T1/P + T∞) where T1 = sequential time, T∞ = critical
path length. Provably efficient.

Practical implementation tips (from Tokio v2 rewrite + Go scheduler):

- **Fixed-size local queue**: Tokio uses 256-slot ring; Go uses 256.
  When full, overflow to a global queue. Avoids unbounded local growth.
- **Random victim selection**: better than round-robin (avoids
  pathological cases).
- **Steal half**: when stealing, take half of victim's queue rather
  than one. Amortizes steal cost.
- **Global queue**: workers fall back to global if no steal target
  has work. Throttled so stealers don't starve.

### M:N vs 1:1

1:1 (Linux NPTL): each thread = kernel task. Simple. Blocking syscalls
just block the thread; another thread keeps running. Cost per thread
high (8MB virt + kernel TCB).

N:1: all user-space tasks on one kernel thread. Lightweight. Blocking
syscalls block ALL tasks. Bad for I/O-bound.

M:N: M user tasks on N kernel threads (typically N = num CPUs).
Best of both: cheap tasks + parallel I/O. But blocking syscalls block
the worker → workaround: spin off "I/O thread" or use async syscalls
exclusively.

Glide today: M:N with workers = NumCPUs. Blocking syscalls handled by
`spawn_thread` (separate pthread per blocking task).

### Cooperative yields vs preemption

Cooperative: tasks yield voluntarily on chan/sleep/I/O. Simple.
Drawback: a CPU-bound task can starve siblings.

Preemptive: scheduler interrupts running task at deadline. Complex.
Go 1.14+ adds async preemption via SIGURG (the runtime sends SIGURG
to a running G's M, which interrupts it).

Glide today: cooperative. Phase Z could add preemption if measurements
show starvation in real workloads. Not urgent.

### Tokio v2 specific notes

From the 2021 rewrite blog post + source:
- Each task has a state field (atomic u32) with bit-packed flags:
  RUNNING, NOTIFIED, COMPLETE, JOIN_WAKER, etc.
- Wake operation: CAS the state from `IDLE` to `NOTIFIED`, then
  q_push if was IDLE.
- This is the "state machine CAS" pattern that Glide's SM-1 should
  use.

## 6 — Lock-free queues

### Vyukov MPMC bounded

D. Vyukov's "MPMC bounded queue". Single CAS per op. Sequence number
per cell determines whether cell is empty/full/in-progress.

Glide's chan ring (chan.c.tmpl) already uses this. Already SOTA.

### LMAX Disruptor

Single-producer-single-consumer or multi-producer with batch sequence
allocation. Pre-allocated buffer, recycled slots. Memory barriers
instead of locks. Designed for fixed-allocation low-latency trading.

Tested at billions of msgs/sec on commodity hardware.

For Glide: applicable to scheduler queues. Phase C work.

### Treiber stack (with epoch/hazard pointers)

Lock-free LIFO via single CAS on head. ABA problem: between read of
head and CAS, address may be reused.

Mitigations:
- Hazard pointers: each thread declares which addresses it's using.
  Memory freed only when no thread declares it.
- Epoch-based reclamation: divide time into epochs; only reclaim
  memory from past epochs.
- Tagged pointers: 16 high bits of pointer = version counter.

Glide tried Treiber for chan wait list earlier and hit lost-wake races
(per `project_perf_next_state_machine.md`). The fix isn't Treiber
itself; it's recheck-after-push.

### Wait-free vs lock-free

Wait-free: every thread completes in bounded steps.
Lock-free: at least one thread makes progress.

Wait-free is stronger but more expensive. Lock-free is typically
enough for hot paths.

For Glide: lock-free suffices.

## 7 — Key papers + sources

Read-list for the next research session before Phase A:

1. Blumofe & Leiserson 1994 — "Scheduling Multithreaded Computations
   by Work Stealing". The foundational paper.
2. Russ Cox 2014 — "Go's work-stealing scheduler" blog post.
3. Vyukov's 1024cores.net blog — lock-free queue patterns.
4. Tokio blog 2021 — "Tokio internals: Understanding Rust's
   asynchronous I/O framework". Covers the v2 rewrite.
5. Rust RFC 2592 — async/await stabilization. RFC 2418 — Future
   trait design.
6. Clebsch, Drossopoulou 2015 — "Deny Capabilities for Safe, Fast
   Actors". Pony's actor design.
7. Go runtime source — `runtime/stack.go`, `runtime/runtime2.go`,
   `runtime/asm_amd64.s`. The `g` struct + morestack flow.
8. LWIP source — `tcp.h`, `tcp.c`. The 120B `tcp_pcb`.

## 8 — Glide-specific design decisions

For each open question, what Glide commits to:

### Q1 — Stack growth strategy

**Decision**: SIGSEGV-on-guard-page + 2x doubling. Max 8MB.

Rationale: We compile to C, so we can't emit Go-style prologue checks
without writing a C codegen plugin or using inline asm in every
function (too invasive). SIGSEGV approach:

- Allocate stack with guard page at bottom (last page protected).
- On guard fault: signal handler computes new size = 2x current,
  allocates, memcpy, fixes pointers, returns.
- Cost: faults are rare (only on growth).
- Downside: signal handler is non-trivial; need to be reentrant.

Alternative considered: trampolines that check explicitly. Rejected
because it adds runtime overhead even when no growth needed.

### Q2 — State struct layout for leaf-fn

**Decision**: Tagged union of per-state live-var sets.

`struct LeafFnState { uint8_t state; union { S0 s0; S1 s1; S2 s2; } u; };`

Size = `sizeof(largest_state_variant) + 1 + padding`.

Live-var analysis:
- At each yield point, compute the set of locals that are referenced
  after the yield.
- Group these into per-state structs.
- Variables not live across any yield go into per-block stack-locals
  (don't need state storage).

This is the standard Rust async / Kotlin pattern. Already proven.

### Q3 — Leaf-fn detection

**Decision**: Conservative call-graph analysis.

A fn is leaf iff:
1. No direct or transitive self-recursion.
2. All callees are either:
   - Other leaf-marked fns.
   - Builtins in the "async-safe leaf" set.
3. No callees that need a stack (`malloc`, deep recursion, etc).

Initial async-safe leaf set (start narrow, expand):
- `chan.send/recv/close/recv_into`
- `select!`
- `sleep_ms`, `time_now_ns`
- `tcp_read_async`, `tcp_write_async`, `tcp_writev2_async`
- `accept_tcp_async` (Phase A reactor work makes this real)

Builtins like `println!`, `malloc`, `free` are NOT leaf-safe (they're
either I/O-blocking or might recurse).

Generics: if a leaf-fn is generic, each monomorphization must also
qualify. Reject if any monomorphization has non-leaf callee.

### Q4 — Where state struct lives

**Decision**: Pool-allocated in per-worker slab caches.

Slab = 4KB page. ~30-40 state structs per slab.

Each worker has private slab cache → no cross-thread contention on
allocation. When cache empty, fetch fresh slab from global allocator.

Per-task: ~3-5B amortized for slab management. Acceptable.

Bit-pack: low 3 bits of `task->next` pointer can encode state /
type flags (pointer is 8-byte aligned).

### Q5 — Scheduler stackful vs stateless dispatch

**Decision**: Single `task` type with a tag bit, two execution paths.

```c
struct __glide_task {
    struct __glide_task* next;   // bit 0 = is_stateless
    uint32_t state;              // RUNNING, READY, BLOCKED, DONE
    int home_worker;
    union {
        struct { /* stackful: ctx, stack_base, stack_size */ } sf;
        struct { void (*step)(struct __glide_task*); /* state struct inline */ } sl;
    };
};
```

Scheduler:
```c
if (task->next & 1) {
    task->sl.step(task);
} else {
    __glide_ctx_switch(&worker_ctx, &task->sf.ctx);
}
```

### Q6 — Wakes for stateless tasks

**Decision**: scheduler directly re-enqueues.

When a chan.recv inside a stateless task would block, the task's
step() function:
1. Marks the chan as "this task is the waiter".
2. Returns `YIELD`.

Scheduler doesn't dispatch task again until chan signals back via
direct `q_push_to(task->home_worker, task)`.

No Future/Waker indirection. Direct task pointer.

### Q7 — Function coloring (invisible)

**Decision**: User never sees "leaf" vs "stackful".

Same `fn`, same `spawn`. Compiler picks per spawn-site based on
target fn's leaf-status. Default: most fns auto-detected as leaf in
typical I/O-bound code.

Escape hatch (rarely needed): `@stackful` attribute forces stackful
representation. Use case: fn does recursion but user wants spawn-able
via the macro `@stackful fn dfs(n: int)`.

This is THE differentiator vs Rust async. Same code shape as Go;
performance closer to Rust async; user pays nothing in syntax.

### Q8 — Preemption

**Decision**: NOT in initial scope. Cooperative only.

Glide's typical workload (HTTP servers, CLI tools, pipelines) is I/O
or chan-driven. CPU-bound tasks that don't yield are rare.

If a real workload starves, add SIGURG-based preemption (Go 1.14+
style). Phase Y (future), not in the 100B plan.

### Q9 — Backtraces for stateless tasks

**Decision**: Pseudo-frame info emitted alongside state struct.

When a stateless task panics or is in the debugger, we need a
backtrace. The state struct doesn't have a traditional stack of
frames.

Compiler emits a parallel "state-to-source-location" table per leaf
fn. Backtrace walker checks task type:
- stackful: walk SP/FP via dbghelp/addr2line (as today).
- stateless: look up task->step's fn name + current state in table.

### Q10 — Cancellation

**Decision**: External cancel via task handle. Sync cleanup if state
machine handles it.

`cancel(handle)` sets task->state = CANCELLING. On next step() call,
task can observe and clean up. If task is parked, scheduler wakes it
into CANCELLING state.

Stackful coros: same model. The coro must observe `cur_task->state`
in long-running loops (cooperative cancel).

Not in initial Phase B scope. Phase E (future).

## 9 — Decision tree per phase

### Phase A (growable stack)
- Initial stack size: 2KB.
- Growth: 2x doubling. Max 8MB.
- Pointer fixup: conservative scan (any 8B value in [old_lo, old_hi)
  gets +delta).
- Fault handling: SIGSEGV (POSIX) / VEH (Windows) catches guard-page
  hit; grow + retry.
- Shrink: not initially. (Add Phase A' later if needed.)

### Phase B (leaf-fn SM codegen)
- Detection: call-graph DFS from each `spawn` site.
- Codegen: tagged union state struct.
- Yield points: hardcoded async-safe builtin set; expand by demand.
- Spawn-site decision: compile-time only (no runtime fallback).

### Phase C (scheduler micro-opt)
- Queue: Vyukov MPMC, 1024 slots per worker.
- Overflow: global queue with mutex.
- Steal: random victim, half-take.
- Batch push: when main accumulates K=8, push as one CAS.

### Phase D (packing + shrink)
- Slab: 4KB pages.
- Bit-pack: state tag in low bits of next ptr.
- Waker: eliminate. Direct task ptr.
- Final shrink optimizations: variant union sharing across phases of
  same fn, struct field reorder for packing.

## 10 — Known unknowns + questions

These need answers from real source reading or implementation
exploration. Capture them so the next research session has clear
targets.

1. **Borrow checker through yields**: How does the borrow analyzer
   handle a `&mut local` whose lifetime spans a `chan.recv()` yield?
   In Rust, this is the `Pin` requirement. In Glide, since arena +
   pool give stable addresses, do we even need a special rule?
   *Action*: examine current `record_borrow` flow in typer.glide for
   yield interactions. Document.

2. **Generic monomorphization explosion**: How many distinct leaf-fn
   monomorphizations does a typical real Glide program produce?
   *Action*: instrument the compiler to count, run against examples/
   on master. Aim for <100 distinct per program.

3. **Tail-call optimization for spawn**: Can `spawn leaf_fn(args)` at
   end of a stackful coro replace the coro (no alloc/free pair)?
   *Action*: study cilk's tail-call-spawn paper.

4. **Cancellation propagation**: When a parent task cancels, do
   children inherit the cancel? What if children are stateless and
   the cancel arrives between steps?
   *Action*: study structured concurrency literature (Trio,
   Kotlin's CoroutineScope).

5. **Stack-walk for backtraces**: Concrete format for the
   state-to-source table. Per-fn cost in binary size?
   *Action*: prototype the emit format. Aim <100B per leaf fn.

6. **Cross-language interop**: Glide compiles to C, can call libc.
   How do stackless leaf fns call libc fns that block? They mustn't
   (block the worker). Today same problem exists for spawn_thread
   fallback.
   *Action*: enforce in compiler — leaf fn can't call non-leaf
   builtin or non-async libc.

7. **Memory profiling**: Does Glide's allocator support cheap
   per-task RSS attribution? Need to know which leaf fns leak.
   *Action*: extend arena tracking to record per-task arena head.

## 11 — Bench targets for each phase

| Phase | Metric | Target |
|---|---|---|
| Baseline (today) | spawn 1M coros, RSS | ~64GB (impossible — show fails OOM) |
| Phase A | spawn 1M coros, RSS | <4GB (4KB avg per coro) |
| Phase B | spawn 1M leaf tasks, RSS | <500MB (500B avg) |
| Phase C | spawn cost ns/op | <50ns |
| Phase D | spawn 1M leaf tasks, RSS | <150MB (150B avg) |
| **Marketing target** | **1M leaf tasks, RSS** | **<150MB** |

For each phase, the bench harness (R2 deliverable) runs:
1. spawn N tasks all parked on a chan recv.
2. Measure RSS via getrusage / GetProcessMemoryInfo.
3. Report MB and bytes-per-task.

Variance must be <10% across N=10 runs (with pinned workers).

## 12 — What we did NOT decide (yet)

- **Garbage collection**: Glide doesn't have one. Arenas + auto-drop
  handle most. The 100B plan doesn't introduce GC. If we ever do,
  it's per-task heap (like Pony / Erlang), not stop-the-world.

- **Distributed concurrency**: out of scope. Glide is single-process
  for now.

- **Real-time guarantees**: out of scope. We optimize for throughput
  + tail latency, not hard deadlines.

- **Wasm target**: out of scope for the 100B plan. Wasm has its own
  threading model that doesn't fit M:N cleanly.

## 13 — Reading next session

When user comes back for R2 (bench harness), this doc should already
be on their lap. They should re-read sections 8 (decisions), 10
(unknowns), 11 (targets). Anything they disagree with is decided
HERE before any code lands.

When user comes back for Phase A (growable stack), they read sections
3 (stack growth) and 8.Q1 + 9. Then we implement.

When user comes back for Phase B (leaf-fn codegen), they read
sections 4 (state machine codegen) and 8.Q2-Q7.

This doc is the lap-top reference. It avoids us re-deriving design
from scratch every session.

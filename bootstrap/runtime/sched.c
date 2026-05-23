// ============================ scheduler runtime ============================
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
/* winsock2.h must precede windows.h on MinGW / mingw-w64; the seed's
   socket runtime later includes winsock2.h, so include it here too to
   keep the order well-defined regardless of which template lands first
   in the emitted bootstrap.c. */
# include <winsock2.h>
# include <windows.h>  /* GetSystemInfo / Sleep / VirtualAlloc */
#else
# include <unistd.h>
# include <sys/mman.h>
#endif

/* Spinlock — 5-10× faster than pthread_mutex for the short critical
   sections we use (queue push/pop, pool head update). Falls back to
   `pause` on contention to be hyperthread-friendly. */
typedef atomic_int __glide_spin_t;
static inline void __glide_spin_lock(__glide_spin_t* l) {
    while (atomic_exchange_explicit(l, 1, memory_order_acquire) == 1) {
        while (atomic_load_explicit(l, memory_order_relaxed) == 1) {
#if defined(__x86_64__) || defined(_M_X64)
            __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(_M_ARM64)
            __asm__ __volatile__("yield" ::: "memory");
#endif
        }
    }
}
static inline void __glide_spin_unlock(__glide_spin_t* l) {
    atomic_store_explicit(l, 0, memory_order_release);
}

/* Stack defaults: configured size + 4 KB guard, page-rounded up.
   With the default 4 KB the total reservation is 8 KB virtual per coro.
   Linux/Mac mmap commits pages lazily; Windows commits virtual but the
   working-set is bound only on first access. Either way idle coros pay
   ~one 4 KB page in physical RAM.
   Override via GLIDE_CORO_STACK (bytes). Real growable stacks (Go-style)
   need pointer-map metadata + copy/relocate — TBD. */
#define __GLIDE_STACK_GUARD 4096
/* 4 KiB was enough for the original micro-benchmarks but any real
   handler (HTTP request parser, JSON encode, .concat chains) easily
   spills past it and crashes on the guard page. 64 KiB matches Go's
   initial goroutine stack and costs ~64 KiB of virtual + a single
   page of physical per idle coro on Linux/macOS. */
static int __glide_stack_size = 65536;

/* Custom assembly context switch — replaces Win32 Fibers and POSIX
   ucontext.h with our own portable, ABI-correct register flip. The
   reason: Fibers crash if the same fiber is SwitchToFiber'd by
   different OS threads over its lifetime, which forces us to pin
   coros to a single worker and rules out work-stealing. With our
   own switch we own the entire state, so a coro can move between
   workers safely.

   Layout: callee-saved GP regs + (Win64 only) callee-saved XMM6-15.
   Caller-saved registers (rax/rcx/rdx/...) were already flushed to the
   caller's stack frame by the compiler before it called us, so we don't
   touch them. SysV marks XMM as caller-saved → no extra work there. */
typedef struct {
#if defined(__aarch64__) || defined(_M_ARM64)
    /* AArch64 (AAPCS64): callee-saved x19-x28, fp (x29), lr (x30),
       d8-d15 (low 64 of v8-v15). sp goes through a GP reg via mov. */
    void* sp;
    void* x19; void* x20; void* x21; void* x22;
    void* x23; void* x24; void* x25; void* x26;
    void* x27; void* x28;
    void* fp;            /* x29 */
    void* lr;            /* x30 - first switch returns into trampoline */
    double d8;  double d9;  double d10; double d11;
    double d12; double d13; double d14; double d15;
#elif defined(_WIN32)
    void* rsp; void* rbx; void* rbp;
    void* rdi; void* rsi;
    void* r12; void* r13; void* r14; void* r15;
    char xmm[160];
#else
    void* rsp; void* rbx; void* rbp;
    void* r12; void* r13; void* r14; void* r15;
#endif
} __glide_coro_ctx;

__attribute__((naked, noinline))
static void __glide_ctx_switch(
    __glide_coro_ctx* from __attribute__((unused)),
    __glide_coro_ctx* to   __attribute__((unused))) {
#if defined(__aarch64__) || defined(_M_ARM64)
    /* AAPCS64: from=x0, to=x1. Save sp through x9 (sp can't be used as
       a base register in stp). stp pairs 16-byte-aligned doublets. */
    __asm__(
        "mov x9, sp\n\t"
        "str x9,  [x0,   #0]\n\t"
        "stp x19, x20, [x0,  #8]\n\t"
        "stp x21, x22, [x0, #24]\n\t"
        "stp x23, x24, [x0, #40]\n\t"
        "stp x25, x26, [x0, #56]\n\t"
        "stp x27, x28, [x0, #72]\n\t"
        "stp x29, x30, [x0, #88]\n\t"
        "stp  d8,  d9, [x0, #104]\n\t"
        "stp d10, d11, [x0, #120]\n\t"
        "stp d12, d13, [x0, #136]\n\t"
        "stp d14, d15, [x0, #152]\n\t"
        "ldr x9,  [x1,   #0]\n\t"
        "mov sp, x9\n\t"
        "ldp x19, x20, [x1,  #8]\n\t"
        "ldp x21, x22, [x1, #24]\n\t"
        "ldp x23, x24, [x1, #40]\n\t"
        "ldp x25, x26, [x1, #56]\n\t"
        "ldp x27, x28, [x1, #72]\n\t"
        "ldp x29, x30, [x1, #88]\n\t"
        "ldp  d8,  d9, [x1, #104]\n\t"
        "ldp d10, d11, [x1, #120]\n\t"
        "ldp d12, d13, [x1, #136]\n\t"
        "ldp d14, d15, [x1, #152]\n\t"
        "ret\n\t"
    );
#elif defined(_WIN32)
    /* Win64 ABI: from=%rcx, to=%rdx. XMM area starts at offset 72. */
    __asm__(
        "movq %rsp,   0(%rcx)\n\t"
        "movq %rbx,   8(%rcx)\n\t"
        "movq %rbp,  16(%rcx)\n\t"
        "movq %rdi,  24(%rcx)\n\t"
        "movq %rsi,  32(%rcx)\n\t"
        "movq %r12,  40(%rcx)\n\t"
        "movq %r13,  48(%rcx)\n\t"
        "movq %r14,  56(%rcx)\n\t"
        "movq %r15,  64(%rcx)\n\t"
        "movdqu %xmm6,   72(%rcx)\n\t"
        "movdqu %xmm7,   88(%rcx)\n\t"
        "movdqu %xmm8,  104(%rcx)\n\t"
        "movdqu %xmm9,  120(%rcx)\n\t"
        "movdqu %xmm10, 136(%rcx)\n\t"
        "movdqu %xmm11, 152(%rcx)\n\t"
        "movdqu %xmm12, 168(%rcx)\n\t"
        "movdqu %xmm13, 184(%rcx)\n\t"
        "movdqu %xmm14, 200(%rcx)\n\t"
        "movdqu %xmm15, 216(%rcx)\n\t"
        "movq  0(%rdx), %rsp\n\t"
        "movq  8(%rdx), %rbx\n\t"
        "movq 16(%rdx), %rbp\n\t"
        "movq 24(%rdx), %rdi\n\t"
        "movq 32(%rdx), %rsi\n\t"
        "movq 40(%rdx), %r12\n\t"
        "movq 48(%rdx), %r13\n\t"
        "movq 56(%rdx), %r14\n\t"
        "movq 64(%rdx), %r15\n\t"
        "movdqu  72(%rdx), %xmm6\n\t"
        "movdqu  88(%rdx), %xmm7\n\t"
        "movdqu 104(%rdx), %xmm8\n\t"
        "movdqu 120(%rdx), %xmm9\n\t"
        "movdqu 136(%rdx), %xmm10\n\t"
        "movdqu 152(%rdx), %xmm11\n\t"
        "movdqu 168(%rdx), %xmm12\n\t"
        "movdqu 184(%rdx), %xmm13\n\t"
        "movdqu 200(%rdx), %xmm14\n\t"
        "movdqu 216(%rdx), %xmm15\n\t"
        "ret\n\t"
    );
#else
    /* SysV AMD64 ABI: from=%rdi, to=%rsi */
    __asm__(
        "movq %rsp,   0(%rdi)\n\t"
        "movq %rbx,   8(%rdi)\n\t"
        "movq %rbp,  16(%rdi)\n\t"
        "movq %r12,  24(%rdi)\n\t"
        "movq %r13,  32(%rdi)\n\t"
        "movq %r14,  40(%rdi)\n\t"
        "movq %r15,  48(%rdi)\n\t"
        "movq  0(%rsi), %rsp\n\t"
        "movq  8(%rsi), %rbx\n\t"
        "movq 16(%rsi), %rbp\n\t"
        "movq 24(%rsi), %r12\n\t"
        "movq 32(%rsi), %r13\n\t"
        "movq 40(%rsi), %r14\n\t"
        "movq 48(%rsi), %r15\n\t"
        "ret\n\t"
    );
#endif
}

typedef void* (*__glide_task_fn)(void*);
typedef struct __glide_task {
    __glide_coro_ctx    ctx;     /* register save area */
    void*               stack;   /* mmap/VirtualAlloc base (low addr) */
    size_t              stack_total; /* bytes including guard page */
    __glide_task_fn     entry;
    void*               arg;
    int                 state; /* 0=ready 1=running 2=blocked 3=done */
    int                 home_worker; /* worker that currently owns this coro;
                                       updated on steal so future unparks land
                                       on the thief's queue (cache-warm). */
    int                 has_run;     /* 0 if never run; once 1, we never
                                       migrate the coro across OS threads (Win64
                                       SEH/TIB invariants make resumed-on-other-
                                       thread fragile, so first-run-only steal). */
    void*               palloc_arena; /* per-coro active arena slot. Wired
                                         through __glide_task_arena_get/set;
                                         keeps cooperative concurrency from
                                         scribbling on each other's
                                         __glide_palloc_active globally. */
    struct __glide_task* next;       /* link in per-worker ready queue */
    struct __glide_task* wait_next;  /* link in chan wait list */
    /* Park hand-off: if non-null on switch-back to worker fiber,
       worker links self into *park_list and unlocks park_lock.
       Done after the switch so unpark can never race with a
       still-mid-switch coro. Either park_lock OR park_spin is set
       per park call, not both — the I/O reactor uses a spinlock per
       fd because the critical sections are 5-10 ns and futex-backed
       pthread_mutex was 2.5 % of the keep-alive HTTP hot path. */
    pthread_mutex_t*     park_lock;
    __glide_spin_t*      park_spin;
    struct __glide_task** park_list;
} __glide_task;

/* Forward decls — out-of-order references between sleep_ms / __glide_park /
   __glide_timer_main / sched_init / task pool helpers used by worker_main. */
static void* __glide_timer_main(void* unused);
int __glide_park(pthread_mutex_t* lock, __glide_task** list);
void __glide_free_task(__glide_task* t);
static void __glide_reset_ctx(__glide_task* t);
static void __glide_flush_main_buf(void);

/* Per-worker queue: each worker pops only from its own queue, so a
   fiber that first ran on worker A always continues on A. This avoids
   cross-thread fiber migration which crashes Win32 Fibers. Stealing
   between workers comes in Phase 1.2 (with proper atomic ownership). */
typedef struct {
    __glide_spin_t  spin;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    __glide_task*   head;
    __glide_task*   tail;
    atomic_int      idle;
} __glide_wq;

/* Sorted sleep queue for the timer thread. Coroutines that call
   sleep_ms park here instead of blocking their worker. */
typedef struct __glide_timer_node {
    long long              deadline_ns;
    __glide_task*          task;
    struct __glide_timer_node* next;
} __glide_timer_node;
static __glide_timer_node* __glide_timer_head = NULL;
static pthread_mutex_t __glide_timer_mu;
static pthread_cond_t  __glide_timer_cv;
static pthread_t __glide_timer_thread;
static int __glide_timer_inited = 0;

static __glide_wq* __glide_wqs = NULL;
static int __glide_q_inited = 0;
static int __glide_n_workers = 0;
static pthread_t* __glide_workers = NULL;
/* Pending count, sharded per worker so increments and decrements don't all bounce
   the same cache line. Spawn increments shards[home_worker]; the worker that runs
   the task decrements shards[my_worker]. Sum across shards = total alive tasks.
   Each shard is its own cache line to kill false sharing between threads. */
typedef struct {
    atomic_int v;
    char _pad[60];
} __glide_pending_shard_t;
static __glide_pending_shard_t* __glide_pending_shards = NULL;
static int __glide_pending_n_shards = 0;
static int __glide_pending_sum(void) {
    int sum = 0;
    int n = __glide_pending_n_shards;
    for (int i = 0; i < n; i++) {
        sum += atomic_load_explicit(&__glide_pending_shards[i].v, memory_order_acquire);
    }
    return sum;
}
static atomic_int __glide_shutdown = 0;
static atomic_int __glide_rr = 0;        /* round-robin spawn counter */

static _Thread_local __glide_task* __glide_cur_task = NULL;
static _Thread_local int __glide_my_worker = -1;
/* Worker's saved context. Each OS thread has its own — when a coro
   parks/yields, we ctx_switch INTO this so the worker resumes its
   loop right after the call site. */
static _Thread_local __glide_coro_ctx __glide_worker_ctx;
#ifdef _WIN32
/* Original OS-thread stack range — saved on worker entry, restored
   when no coro is running. Win64 SEH walks the stack via TIB, so we
   must point TIB at the coro's stack while it runs and back at the
   OS thread's stack between switches. Without this, any code path that
   raises an exception or probes the stack from a migrated coro reads
   garbage and crashes. */
static _Thread_local void* __glide_orig_stack_base = NULL;
static _Thread_local void* __glide_orig_stack_limit = NULL;
static inline void __glide_tib_set_stack(void* base, void* limit) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    tib->StackBase = base;
    tib->StackLimit = limit;
}
#endif

/* Perf counters defined further below; forward-declare here so the
   queue push helpers can bump them. */
extern atomic_long __glide_perf_q_pushes;
extern atomic_long __glide_perf_cv_signals;

static void __glide_q_push_to(int wid, __glide_task* t) {
    __glide_wq* q = &__glide_wqs[wid];
    __glide_spin_lock(&q->spin);
    t->next = NULL;
    if (q->tail) q->tail->next = t; else q->head = t;
    q->tail = t;
    __glide_spin_unlock(&q->spin);
    atomic_fetch_add_explicit(&__glide_perf_q_pushes, 1, memory_order_relaxed);
    if (atomic_load_explicit(&q->idle, memory_order_relaxed)) {
        pthread_mutex_lock(&q->mu);
        pthread_cond_signal(&q->cv);
        pthread_mutex_unlock(&q->mu);
        atomic_fetch_add_explicit(&__glide_perf_cv_signals, 1, memory_order_relaxed);
    }
}

/* Splice a pre-built chain (head ... tail, n nodes, NULL-terminated) onto
   the back of a worker's queue under one spinlock acquisition. Used by main's
   batched-push path so 32 spawns cost ~1 lock instead of 32. */
static void __glide_q_push_chain(int wid, __glide_task* head, __glide_task* tail, int n) {
    if (!head) return; (void)n;
    __glide_wq* q = &__glide_wqs[wid];
    __glide_spin_lock(&q->spin);
    if (q->tail) q->tail->next = head; else q->head = head;
    q->tail = tail;
    __glide_spin_unlock(&q->spin);
    if (atomic_load_explicit(&q->idle, memory_order_relaxed)) {
        pthread_mutex_lock(&q->mu);
        pthread_cond_broadcast(&q->cv);
        pthread_mutex_unlock(&q->mu);
    }
}

/* Sticky preferred worker for OS threads that aren't part of the M:N
   pool. Main thread gets the first slot (W0); subsequent spawn_thread
   callers (accept loops in http_listen_workers, for example) round-
   robin over the rest so they don't all dogpile W0's spinlock + cv. */
static _Thread_local int __glide_outside_pref_worker = -1;
static atomic_int        __glide_next_outside_pref   = 0;

static int __glide_pick_worker(void) {
    /* Inside a coro: same worker as caller (cheap, keeps cache hot). */
    if (__glide_cur_task != NULL && __glide_my_worker >= 0) return __glide_my_worker;
    /* Outside: stick to one worker per OS thread. Single-main-thread
       case still resolves to W0 (it asks first), so the well-trodden
       hot path is unchanged. The win shows up when there are 2+
       non-coro spawners in the same process. */
    if (__glide_outside_pref_worker < 0) {
        int next = atomic_fetch_add_explicit(&__glide_next_outside_pref, 1,
                                             memory_order_relaxed);
        int n = __glide_n_workers;
        if (n <= 0) n = 1;
        __glide_outside_pref_worker = next % n;
    }
    return __glide_outside_pref_worker;
}

/* Work-stealing: when our queue is empty, try grabbing the head of
   another worker's queue (random victim, walk all on miss). On steal
   we adopt the task as our own (home_worker = me) so future unparks
   stay cache-warm here. Now safe to migrate because our custom ctx
   switch is stateless across threads — no Win32 Fiber crash trap. */
static atomic_uint __glide_steal_rr = 0;
static __glide_task* __glide_try_steal(void) {
    int n = __glide_n_workers;
    if (n <= 1) return NULL;
    int my = __glide_my_worker;
    unsigned int seed = atomic_fetch_add_explicit(&__glide_steal_rr, 1, memory_order_relaxed);
    int start = (int)(seed % (unsigned int)n);
    for (int i = 0; i < n; i++) {
        int v = (start + i) % n;
        if (v == my) continue;
        __glide_wq* q = &__glide_wqs[v];
        __glide_spin_lock(&q->spin);
        /* Walk queue head: only first-run-untouched tasks are stealable. */
        __glide_task* prev = NULL;
        __glide_task* t = q->head;
        while (t && t->has_run) { prev = t; t = t->next; }
        if (t) {
            if (prev) prev->next = t->next; else q->head = t->next;
            if (q->tail == t) q->tail = prev;
            __glide_spin_unlock(&q->spin);
            t->home_worker = my;
            t->next = NULL;
            return t;
        }
        __glide_spin_unlock(&q->spin);
    }
    return NULL;
}

static __glide_task* __glide_q_pop_my(void) {
    __glide_wq* q = &__glide_wqs[__glide_my_worker];
    while (!atomic_load_explicit(&__glide_shutdown, memory_order_relaxed)) {
        __glide_spin_lock(&q->spin);
        __glide_task* t = q->head;
        if (t) {
            q->head = t->next;
            if (q->head == NULL) q->tail = NULL;
            __glide_spin_unlock(&q->spin);
            return t;
        }
        __glide_spin_unlock(&q->spin);
        __glide_task* stolen = __glide_try_steal();
        if (stolen) return stolen;
        pthread_mutex_lock(&q->mu);
        if (q->head || atomic_load(&__glide_shutdown)) {
            pthread_mutex_unlock(&q->mu);
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 500000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        atomic_store_explicit(&q->idle, 1, memory_order_relaxed);
        pthread_cond_timedwait(&q->cv, &q->mu, &ts);
        atomic_store_explicit(&q->idle, 0, memory_order_relaxed);
        pthread_mutex_unlock(&q->mu);
    }
    return NULL;
}

/* Trampoline reached on a coro's first switch-in. Reads the bound
   task from TLS (worker_main set __glide_cur_task before switching),
   runs the user fn, marks done, then hands control back to the worker
   via ctx_switch. The final switch never returns: we discard the saved
   ctx (worker is going to free us anyway). */
static void __glide_coro_trampoline(void) {
    __glide_task* t = __glide_cur_task;
    (void)t->entry(t->arg);
    t->state = 3;
    __glide_ctx_switch(&t->ctx, &__glide_worker_ctx);
    __builtin_unreachable();
}

/* Stack pool: hold a small free list of recently-freed regions so a
   spawn-burst doesn't pay mmap+munmap (Linux) / VirtualAlloc+VirtualFree
   (Win) per coro. Significant on Windows where these are slow syscalls. */
typedef struct __glide_stack_node {
    void*  base;
    size_t total;
    struct __glide_stack_node* next;
} __glide_stack_node;
static __glide_stack_node* __glide_stack_pool = NULL;
static int __glide_stack_pool_count = 0;
static const int __GLIDE_STACK_POOL_MAX = 16384;
static __glide_spin_t __glide_stack_pool_spin = 0;

/* Stack allocator: mmap (POSIX) or VirtualAlloc (Win) so unused pages
   stay uncommitted (Linux/Mac) and overflow into the guard page raises
   SIGSEGV/EXCEPTION_ACCESS_VIOLATION instead of silently corrupting the
   neighbour stack. Returns the LOW address of the whole region; usable
   stack starts at `base + __GLIDE_STACK_GUARD`. */
/* Cached page size for the running host. Apple Silicon uses a native
   16 KB page; x86_64 Linux/Win/Mac all use 4 KB. mmap/mprotect/VirtualAlloc
   round to whole pages, so any rounding done here MUST match the kernel's
   page or guard-page protection ends up covering the wrong region (or
   straddling two stacks at once). Looked up once on first call; harmless
   data race for the racy first writes since both threads converge on the
   same constant. */
static size_t __glide_host_page_size(void) {
    static size_t cached = 0;
    if (cached) return cached;
    size_t p = 4096;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (si.dwPageSize > 0) p = (size_t)si.dwPageSize;
#else
    long sc = sysconf(_SC_PAGESIZE);
    if (sc > 0) p = (size_t)sc;
#endif
    cached = p;
    return p;
}

static void* __glide_alloc_stack(size_t* out_total) {
    size_t total = (size_t)__glide_stack_size + __GLIDE_STACK_GUARD;
    size_t page = __glide_host_page_size();
    total = (total + page - 1) & ~(page - 1);
    /* Pool fast path. */
    __glide_spin_lock(&__glide_stack_pool_spin);
    __glide_stack_node* n = __glide_stack_pool;
    while (n && n->total != total) n = n->next;
    if (n && n->total == total) {
        /* Unlink first matching node. */
        __glide_stack_node** p = &__glide_stack_pool;
        while (*p != n) p = &(*p)->next;
        *p = n->next;
        __glide_stack_pool_count--;
        __glide_spin_unlock(&__glide_stack_pool_spin);
        void* base = n->base;
        free(n);
        *out_total = total;
        return base;
    }
    __glide_spin_unlock(&__glide_stack_pool_spin);
    void* base;
#ifdef _WIN32
    base = VirtualAlloc(NULL, total, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!base) return NULL;
    DWORD old;
    VirtualProtect(base, __GLIDE_STACK_GUARD, PAGE_NOACCESS, &old);
#else
    base = mmap(NULL, total, PROT_READ|PROT_WRITE,
                MAP_ANON|MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED) return NULL;
    mprotect(base, __GLIDE_STACK_GUARD, PROT_NONE);
#endif
    *out_total = total;
    return base;
}

static void __glide_free_stack(void* base, size_t total) {
    if (!base) return;
    /* Hand back to pool if there's room. */
    __glide_spin_lock(&__glide_stack_pool_spin);
    if (__glide_stack_pool_count < __GLIDE_STACK_POOL_MAX) {
        __glide_stack_node* n = (__glide_stack_node*)malloc(sizeof(*n));
        n->base = base;
        n->total = total;
        n->next = __glide_stack_pool;
        __glide_stack_pool = n;
        __glide_stack_pool_count++;
        __glide_spin_unlock(&__glide_stack_pool_spin);
        return;
    }
    __glide_spin_unlock(&__glide_stack_pool_spin);
#ifdef _WIN32
    (void)total;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, total);
#endif
}

/* Plant trampoline so the first ctx_switch into this ctx returns into
   the trampoline. x86-64 pushes the address onto the new stack and
   relies on `ret`; AArch64 sets `lr` (x30) directly in the saved ctx
   and `ret` jumps to lr. SP stays 16-byte aligned per AAPCS64 / AMD64
   call ABI either way. */
static void __glide_coro_init(__glide_task* t) {
    size_t total = 0;
    t->stack = __glide_alloc_stack(&total);
    t->stack_total = total;
    char* top = (char*)t->stack + total;
    top -= 16;                       /* headroom — never write past top */
    uintptr_t sp = ((uintptr_t)top) & ~(uintptr_t)15;
#if defined(__aarch64__) || defined(_M_ARM64)
    t->ctx.sp = (void*)sp;
    t->ctx.lr = (void*)__glide_coro_trampoline;
    t->ctx.x19 = 0; t->ctx.x20 = 0; t->ctx.x21 = 0; t->ctx.x22 = 0;
    t->ctx.x23 = 0; t->ctx.x24 = 0; t->ctx.x25 = 0; t->ctx.x26 = 0;
    t->ctx.x27 = 0; t->ctx.x28 = 0;
    t->ctx.fp = 0;
    t->ctx.d8  = 0; t->ctx.d9  = 0; t->ctx.d10 = 0; t->ctx.d11 = 0;
    t->ctx.d12 = 0; t->ctx.d13 = 0; t->ctx.d14 = 0; t->ctx.d15 = 0;
#else
    *(void**)sp = (void*)__glide_coro_trampoline;
    t->ctx.rsp = (void*)sp;
    t->ctx.rbx = 0; t->ctx.rbp = 0;
    t->ctx.r12 = 0; t->ctx.r13 = 0; t->ctx.r14 = 0; t->ctx.r15 = 0;
#ifdef _WIN32
    t->ctx.rdi = 0; t->ctx.rsi = 0;
#endif
#endif
}

static void __glide_coro_destroy(__glide_task* t) {
    __glide_free_stack(t->stack, t->stack_total);
    t->stack = NULL;
    t->stack_total = 0;
}

static atomic_int __glide_pin_workers_enabled = 0;
static void __glide_maybe_pin_worker(int wid) {
    if (!atomic_load_explicit(&__glide_pin_workers_enabled, memory_order_relaxed)) return;
    /* Pin worker `wid` to CPU `wid % ncpu`. Eliminates the OS scheduler
       migrations that produce 100x bimodal latency on micro-benches.
       Default off; bench harness sets GLIDE_PIN_WORKERS=1 at sched
       init. Real workloads usually don't want this (preempts the OS's
       load-balancing) - it's a measurement tool. */
#ifdef _WIN32
    DWORD_PTR mask = ((DWORD_PTR)1) << (wid & 63);
    SetThreadAffinityMask(GetCurrentThread(), mask);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(wid % CPU_SETSIZE, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
    /* macOS / *BSD: no portable per-thread affinity. Skip. */
}

static void* __glide_worker_main(void* arg) {
    __glide_my_worker = (int)(intptr_t)arg;
    __glide_maybe_pin_worker(__glide_my_worker);
#ifdef _WIN32
    /* Snapshot the OS thread's real stack so we can swap TIB.StackBase/Limit
       to the coro's region while it runs and back to ours between switches. */
    {
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        __glide_orig_stack_base = tib->StackBase;
        __glide_orig_stack_limit = tib->StackLimit;
    }
#endif
    while (!atomic_load(&__glide_shutdown)) {
        __glide_task* t = __glide_q_pop_my();
        if (!t) break;
        __glide_cur_task = t;
        if (!t->has_run) __glide_reset_ctx(t);
        t->state = 1;
        t->has_run = 1;  /* stick to this OS thread from now on */
#ifdef _WIN32
        __glide_tib_set_stack((char*)t->stack + t->stack_total, t->stack);
#endif
        __glide_ctx_switch(&__glide_worker_ctx, &t->ctx);
#ifdef _WIN32
        __glide_tib_set_stack(__glide_orig_stack_base, __glide_orig_stack_limit);
#endif
        __glide_cur_task = NULL;
        if (t->state == 3) {
            __glide_free_task(t);  /* pool reuse — keeps stack mmap'd */
            /* Single atomic on this worker's shard. The previous code also did a
               cross-queue cv broadcast when total pending hit zero, but
               sched_shutdown busy-waits on pending_sum and broadcasts itself once
               shutdown=1, so this hot-path broadcast is redundant. */
            atomic_fetch_sub_explicit(&__glide_pending_shards[__glide_my_worker].v, 1, memory_order_acq_rel);
        } else if (t->state == 2) {
            /* Parked. Complete the hand-off: link into wait list,
               release the lock the parker held. After this point
               unpark may safely queue the task. */
            if (t->park_list) {
                t->wait_next = *t->park_list;
                *t->park_list = t;
                t->park_list = NULL;
            }
            if (t->park_lock) {
                pthread_mutex_unlock(t->park_lock);
                t->park_lock = NULL;
            } else if (t->park_spin) {
                __glide_spin_unlock(t->park_spin);
                t->park_spin = NULL;
            }
        } else {
            t->state = 0;
            __glide_q_push_to(t->home_worker, t);
        }
    }
    return NULL;
}

#ifndef _WIN32
#include <signal.h>
static void __glide_perf_sigusr2(int sig) {
    (void)sig;
    extern void __glide_perf_dump(void);
    __glide_perf_dump();
}
#endif

void __glide_sched_init(void) {
    if (__glide_q_inited) return;
#ifndef _WIN32
    /* SIGUSR2 dumps the perf counters and resets them — used by the
       bench scripts to read parks/q_pushes/cv_signals across a wrk run. */
    signal(SIGUSR2, __glide_perf_sigusr2);
#endif
    const char* env_stk = getenv("GLIDE_CORO_STACK");
    if (env_stk) {
        int n = atoi(env_stk);
        if (n >= 1024) __glide_stack_size = n;  /* min 1 KB to avoid SIGSEGV on entry */
    }
    const char* env_pin = getenv("GLIDE_PIN_WORKERS");
    if (env_pin && env_pin[0] != 0 && env_pin[0] != '0') {
        atomic_store_explicit(&__glide_pin_workers_enabled, 1, memory_order_relaxed);
    }
    const char* env = getenv("GLIDE_WORKERS");
    if (env) __glide_n_workers = atoi(env);
    if (__glide_n_workers <= 0) {
        int ncpu;
#ifdef _WIN32
        SYSTEM_INFO si; GetSystemInfo(&si);
        ncpu = (int)si.dwNumberOfProcessors;
#else
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        ncpu = (n > 0) ? (int)n : 4;
#endif
        /* Cap default workers at 8. The M:N pool plus the reactor/timer
           pthreads plus any spawn_thread accept loops the user adds are
           all competing for CPU; on a 32-vCPU host scaling workers to
           ncpu makes wakes/idles thrash the cv list and drops HTTP
           throughput by 2× compared to 8 workers. Users can opt back
           into ncpu via GLIDE_WORKERS=<n>. */
        __glide_n_workers = ncpu < 8 ? ncpu : 8;
    }
    __glide_wqs = (__glide_wq*)calloc(__glide_n_workers, sizeof(__glide_wq));
    for (int i = 0; i < __glide_n_workers; i++) {
        pthread_mutex_init(&__glide_wqs[i].mu, NULL);
        pthread_cond_init(&__glide_wqs[i].cv, NULL);
    }
    /* One shard per worker plus one for non-coro callers (main / arbitrary OS threads). */
    __glide_pending_n_shards = __glide_n_workers + 1;
    __glide_pending_shards = (__glide_pending_shard_t*)calloc(__glide_pending_n_shards, sizeof(__glide_pending_shard_t));
    __glide_q_inited = 1;
    __glide_workers = (pthread_t*)malloc(sizeof(pthread_t) * __glide_n_workers);
    for (int i = 0; i < __glide_n_workers; i++) {
        pthread_create(&__glide_workers[i], NULL, __glide_worker_main, (void*)(intptr_t)i);
    }
    pthread_mutex_init(&__glide_timer_mu, NULL);
    pthread_cond_init(&__glide_timer_cv, NULL);
    __glide_timer_inited = 1;
    pthread_create(&__glide_timer_thread, NULL, __glide_timer_main, NULL);
}

void __glide_sched_shutdown(void) {
    if (!__glide_q_inited) return;
    __glide_flush_main_buf();
    while (__glide_pending_sum() > 0) {
        struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 1000000;
        nanosleep(&ts, NULL);
    }
    atomic_store(&__glide_shutdown, 1);
    for (int i = 0; i < __glide_n_workers; i++) {
        pthread_mutex_lock(&__glide_wqs[i].mu);
        pthread_cond_broadcast(&__glide_wqs[i].cv);
        pthread_mutex_unlock(&__glide_wqs[i].mu);
    }
    for (int i = 0; i < __glide_n_workers; i++) {
        pthread_join(__glide_workers[i], NULL);
    }
    if (__glide_timer_inited) {
        pthread_mutex_lock(&__glide_timer_mu);
        pthread_cond_broadcast(&__glide_timer_cv);
        pthread_mutex_unlock(&__glide_timer_mu);
        pthread_join(__glide_timer_thread, NULL);
        __glide_timer_inited = 0;
    }
    free(__glide_workers); __glide_workers = NULL;
    free(__glide_wqs); __glide_wqs = NULL;
    free(__glide_pending_shards); __glide_pending_shards = NULL;
    __glide_pending_n_shards = 0;
    __glide_q_inited = 0;
}

/* Task pool: full task structs (stack + ctx already initialized). On reuse
   we just rewind the trampoline pointer at the top of the existing stack —
   no mmap, no calloc, no setup work. Burst spawns become ~1 mutex + 1 push. */
static __glide_task* __glide_task_pool = NULL;
static int __glide_task_pool_count = 0;
static const int __GLIDE_TASK_POOL_MAX = 16384;
static __glide_spin_t __glide_task_pool_spin = 0;

/* Per-thread magazine. The global pool's spinlock is the spawn hot-path
   bottleneck under burst load (13 threads contending). Each thread keeps a
   small LIFO; alloc/free hit it lock-free, and we only touch the global pool
   in batches when the magazine fills or drains. Reduces global ops ~32x. */
#define __GLIDE_TLS_POOL_MAX 256
#define __GLIDE_TLS_POOL_BATCH 64
static __thread __glide_task* __glide_tls_pool = NULL;
static __thread int __glide_tls_pool_count = 0;

static void __glide_reset_ctx(__glide_task* t) {
    char* top = (char*)t->stack + t->stack_total;
    top -= 16;
    uintptr_t sp = ((uintptr_t)top) & ~(uintptr_t)15;
#if defined(__aarch64__) || defined(_M_ARM64)
    t->ctx.sp = (void*)sp;
    t->ctx.lr = (void*)__glide_coro_trampoline;
    t->ctx.x19 = 0; t->ctx.x20 = 0; t->ctx.x21 = 0; t->ctx.x22 = 0;
    t->ctx.x23 = 0; t->ctx.x24 = 0; t->ctx.x25 = 0; t->ctx.x26 = 0;
    t->ctx.x27 = 0; t->ctx.x28 = 0;
    t->ctx.fp = 0;
    t->ctx.d8  = 0; t->ctx.d9  = 0; t->ctx.d10 = 0; t->ctx.d11 = 0;
    t->ctx.d12 = 0; t->ctx.d13 = 0; t->ctx.d14 = 0; t->ctx.d15 = 0;
#else
    *(void**)sp = (void*)__glide_coro_trampoline;
    t->ctx.rsp = (void*)sp;
    /* GP/XMM regs zeroed via 8-byte stores — faster than memset for
       this fixed-size struct, and the compiler vectorizes if it cares. */
    t->ctx.rbx = 0; t->ctx.rbp = 0;
    t->ctx.r12 = 0; t->ctx.r13 = 0; t->ctx.r14 = 0; t->ctx.r15 = 0;
#ifdef _WIN32
    t->ctx.rdi = 0; t->ctx.rsi = 0;
    /* xmm[160] = 20 × 8 bytes; unrolled clear for branchless write. */
    uint64_t* x = (uint64_t*)t->ctx.xmm;
    x[0]=0; x[1]=0; x[2]=0; x[3]=0; x[4]=0; x[5]=0; x[6]=0; x[7]=0;
    x[8]=0; x[9]=0; x[10]=0; x[11]=0; x[12]=0; x[13]=0; x[14]=0; x[15]=0;
    x[16]=0; x[17]=0; x[18]=0; x[19]=0;
#endif
#endif
}

/* No-op now: state/has_run/wait_next/park_lock/park_list are cleared at FREE time
   instead, so the spawn hot path on main has fewer stores. The reset_ctx clear
   already moved to worker_main earlier (gated on has_run==0). Inlined since callers
   sometimes need the return value. */
static inline __glide_task* __glide_finish_alloc(__glide_task* t) { return t; }

__glide_task* __glide_alloc_task(void) {
    /* Magazine fast path: pop without ever touching the spinlock. */
    __glide_task* t = __glide_tls_pool;
    if (t) {
        __glide_tls_pool = t->next;
        __glide_tls_pool_count--;
        return __glide_finish_alloc(t);
    }
    /* Magazine empty: refill a batch in one global-lock acquisition. */
    __glide_spin_lock(&__glide_task_pool_spin);
    t = __glide_task_pool;
    if (t) {
        __glide_task_pool = t->next;
        __glide_task_pool_count--;
        __glide_task* head = NULL;
        int taken = 0;
        while (taken < __GLIDE_TLS_POOL_BATCH - 1 && __glide_task_pool) {
            __glide_task* x = __glide_task_pool;
            __glide_task_pool = x->next;
            __glide_task_pool_count--;
            x->next = head;
            head = x;
            taken++;
        }
        __glide_spin_unlock(&__glide_task_pool_spin);
        __glide_tls_pool = head;
        __glide_tls_pool_count = taken;
        return __glide_finish_alloc(t);
    }
    __glide_spin_unlock(&__glide_task_pool_spin);
    /* Pool fully drained: fresh allocation. */
    t = (__glide_task*)calloc(1, sizeof(__glide_task));
    __glide_coro_init(t);
    return t;
}

void __glide_free_task(__glide_task* t) {
    /* Reset all the per-task transient fields here, on the worker thread that's
       freeing — main's spawn hot path was paying for these N stores per task. */
    t->state = 0; t->has_run = 0;
    t->wait_next = NULL;
    t->park_lock = NULL; t->park_spin = NULL; t->park_list = NULL;
    t->palloc_arena = NULL;
    if (__glide_tls_pool_count < __GLIDE_TLS_POOL_MAX) {
        t->next = __glide_tls_pool;
        __glide_tls_pool = t;
        __glide_tls_pool_count++;
        return;
    }
    /* Magazine full: flush BATCH (this task + last BATCH-1 from TLS) to global. */
    /* `t` may carry a stale ->next from the run queue; null it so the chain
       we're about to build terminates cleanly when we walk it for overflow. */
    t->next = NULL;
    __glide_task* batch = t;
    int batch_n = 1;
    while (batch_n < __GLIDE_TLS_POOL_BATCH && __glide_tls_pool) {
        __glide_task* x = __glide_tls_pool;
        __glide_tls_pool = x->next;
        __glide_tls_pool_count--;
        x->next = batch;
        batch = x;
        batch_n++;
    }
    __glide_spin_lock(&__glide_task_pool_spin);
    int room = __GLIDE_TASK_POOL_MAX - __glide_task_pool_count;
    int put = (room < batch_n) ? room : batch_n;
    __glide_task* overflow = batch;
    if (put > 0) {
        __glide_task* tail = batch;
        for (int i = 0; i < put - 1; i++) tail = tail->next;
        overflow = tail->next;
        tail->next = __glide_task_pool;
        __glide_task_pool = batch;
        __glide_task_pool_count += put;
    }
    __glide_spin_unlock(&__glide_task_pool_spin);
    while (overflow) {
        __glide_task* x = overflow;
        overflow = x->next;
        __glide_coro_destroy(x);
        free(x);
    }
}

/* Per-OS-thread spawn buffer for the from-main path. Batching 32 spawns into
   one queue lock acquisition cuts spinlock contention dramatically when burst-
   spawning into W0. pending is incremented immediately (per spawn) so callers'
   `pending_count()` busy-waits don't observe a stale-low value while tasks sit
   in the buffer. The flush is triggered by buffer-full, by sched_shutdown, and
   by pending_count itself (so wait loops can never deadlock on unflushed work). */
#define __GLIDE_MAIN_BATCH 16
static _Thread_local __glide_task* __glide_main_buf_head = NULL;
static _Thread_local __glide_task* __glide_main_buf_tail = NULL;
static _Thread_local int __glide_main_buf_count = 0;
/* Set on the program's main thread by the entrypoint wrapper. Foreign
   pthreads (spawn_thread'd accept loops, IOCP reactor, etc) leave it
   at 0, which steers __glide_spawn to push direct instead of
   buffering for an end-of-batch flush that may never come. */
_Thread_local int __glide_is_main_tls = 0;
static void __glide_flush_main_buf(void) {
    if (!__glide_main_buf_head) return;
    /* Match the per-thread sticky worker chosen by __glide_pick_worker
       above, so the chain lands on the same queue the tasks' home_worker
       points at. Otherwise work-stealing has to immediately re-route. */
    int target = __glide_outside_pref_worker;
    if (target < 0) target = 0;
    __glide_q_push_chain(target, __glide_main_buf_head, __glide_main_buf_tail, __glide_main_buf_count);
    __glide_main_buf_head = NULL;
    __glide_main_buf_tail = NULL;
    __glide_main_buf_count = 0;
}

void __glide_spawn(__glide_task_fn fn, void* arg) {
    __glide_task* t = __glide_alloc_task();
    t->entry = fn;
    t->arg = arg;
    t->home_worker = __glide_pick_worker();
    /* Increment the SPAWNER's shard, not the home_worker's. With main always
       picking W0, sharding by home_worker would put every increment on shard[0] and
       defeat the whole point. Spawner-thread keeps main's stream separate. */
    int __sidx = (__glide_my_worker >= 0) ? __glide_my_worker : __glide_n_workers;
    atomic_fetch_add_explicit(&__glide_pending_shards[__sidx].v, 1, memory_order_relaxed);
    if (__glide_cur_task != NULL && __glide_my_worker >= 0) {
        /* Inside a coro: same-thread same-queue, spinlock is uncontended. */
        __glide_q_push_to(t->home_worker, t);
        return;
    }
    /* Outside a coro: main batches its high-frequency spawn burst into
       a per-thread buffer that flushes at __GLIDE_MAIN_BATCH or on the
       next pending_count(). Foreign pthreads (e.g. the spawn_thread'd
       accept loops in http.glide) don't share that flush cadence - if
       they buffer, the first 31 spawns sit unprocessed until a 32nd
       arrives. They push direct instead. The per-thread buf is TLS so
       there's no shared state to race on; we just need a way to tell
       "main" from "foreign pthread", which is the __glide_is_main_tls
       flag set by the entrypoint wrapper. */
    if (__glide_is_main_tls) {
        t->next = NULL;
        if (__glide_main_buf_tail) __glide_main_buf_tail->next = t;
        else __glide_main_buf_head = t;
        __glide_main_buf_tail = t;
        __glide_main_buf_count++;
        if (__glide_main_buf_count >= __GLIDE_MAIN_BATCH) {
            __glide_flush_main_buf();
        }
        return;
    }
    /* Foreign pthread: push direct. */
    __glide_q_push_to(t->home_worker, t);
}

void yield_now(void) {
    __glide_task* t = __glide_cur_task;
    if (!t) return;
    t->state = 0;
    __glide_ctx_switch(&t->ctx, &__glide_worker_ctx);
}

/* Park current coro into state=blocked and yield to its worker. The
   caller is responsible for arranging the wake (e.g. IOCP reactor
   calls __glide_unpark_task once the I/O completes). No-op when not
   running inside a coro. */
void __glide_park_blocked(void) {
    __glide_task* t = __glide_cur_task;
    if (!t) return;
    t->state = 2;
    __glide_ctx_switch(&t->ctx, &__glide_worker_ctx);
}

/* Wake a previously-parked coro from any thread (typically the IOCP
   reactor thread). Pushes onto the coro's home worker queue so the
   worker pops it on its next tick. Safe to call before the coro
   actually parks - q_push_to handles the queue race. */
void __glide_unpark_task(__glide_task* t) {
    if (t == NULL) return;
    __glide_q_push_to(t->home_worker, t);
}

int pending_count(void) {
    /* Flush any locally-buffered spawns before reading; otherwise a busy-wait
       like `while pending_count() > 0 {}` could hold tasks in the buffer forever. */
    if (__glide_cur_task == NULL) __glide_flush_main_buf();
    return __glide_pending_sum();
}

int64_t now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq; static int inited = 0;
    if (!inited) { QueryPerformanceFrequency(&freq); inited = 1; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

static long long __glide_monotonic_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq; static int inited = 0;
    if (!inited) { QueryPerformanceFrequency(&freq); inited = 1; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (long long)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
#endif
}

/* Timer thread: sleeps until the head deadline, unparks expired tasks. */
static void* __glide_timer_main(void* unused) {
    (void)unused;
    pthread_mutex_lock(&__glide_timer_mu);
    while (!atomic_load(&__glide_shutdown)) {
        if (__glide_timer_head == NULL) {
            pthread_cond_wait(&__glide_timer_cv, &__glide_timer_mu);
            continue;
        }
        long long now = __glide_monotonic_ns();
        if (__glide_timer_head->deadline_ns <= now) {
            __glide_timer_node* expired = __glide_timer_head;
            __glide_timer_head = expired->next;
            __glide_task* t = expired->task;
            free(expired);
            pthread_mutex_unlock(&__glide_timer_mu);
            t->state = 0;
            __glide_q_push_to(t->home_worker, t);
            pthread_mutex_lock(&__glide_timer_mu);
            continue;
        }
        long long delta = __glide_timer_head->deadline_ns - now;
        struct timespec abst;
#ifdef _WIN32
        struct timespec _now_rt; clock_gettime(CLOCK_REALTIME, &_now_rt);
        abst.tv_sec = _now_rt.tv_sec; abst.tv_nsec = _now_rt.tv_nsec;
#else
        clock_gettime(CLOCK_REALTIME, &abst);
#endif
        long long add_ns = abst.tv_nsec + delta;
        abst.tv_sec  += (time_t)(add_ns / 1000000000LL);
        abst.tv_nsec  = (long)(add_ns % 1000000000LL);
        pthread_cond_timedwait(&__glide_timer_cv, &__glide_timer_mu, &abst);
    }
    pthread_mutex_unlock(&__glide_timer_mu);
    return NULL;
}

void sleep_ms(int ms) {
    __glide_task* t = __glide_cur_task;
    if (!t) {
        /* Not in a coro - blocking sleep. Flush first so any spawns we just
           queued aren't held in our local buffer while we sit here idle. */
        __glide_flush_main_buf();
#ifdef _WIN32
        Sleep((DWORD)ms);
#else
        struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)((ms % 1000)) * 1000000L;
        nanosleep(&ts, NULL);
#endif
        return;
    }
    /* Coro - register on the timer queue, park, worker runs others. */
    long long deadline = __glide_monotonic_ns() + (long long)ms * 1000000LL;
    __glide_timer_node* node = (__glide_timer_node*)malloc(sizeof(__glide_timer_node));
    node->deadline_ns = deadline;
    node->task = t;
    pthread_mutex_lock(&__glide_timer_mu);
    __glide_timer_node** cur = &__glide_timer_head;
    while (*cur && (*cur)->deadline_ns <= deadline) cur = &(*cur)->next;
    node->next = *cur;
    *cur = node;
    /* Wake timer thread to recompute its deadline. */
    pthread_cond_signal(&__glide_timer_cv);
    /* Park: worker hand-off releases __glide_timer_mu after switch. */
    __glide_park(&__glide_timer_mu, NULL);
}

/* Coro park: caller holds `lock` and wants to wait on `list`. Returns
   1 if parked (caller must re-acquire `lock` and re-check state), 0
   if the caller is not in a coroutine context (caller should fall
   back to pthread_cond_wait). The actual list link + lock release is
   done by the worker AFTER the fiber switch — that's how we avoid the
   classic park/unpark race. */
/* Counters: instrumented to confirm where the throughput cost is
   coming from. Call __glide_perf_dump() to print and reset. Non-static
   because __glide_q_push_to (defined earlier in this same TU) bumps
   q_pushes / cv_signals via the forward decl above. */
atomic_long __glide_perf_parks       = 0;
atomic_long __glide_perf_spin_parks  = 0;
atomic_long __glide_perf_unparks     = 0;
atomic_long __glide_perf_q_pushes    = 0;
atomic_long __glide_perf_cv_signals  = 0;
atomic_long __glide_perf_wake_ns     = 0;
atomic_long __glide_perf_wake_calls  = 0;

long long __glide_perf_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

void __glide_perf_dump(void) {
    long p  = atomic_exchange(&__glide_perf_parks, 0);
    long sp = atomic_exchange(&__glide_perf_spin_parks, 0);
    long u  = atomic_exchange(&__glide_perf_unparks, 0);
    long q  = atomic_exchange(&__glide_perf_q_pushes, 0);
    long cs = atomic_exchange(&__glide_perf_cv_signals, 0);
    long wn = atomic_exchange(&__glide_perf_wake_ns, 0);
    long wc = atomic_exchange(&__glide_perf_wake_calls, 0);
    long avg = wc > 0 ? wn / wc : 0;
    fprintf(stderr,
            "[glide perf] parks=%ld spin_parks=%ld unparks=%ld q_pushes=%ld cv_signals=%ld\n"
            "             wake_calls=%ld wake_total_ns=%ld wake_avg_ns=%ld\n",
            p, sp, u, q, cs, wc, wn, avg);
}

int __glide_park(pthread_mutex_t* lock, __glide_task** list) {
    __glide_task* t = __glide_cur_task;
    if (!t) {
        /* Not in a coro — release the wait-list mutex the caller locked
           (so the reactor pthread / other coros can still touch it),
           flush main's pending spawn buffer so the workers see them
           while main blocks, and signal a non-coro fallback to the caller. */
        if (lock) pthread_mutex_unlock(lock);
        __glide_flush_main_buf();
        return 0;
    }
    atomic_fetch_add_explicit(&__glide_perf_parks, 1, memory_order_relaxed);
    t->park_lock = lock;
    t->park_list = list;
    t->state = 2;
    __glide_ctx_switch(&t->ctx, &__glide_worker_ctx);
    return 1;
}

/* Spinlock variant of __glide_park. Same protocol — caller holds the
   spinlock, returns 1 parked / 0 non-coro fallback. Used by the I/O
   reactor: per-fd waiter critical sections are 5-10 ns of list ops,
   well below the futex-call overhead a pthread_mutex incurs. */
int __glide_spin_park(__glide_spin_t* lock, __glide_task** list) {
    __glide_task* t = __glide_cur_task;
    if (!t) {
        if (lock) __glide_spin_unlock(lock);
        __glide_flush_main_buf();
        return 0;
    }
    atomic_fetch_add_explicit(&__glide_perf_spin_parks, 1, memory_order_relaxed);
    t->park_spin = lock;
    t->park_list = list;
    t->state = 2;
    __glide_ctx_switch(&t->ctx, &__glide_worker_ctx);
    return 1;
}

/* Caller holds the chan/mutex protecting the wait list. Pop the head and queue it. */
void __glide_unpark_one(__glide_task** list) {
    __glide_task* t = *list;
    if (!t) return;
    *list = t->wait_next;
    t->wait_next = NULL;
    t->state = 0;
    atomic_fetch_add_explicit(&__glide_perf_unparks, 1, memory_order_relaxed);
    __glide_q_push_to(t->home_worker, t);
}

/* Per-coro arena accessors. The Glide-side __glide_palloc / _get / _set
   live in src/builtins/builtins.glide (emitted earlier in the same TU)
   and forward-declare these. When running inside a coroutine, the
   arena slot lives in the task struct so cooperative yields can't
   cross-pollute. Outside a coro (main thread, OS thread spawned via
   spawn_thread that hasn't installed a task) we return -1 and the
   builtins fall back to the file-local g_palloc_active. */
int __glide_task_is_active(void) {
    return __glide_cur_task != NULL ? 1 : 0;
}
void* __glide_task_arena_get(void) {
    return __glide_cur_task != NULL ? __glide_cur_task->palloc_arena : NULL;
}
void __glide_task_arena_set(void* a) {
    if (__glide_cur_task != NULL) {
        __glide_cur_task->palloc_arena = a;
    }
}


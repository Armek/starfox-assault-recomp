/* threads.c - guest threads, context switching and interrupt delivery.
 *
 * THREADS. The Dolphin OS scheduler (recompiled as-is) switches threads in SelectThread():
 *     if (!(cur->context.state & EXC) && OSSaveContext(&cur->context)) return;  // resumed later
 *     ... pick next ...
 *     OSLoadContext(&next->context);                                          // never returns
 * Compiled code cannot swap stacks, so every guest thread runs on its own host thread with its own
 * CPU state (Ctx), and a baton makes sure exactly one of them runs at a time, like the single
 * Gekko core. OSSaveContext() records which guest OSContext the calling host thread owns;
 * OSLoadContext() hands the baton to the host thread owning that context (starting a new host
 * thread the first time a context created by OSCreateThread() is loaded) and parks the caller
 * until its own context is loaded again. When the caller resumes, OSLoadContext() simply returns,
 * which leaves SelectThread() exactly as "OSSaveContext() returned 1" would have.
 *
 * INTERRUPTS. Generated code calls RT_SAFEPOINT at every loop back-edge. Every few thousand
 * safepoints the runtime checks its device timers; if an interrupt is due and the running
 * thread has MSR[EE] set, it performs what the exception vector + __OSDispatchInterrupt would:
 * save the registers into the current OSContext (marked EXC), call the SDK's registered handler
 * from the table at 0x80003040 with the scheduler disabled, then __OSReschedule().
 */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <time.h>
#include "funcs.h"
#include "runtime.h"
#include "gx.h"

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK Mutex;
typedef CONDITION_VARIABLE Cond;
#define MUTEX_INIT SRWLOCK_INIT
#define COND_INIT CONDITION_VARIABLE_INIT
static void lock(Mutex *m) { AcquireSRWLockExclusive(m); }
static void unlock(Mutex *m) { ReleaseSRWLockExclusive(m); }
static void wait(Cond *cv, Mutex *m) { SleepConditionVariableSRW(cv, m, INFINITE, 0); }
static void broadcast(Cond *cv) { WakeAllConditionVariable(cv); }
#define THREAD_LOCAL __declspec(thread)
#else
#include <pthread.h>
typedef pthread_mutex_t Mutex;
typedef pthread_cond_t Cond;
#define MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define COND_INIT PTHREAD_COND_INITIALIZER
static void lock(Mutex *m) { pthread_mutex_lock(m); }
static void unlock(Mutex *m) { pthread_mutex_unlock(m); }
static void wait(Cond *cv, Mutex *m) { pthread_cond_wait(cv, m); }
static void broadcast(Cond *cv) { pthread_cond_broadcast(cv); }
#define THREAD_LOCAL _Thread_local
#endif

/* ---- guest OS layout ------------------------------------------------------------------------- */

#define OS_CURRENT_CONTEXT 0x800000D4u  /* OSContext* of the running thread / exception */
#define OS_CURRENT_THREAD 0x800000E4u   /* OSThread* */
#define OS_HANDLER_TABLE 0x80003040u    /* __OSInterruptHandler[32] */
#define MSR_EE 0x8000u
#define CTX_STATE 0x1A2u
#define CTX_STATE_EXC 0x02u

/* ---- host threads ---------------------------------------------------------------------------- */

typedef struct HThread {
    Ctx *c;
    uint32_t saved_ctx;    /* guest OSContext this thread parked itself in (OSSaveContext) */
    uint32_t irq_ctx[8];   /* contexts saved on exception entry, innermost last */
    jmp_buf irq_jmp[8];    /* where "return from exception" (loading irq_ctx[i]) lands */
    int irq_depth;
    uint32_t resume_ctx;   /* the context whose loading woke this thread */
    int exiting;           /* thread function returned; it is running OSExitThread */
    uint32_t start_pc, exit_pc;
    int id;
} HThread;

#define MAX_THREADS 64
static HThread *g_threads[MAX_THREADS];
static int g_thread_count;
static HThread *g_running;
static Mutex g_lock = MUTEX_INIT;
static Cond g_cv = COND_INIT;
static THREAD_LOCAL HThread *t_self;

int g_safepoint_budget = 4096;

Ctx *rt_current_ctx(void) { return t_self ? t_self->c : NULL; }

static void save_to_guest(Ctx *c, uint32_t ctx) {
    for (int i = 0; i < 32; i++) wr32(ctx + i * 4, c->r[i]);
    wr32(ctx + 0x80, c->cr);
    wr32(ctx + 0x84, c->lr);
    wr32(ctx + 0x88, c->ctr);
    wr32(ctx + 0x8C, (c->xer_so << 31) | (c->xer_ca << 29));
    wr32(ctx + 0x19C, c->msr);
}

static void load_from_guest(Ctx *c, uint32_t ctx) {
    for (int i = 0; i < 32; i++) c->r[i] = rd32(ctx + i * 4);
    c->cr = rd32(ctx + 0x80);
    c->lr = rd32(ctx + 0x84);
    c->ctr = rd32(ctx + 0x88);
    uint32_t xer = rd32(ctx + 0x8C);
    c->xer_so = xer >> 31;
    c->xer_ca = (xer >> 29) & 1;
    for (int i = 0; i < 32; i++) {
        c->f[i].ps0 = u2d(rd64(ctx + 0x90 + i * 8));
        c->f[i].ps1 = u2d(rd64(ctx + 0x1C8 + i * 8));
    }
    c->fpscr = rd32(ctx + 0x194);
    c->msr = rd32(ctx + 0x19C);
    for (int i = 0; i < 8; i++) c->gqr[i] = rd32(ctx + 0x1A4 + i * 4);
}

static int owns(HThread *t, uint32_t ctx) {
    if (t->saved_ctx == ctx) return 1;
    for (int i = 0; i < t->irq_depth; i++)
        if (t->irq_ctx[i] == ctx) return 1;
    return 0;
}

/* Give the CPU to `next` and block until someone gives it back to `self`. */
static void hand_off(HThread *self, HThread *next) {
    lock(&g_lock);
    g_running = next;
    broadcast(&g_cv);
    while (g_running != self) wait(&g_cv, &g_lock);
    unlock(&g_lock);
    t_self = self;
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID arg)
#else
static void *thread_main(void *arg)
#endif
{
    HThread *t = arg;
    lock(&g_lock);
    while (g_running != t) wait(&g_cv, &g_lock);
    unlock(&g_lock);
    t_self = t;
    printf("[recomp] thread %d starts at %08X %s (sp=%08X)\n", t->id, t->start_pc, rt_func_name(t->start_pc), t->c->r[1]);
    rt_call(t->c, t->start_pc);
    /* The thread function returned: continue at the LR OSCreateThread set up (OSExitThread). */
    t->exiting = 1;
    rt_call(t->c, t->exit_pc);
    rt_fatal("guest thread %d returned from its exit function", t->id);
    return 0;
}

static HThread *new_thread(uint32_t ctx) {
    if (g_thread_count == MAX_THREADS) rt_fatal("too many guest threads");
    HThread *t = calloc(1, sizeof *t);
    t->c = calloc(1, sizeof(Ctx));
    load_from_guest(t->c, ctx);
    t->saved_ctx = ctx;
    t->start_pc = rd32(ctx + 0x198); /* srr0 */
    t->exit_pc = t->c->lr;
    t->id = g_thread_count;
    g_threads[g_thread_count++] = t;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 8 << 20, thread_main, t, 0, NULL);
    if (!h) rt_fatal("CreateThread failed");
    CloseHandle(h);
#else
    pthread_t pt;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    if (pthread_create(&pt, &attr, thread_main, t)) rt_fatal("pthread_create failed");
    pthread_detach(pt);
#endif
    return t;
}

void rt_threads_init(Ctx *main_ctx) {
    HThread *t = calloc(1, sizeof *t);
    t->c = main_ctx;
    t->id = 0;
    g_threads[g_thread_count++] = t;
    g_running = t;
    t_self = t;
}

/* u32 OSSaveContext(OSContext *context): returns 0 now; "returns 1" when the context is loaded. */
HLE(OSSaveContext) {
    t_self->saved_ctx = c->r[3];
    save_to_guest(c, c->r[3]);
    c->r[3] = 0;
}

/* Loading a context saved on exception entry = return from that exception: abandon the
 * handler's remaining host frames and land back in exception() (like rfi). */
static void return_from_exception_if_irq(HThread *self, uint32_t ctx) {
    for (int k = self->irq_depth - 1; k >= 0; k--) {
        if (self->irq_ctx[k] == ctx) {
            self->irq_depth = k + 1;
            longjmp(self->irq_jmp[k], 1);
        }
    }
}

/* void OSLoadContext(OSContext *context): never returns on hardware. */
HLE(OSLoadContext) {
    HThread *self = t_self;
    uint32_t ctx = c->r[3];
    wr16(ctx + CTX_STATE, rd16(ctx + CTX_STATE) & ~CTX_STATE_EXC);
    return_from_exception_if_irq(self, ctx);
    if (self->saved_ctx == ctx) return; /* SelectThread picked us again: "OSSaveContext returned 1" */
    if (self->exiting) self->saved_ctx = 0; /* a dead thread's context may be reused */
    HThread *target = NULL;
    for (int i = 0; i < g_thread_count; i++)
        if (g_threads[i] != self && owns(g_threads[i], ctx)) { target = g_threads[i]; break; }
    if (!target) target = new_thread(ctx);
    target->resume_ctx = ctx;
    hand_off(self, target);
    /* Someone loaded one of our contexts. If it was an interrupted one, resume there. */
    return_from_exception_if_irq(self, self->resume_ctx);
}

/* ---- interrupts ------------------------------------------------------------------------------ */

static uint32_t g_pending; /* bit n = interrupt n pending */
static double g_next_vi;
static const double VI_PERIOD = 1.0 / 59.94;

static double now_s(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

void rt_raise_interrupt(int n) { g_pending |= 1u << n; g_safepoint_budget = 0; }

/* ---- decrementer: OSAlarm's time source ---------------------------------------------------------
 * DEC counts down at the timebase rate; the exception fires when it passes zero. */
static int g_dec_armed;
static uint64_t g_dec_deadline;

void rt_set_dec(Ctx *c, uint32_t value) {
    (void)c;
    g_dec_deadline = rt_timebase() + (uint64_t)(int64_t)(int32_t)value;
    g_dec_armed = 1;
    if ((int32_t)value <= 0) g_safepoint_budget = 0;
}

uint32_t rt_get_dec(Ctx *c) {
    (void)c;
    return (uint32_t)(int32_t)(int64_t)(g_dec_deadline - rt_timebase());
}

/* Exception entry, call `fn(code, context)`, exception return. Interrupts go through the
 * __OSDispatchInterrupt sequence (scheduler disabled around the handler, then reschedule). */
static void exception(Ctx *c, uint32_t fn, int code, int is_interrupt) {
    uint32_t ctx = rd32(OS_CURRENT_CONTEXT);
    if (!fn || !ctx) return;
    HThread *self = t_self;
    if (self->irq_depth == 8) rt_fatal("interrupts nested too deeply");

    /* exception entry: save state into the current context and mask external interrupts */
    static THREAD_LOCAL Ctx saved_regs[8];
    int depth = self->irq_depth;
    saved_regs[depth] = *c;
    save_to_guest(c, ctx);
    wr16(ctx + CTX_STATE, rd16(ctx + CTX_STATE) | CTX_STATE_EXC);
    self->irq_ctx[depth] = ctx;
    self->irq_depth = depth + 1;
    c->msr &= ~MSR_EE;

    if (!setjmp(self->irq_jmp[depth])) {
        if (is_interrupt) F_OSDisableScheduler(c);
        c->r[3] = (uint32_t)code;
        c->r[4] = ctx;
        rt_call(c, fn);
        if (is_interrupt) {
            F_OSEnableScheduler(c);
            F___OSReschedule(c);
        }
    }
    /* exception return: reached normally or by OSLoadContext(ctx) longjmp'ing here */
    self = t_self;
    self->irq_depth = depth;
    wr16(ctx + CTX_STATE, rd16(ctx + CTX_STATE) & ~CTX_STATE_EXC);
    *c = saved_regs[depth];
}

void rt_safepoint(Ctx *c) {
    g_safepoint_budget = 4096;
    if (video_quit_requested()) { printf("[recomp] window closed\n"); exit(0); }
    double t = now_s();
    if (t >= g_next_vi) {
        if (g_next_vi == 0) g_next_vi = t;
        g_next_vi += VI_PERIOD;
        if (g_next_vi < t) g_next_vi = t + VI_PERIOD; /* fell behind: don't burst */
        rt_vi_retrace();
        g_pending |= 1u << 24;
    }
    if (!(c->msr & MSR_EE) || t_self->irq_depth != 0) return;
    if (g_dec_armed && rt_timebase() >= g_dec_deadline) {
        g_dec_armed = 0;
        exception(c, G_DecrementerExceptionCallback, 8 /* __OS_EXCEPTION_DECREMENTER */, 0);
    }
    if (g_pending) {
        uint32_t p = g_pending;
        g_pending = 0;
        for (int n = 0; n < 32; n++)
            if (p & (1u << n)) exception(c, rd32(OS_HANDLER_TABLE + n * 4), n, 1);
    } else if (rd32(OS_CURRENT_THREAD) == 0) {
        /* The scheduler is idling (no runnable thread): sleep until the next retrace. */
        double wait_s = g_next_vi - now_s();
        if (g_dec_armed) {
            double dec_s = (double)(int64_t)(g_dec_deadline - rt_timebase()) / 40500000.0;
            if (dec_s < wait_s) wait_s = dec_s;
        }
        if (wait_s > 0.0005) rt_sleep_us((unsigned)(wait_s * 1e6));
        g_safepoint_budget = 0;
    }
}

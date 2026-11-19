/* runtime.c - host runtime for the recompiled game.
 *
 * Guest memory, the indirect-call dispatcher, tracing and diagnostics, boot setup (the low-memory
 * state the GameCube IPL and apploader leave behind) and the entry point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#endif
#define SDL_MAIN_HANDLED /* keep our own main() */
#include <SDL.h>
#include "funcs.h"
#include "runtime.h"
#include "gx.h"

typedef struct { uint32_t addr; GuestFn fn; const char *name; } FuncEntry;
extern const unsigned g_func_count;
extern const FuncEntry g_funcs[];

uint8_t *g_mem;
uint8_t *g_lc;
static Ctx g_ctx;

/* ---- diagnostics ------------------------------------------------------------------------------- */

uint32_t g_trace_ring[RT_TRACE_RING];
uint32_t g_trace_pos;
int g_trace_sdk;
static FILE *g_sdk_log;
static unsigned long g_sdk_calls, g_sdk_log_limit = 5000;

static void dump_trace_to(FILE *f, unsigned count) {
    fprintf(f, "[recomp] last functions entered (oldest first):\n");
    uint32_t pos = g_trace_pos, n = pos < RT_TRACE_RING ? pos : RT_TRACE_RING;
    for (uint32_t i = pos - n; i != pos; i++) {
        uint32_t a = g_trace_ring[i & (RT_TRACE_RING - 1)];
        if (i + count >= pos) fprintf(f, "    %08X %s\n", a, rt_func_name(a));
    }
}

void rt_dump_trace(void) { dump_trace_to(stderr, 24); }

/* ---- crash reports -------------------------------------------------------------------------------
 * A fatal error or a host crash (segfault and the like) appends a report to user/crash.log: what
 * happened, the guest registers and the last guest functions entered. Players can send that file.
 * When the program wasn't started from a terminal (Steam, a file manager, the Deck's game mode),
 * everything it prints goes to user/sfa.log too, for the last run. */
static void write_report(FILE *f, const char *what) {
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "==== %s, build %s %s ====\n", when, __DATE__, __TIME__);
    fprintf(f, "[recomp] fatal: %s\n", what);
    Ctx *c = rt_current_ctx();
    if (!c) c = &g_ctx;
    fprintf(f, "[recomp] guest registers: r1=%08X lr=%08X ctr=%08X r3=%08X r4=%08X r5=%08X msr=%08X\n",
            c->r[1], c->lr, c->ctr, c->r[3], c->r[4], c->r[5], c->msr);
    if (g_mem) {
        uint32_t stage = rd8(0x802A2B60u + 0x13C);
        fprintf(f, "[recomp] current stage %u (mission %d)\n", stage, stage < 0x3B ? rd8(0x80223560u + stage * 0x14) + 1 : -1);
    }
    dump_trace_to(f, 64);
    fflush(f);
}

static int g_no_dialogs; /* testing: never stop on an error box */

static void crash_report(const char *what) {
    static int reporting;
    if (reporting++) return; /* a crash while reporting */
    fflush(stdout);
    write_report(stderr, what);
    FILE *f = fopen("user/crash.log", "a");
    if (f) {
        write_report(f, what);
        fclose(f);
    }
    if (g_sdk_log) fflush(g_sdk_log);
    char box[1200];
    snprintf(box, sizeof box, "%s\n\nDetails were saved to user/crash.log next to the program.", what);
    if (!g_no_dialogs) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Star Fox: Assault (recompiled) stopped", box, NULL);
}

void rt_fatal(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    crash_report(msg);
    exit(1);
}

#ifdef _WIN32
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    char msg[256];
    snprintf(msg, sizeof msg, "the program crashed (exception 0x%08lX at %p, address %p)",
             (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
             ep->ExceptionRecord->NumberParameters >= 2 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL);
    crash_report(msg);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static void on_crash(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char msg[256];
    snprintf(msg, sizeof msg, "the program crashed (%s at address %p)", strsignal(sig), si ? si->si_addr : NULL);
    crash_report(msg);
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

/* After the working folder is settled (next to the program or the game). Test runs (g_no_dialogs)
 * keep their output where the caller sends it. */
static void crash_handling_init(void) {
#ifdef _WIN32
    _mkdir("user");
    int tty = _isatty(_fileno(stdout));
    SetUnhandledExceptionFilter(on_crash);
#else
    mkdir("user", 0755);
    int tty = isatty(fileno(stdout));
    static uint8_t alt[64 * 1024];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
#endif
    if (!tty && !g_no_dialogs && freopen("user/sfa.log", "w", stdout)) {
        setvbuf(stdout, NULL, _IOLBF, 4096);
#ifdef _WIN32
        _dup2(_fileno(stdout), _fileno(stderr));
#else
        dup2(fileno(stdout), fileno(stderr));
#endif
    }
}

void rt_console_putc(char ch) { fputc(ch, stdout); if (ch == '\n') fflush(stdout); }

/* ---- function lookup --------------------------------------------------------------------------- */

#ifdef RT_WATCH
/* Write watchpoint (debug builds only): SFA_WATCH=<hex address>:<hex length>. */
uint32_t g_watch_lo = 0xFFFFFFFFu, g_watch_len = 0;
void rt_watch_hit(uint32_t addr, uint32_t value, int size) {
    static unsigned hits, zero;
    static int nan_only = -1; /* SFA_WATCH_NAN=1: only 32-bit writes of an infinite float (=2: or a NaN,
                               * though -1 markers look like NaNs) */
    if (nan_only < 0) { const char *s = getenv("SFA_WATCH_NAN"); nan_only = s ? atoi(s) : 0; }
    if (nan_only && (size != 4 || ((value >> 23) & 0xFF) != 0xFF)) return;
    if (nan_only == 1 && (value & 0x7FFFFF)) return;
    if (nan_only) { /* and the guest call stack: these come from deep in shared engine code */
        Ctx *c = rt_current_ctx();
        if (c && hits < 60) {
            printf("[watch] write %08X = %08X (%s) by %s (lr %08X; f0-f5 %g %g %g %g %g %g)", addr, value,
                   (value & 0x7FFFFF) ? "NaN" : "inf", rt_func_containing(c->lr), c->lr, c->f[0].ps0, c->f[1].ps0,
                   c->f[2].ps0, c->f[3].ps0, c->f[4].ps0, c->f[5].ps0);
            for (uint32_t sp = c->r[1], k = 0; k < 10 && sp && ppc_is_ram(sp); k++) {
                uint32_t next = rd32(sp);
                if (!ppc_is_ram(next) || next <= sp) break;
                printf(" < %s", rt_func_containing(rd32(next + 4)));
                sp = next;
            }
            printf("\n");
            fflush(stdout);
        }
        hits++;
        return;
    }
    if (!value) { /* zero fills (heap clears) are summarised, not listed */
        if ((++zero & 0xFFF) == 1) printf("[watch] %u zero writes so far (latest %08X)\n", zero, addr);
        return;
    }
    if (++hits > 60) return;
    printf("[watch] write%d %08X = %0*X in", size * 8, addr, size * 2, value);
    for (uint32_t i = 4; i >= 1; i--) printf(" %s", rt_func_name(g_trace_ring[(g_trace_pos - i) & (RT_TRACE_RING - 1)]));
    printf("\n");
}
static void watch_init(void) {
    const char *w = getenv("SFA_WATCH");
    if (!w) return;
    unsigned lo, len;
    if (sscanf(w, "%x:%x", &lo, &len) == 2) {
        g_watch_lo = lo & 0x01FFFFFFu;
        g_watch_len = len;
        printf("[watch] watching writes to %08X-%08X\n", lo, lo + len - 1);
    }
}
#else
static void watch_init(void) {}
#endif

/* g_rel[i].loaded: 0 never linked, REL_LINKED, or REL_UNLINKED (unlinked, but its code is still where it
 * was: the game calls into a module it has just unlinked, see rel_notify). */
#define REL_LINKED 1
#define REL_UNLINKED 2

static uint32_t g_rel_link_seq[RT_NUM_RELS], g_rel_links; /* when each module was last linked */

/* A runtime address inside a REL module's code -> the address its code was generated at. Linked
 * modules come first. Otherwise the address is in freed memory, which holds the code of the module
 * linked there most recently (unlinked ranges overlap: modules load into the same heap). */
static uint32_t rel_to_generated(uint32_t addr) {
    int best = -1;
    for (unsigned i = 0; i < RT_NUM_RELS; i++) {
        const RelInfo *ri = &g_rel_info[i];
        uint32_t base = g_rel[i].sec[ri->text_sec];
        if (!g_rel[i].loaded || addr < base || addr >= base + ri->text_size) continue;
        if (g_rel[i].loaded == REL_LINKED) return addr - base + ri->vbase;
        if (best < 0 || g_rel_link_seq[i] > g_rel_link_seq[best]) best = (int)i;
    }
    if (best >= 0) return addr - g_rel[best].sec[g_rel_info[best].text_sec] + g_rel_info[best].vbase;
    return addr;
}

static const FuncEntry *find_func(uint32_t addr) {
    if (addr >= 0x80000000u) addr = rel_to_generated(addr);
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_funcs[mid].addr == addr) return &g_funcs[mid];
        if (g_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

/* OSNotifyLink(OSModuleInfo *module): called by the SDK's Link() once a module is relocated.
 * Its section table now holds absolute addresses (and the bss address). Record them so the
 * module's generated code can compute every relocated address. */
static void rel_notify(Ctx *c, int linked) {
    uint32_t m = c->r[3], id = rd32(m), nsec = rd32(m + 0xC), si = rd32(m + 0x10);
    for (unsigned i = 0; i < RT_NUM_RELS; i++) {
        const RelInfo *ri = &g_rel_info[i];
        if (ri->id != id || ri->nsec != nsec) continue;
        int match = 1;
        for (uint32_t k = 0; k < nsec && match; k++) match = rd32(si + k * 8 + 4) == ri->sizes[k];
        if (!match) continue;
        if (linked) {
            for (uint32_t k = 0; k < nsec && k < 32; k++) g_rel[i].sec[k] = rd32(si + k * 8) & ~3u;
            g_rel[i].loaded = REL_LINKED;
            g_rel_link_seq[i] = ++g_rel_links;
            printf("[rel] linked %s: text 0x%08X, %u sections\n", ri->name, g_rel[i].sec[ri->text_sec], nsec);
        } else {
            /* Keep the section addresses: freeing a module doesn't erase it, and the game relies on
             * that. Leaving a stage, fn_8001B098 unlinks the gameplay module (m9 in Mission 1) and its
             * caller fn_8003CE84 then still calls a function pointer into it. On the console that runs
             * the old code from freed memory; here it used to stop with "indirect call to unknown
             * address" (after a game over, or finishing Mission 1). */
            g_rel[i].loaded = REL_UNLINKED;
            printf("[rel] unlinked %s\n", ri->name);
        }
        return;
    }
    rt_fatal("OSNotify%sLink: unknown module id %u with %u sections", linked ? "" : "Un", id, nsec);
}
HLE(OSNotifyLink) { rel_notify(c, 1); }
HLE(OSNotifyUnlink) { rel_notify(c, 0); }

const char *rt_func_name(uint32_t addr) {
    const FuncEntry *e = find_func(addr);
    return e ? e->name : "?";
}

/* SFA_COUNT_CALLS=<name prefix> (testing), e.g. "mb01b:": count calls to the functions whose names
 * start with it (their generated addresses), and print the counts with rt_count_report. */
uint32_t g_count_lo, g_count_hi;
static uint32_t *g_counts;

/* SFA_PROBE=<hex address> (testing, with SFA_COUNT_CALLS covering it): every 997th call of that function,
 * print the 0x120 bytes its first argument (r3) points to, as floats and hex */
static uint32_t g_probe_addr;

static uint32_t g_probe_every = 997;

/* SFA_PROBE_NAN=1: instead, keep each object's last copy while the float at +0x80 is a number, and the
 * first time it is NaN print that copy and the current one (up to 12 objects), to see what broke it */
static int g_probe_nan;
static uint32_t g_probe_field = 0x80; /* SFA_PROBE_FIELD=<hex offset> */
typedef struct { uint32_t obj, reported; uint8_t data[0x120]; } ProbeSnap;
static ProbeSnap *g_snaps;
#define PROBE_SNAPS 16384

static void probe_nan(Ctx *c, uint32_t addr) {
    static int reports;
    uint32_t o = c->r[3];
    if (!ppc_is_ram(o) || reports >= 12) return;
    if (!g_snaps) g_snaps = calloc(PROBE_SNAPS, sizeof *g_snaps);
    ProbeSnap *s = &g_snaps[((o >> 4) * 2654435761u >> 18) & (PROBE_SNAPS - 1)];
    uint32_t w = rd32(o + g_probe_field);
    int nan = ((w >> 23) & 0xFF) == 0xFF;
    if (!nan) {
        s->obj = o;
        s->reported = 0;
        for (int i = 0; i < 0x120; i++) s->data[i] = rd8(o + (uint32_t)i);
        return;
    }
    if (s->obj != o || s->reported) return;
    s->reported = 1;
    reports++;
    printf("[probe-nan] %s obj %08X: field +%X became NaN since the previous call. Changed words (before -> now):\n",
           rt_func_name(addr), o, g_probe_field);
    for (uint32_t k = 0; k < 0x120; k += 4) {
        uint32_t before = (uint32_t)s->data[k] << 24 | s->data[k + 1] << 16 | s->data[k + 2] << 8 | s->data[k + 3];
        uint32_t now = rd32(o + k);
        if (before == now) continue;
        float fb, fn;
        memcpy(&fb, &before, 4);
        memcpy(&fn, &now, 4);
        printf("    +%03X: %08X (%g) -> %08X (%g)\n", k, before, fb, now, fn);
    }
    uint32_t p = o; /* the parent chain (+0x10): world (+0x70) and local (+0x80) positions */
    for (int lvl = 0; lvl < 5 && ppc_is_ram(p); lvl++, p = rd32(p + 0x10)) {
        float v[6];
        for (int k = 0; k < 6; k++) { uint32_t x = rd32(p + (k < 3 ? 0x70 : 0x74) + 4 * k); memcpy(&v[k], &x, 4); }
        printf("    level %d %08X: world (%g, %g, %g) local (%g, %g, %g)\n", lvl, p, v[0], v[1], v[2], v[3], v[4], v[5]);
    }
    fflush(stdout);
}

void rt_count_call(Ctx *c, uint32_t addr) {
    uint32_t n = ++g_counts[(addr - g_count_lo) >> 2];
    if (g_probe_nan && addr == g_probe_addr) { probe_nan(c, addr); return; }
    static int args = -1; /* SFA_PROBE_ARGS=1: each second, the calls to the probed function and its f1 range */
    if (args < 0) args = getenv("SFA_PROBE_ARGS") != NULL;
    if (args && addr == g_probe_addr) {
        static uint64_t sec_start;
        static uint32_t calls;
        static double lo, hi;
        uint64_t t = rt_timebase();
        double f1 = c->f[1].ps0;
        if (!calls) lo = hi = f1;
        if (f1 < lo) lo = f1;
        if (f1 > hi) hi = f1;
        calls++;
        if (!sec_start) sec_start = t;
        if (t - sec_start >= 40500000ull) {
            printf("[probe-args] %s: %u calls in %.2f s; f1 %g .. %g\n", rt_func_name(addr), calls,
                   (double)(t - sec_start) / 40500000.0, lo, hi);
            fflush(stdout);
            sec_start = t;
            calls = 0;
        }
        return;
    }
    if (addr != g_probe_addr || n % g_probe_every != 1 % g_probe_every || !ppc_is_ram(c->r[3])) return;
    printf("[probe] %s #%u obj %08X:", rt_func_name(addr), n, c->r[3]);
    for (int r = 4; r <= 6; r++) /* vector arguments, if r4-r6 point at RAM */
        if (ppc_is_ram(c->r[r])) {
            float v[3];
            for (int k = 0; k < 3; k++) { uint32_t w = rd32(c->r[r] + 4 * k); memcpy(&v[k], &w, 4); }
            printf(" r%d=%08X (%g, %g, %g)", r, c->r[r], v[0], v[1], v[2]);
        }
    for (uint32_t o = 0; o < 0x120; o += 4) {
        uint32_t w = rd32(c->r[3] + o);
        float f;
        memcpy(&f, &w, 4);
        if (o % 32 == 0) printf("\n   +%03X", o);
        if (w && ((w >> 23) & 0xFF) > 0x60 && ((w >> 23) & 0xFF) < 0xA0) printf(" %10.4g", f);
        else printf(" %10X", w);
    }
    printf("\n");
    fflush(stdout);
}

static void count_init(void) {
    const char *p = getenv("SFA_COUNT_CALLS");
    if (!p || !*p) return;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (unsigned i = 0; i < g_func_count; i++)
        if (!strncmp(g_funcs[i].name, p, strlen(p))) {
            if (g_funcs[i].addr < lo) lo = g_funcs[i].addr;
            if (g_funcs[i].addr + 4 > hi) hi = g_funcs[i].addr + 4;
        }
    if (hi <= lo) { printf("[count] no functions named %s*\n", p); return; }
    g_counts = calloc((hi - lo) / 4, sizeof *g_counts);
    g_count_lo = lo;
    g_count_hi = hi;
    printf("[count] counting calls to %s* (%08X-%08X)\n", p, lo, hi);
    const char *pr = getenv("SFA_PROBE");
    if (pr) g_probe_addr = (uint32_t)strtoul(pr, NULL, 16);
    const char *pe = getenv("SFA_PROBE_EVERY"); /* default: every 997th call */
    if (pe && atoi(pe) > 0) g_probe_every = (uint32_t)atoi(pe);
    g_probe_nan = getenv("SFA_PROBE_NAN") != NULL;
    const char *pf = getenv("SFA_PROBE_FIELD");
    if (pf) g_probe_field = (uint32_t)strtoul(pf, NULL, 16);
}

void rt_count_report(void) {
    if (!g_counts) return;
    printf("[count] calls in the last period:");
    int any = 0;
    for (uint32_t a = g_count_lo; a < g_count_hi; a += 4) {
        uint32_t n = g_counts[(a - g_count_lo) >> 2];
        if (!n) continue;
        printf(" %s=%u", rt_func_name(a), n);
        g_counts[(a - g_count_lo) >> 2] = 0;
        any = 1;
    }
    printf(any ? "\n" : " none\n");
    fflush(stdout);
}

/* Name of the function containing addr (for return addresses). */
const char *rt_func_containing(uint32_t addr) {
    if (addr >= 0x80000000u) addr = rel_to_generated(addr);
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_funcs[mid].addr <= addr) lo = mid + 1; else hi = mid;
    }
    return lo ? g_funcs[lo - 1].name : "?";
}

void rt_sdk_enter(Ctx *c, uint32_t addr) {
    g_sdk_calls++;
    if (g_sdk_log && g_sdk_calls <= g_sdk_log_limit)
        fprintf(g_sdk_log, "%6lu %-30s from %08X %-30s r3=%08X r4=%08X r5=%08X\n", g_sdk_calls, rt_func_name(addr),
                c->lr, rt_func_containing(c->lr), c->r[3], c->r[4], c->r[5]);
}

void rt_call(Ctx *c, uint32_t addr) {
    const FuncEntry *e = find_func(addr);
    if (!e) rt_fatal("indirect call to unknown address 0x%08X (lr=0x%08X)", addr, c->lr);
    e->fn(c);
}

void rt_jump(Ctx *c, uint32_t addr) {
    const FuncEntry *e = find_func(addr);
    if (!e) rt_fatal("jump to an address that is not a function start: 0x%08X", addr);
    e->fn(c);
}

/* ---- hooks used by generated code -------------------------------------------------------------- */

void rt_trap(Ctx *c, uint32_t pc) { (void)c; rt_fatal("trap at 0x%08X", pc); }
void rt_illegal(Ctx *c, uint32_t pc, uint32_t insn) { (void)c; rt_fatal("illegal instruction 0x%08X at 0x%08X", insn, pc); }
/* The SDK only uses `sc` as a full synchronization barrier (DCFlushRange, ICFlashInvalidate). */
void rt_syscall(Ctx *c, uint32_t pc) { (void)c; (void)pc; }

void rt_hle_unimplemented(Ctx *c, uint32_t addr, const char *name) {
    fprintf(stderr, "[recomp] stub called: %s (0x%08X) from 0x%08X\n", name, addr, c->lr);
    c->r[3] = 0;
}

/* Gekko timebase: bus clock / 4 = 40.5 MHz, from a high-resolution monotonic host clock.
 * (The SDK times hardware events with it, e.g. __AI_SRC_INIT measures 1/48000 s.) */
uint64_t rt_timebase(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER t;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&t);
    uint64_t ticks = (uint64_t)(t.QuadPart - start.QuadPart);
    return ticks / (uint64_t)freq.QuadPart * 40500000u + ticks % (uint64_t)freq.QuadPart * 40500000u / (uint64_t)freq.QuadPart;
#else
    static struct timespec start;
    struct timespec ts;
    if (!start.tv_sec && !start.tv_nsec) clock_gettime(CLOCK_MONOTONIC, &start);
    clock_gettime(CLOCK_MONOTONIC, &ts); /* counted from the first call, like the Windows branch */
    int64_t ns = (int64_t)(ts.tv_sec - start.tv_sec) * 1000000000 + (ts.tv_nsec - start.tv_nsec);
    return (uint64_t)(ns / 1000000000) * 40500000u + (uint64_t)(ns % 1000000000) * 405u / 10000u;
#endif
}

void rt_dcbz(uint32_t addr) {
    addr &= ~31u;
    for (int i = 0; i < 32; i += 4) wr32(addr + i, 0);
}

void rt_icache_invalidate(uint32_t addr) { (void)addr; }

void rt_sleep_us(unsigned us) {
#ifdef _WIN32
    Sleep(us / 1000);
#else
    usleep(us);
#endif
}

void rt_lswi(Ctx *c, uint32_t ea, int rd, int nb) {
    int r = rd - 1, i = 0;
    while (nb-- > 0) {
        if (i == 0) { r = (r + 1) & 31; c->r[r] = 0; }
        c->r[r] |= (uint32_t)rd8(ea++) << (24 - i * 8);
        i = (i + 1) & 3;
    }
}

void rt_stswi(Ctx *c, uint32_t ea, int rs, int nb) {
    int r = rs - 1, i = 0;
    while (nb-- > 0) {
        if (i == 0) r = (r + 1) & 31;
        wr8(ea++, (uint8_t)(c->r[r] >> (24 - i * 8)));
        i = (i + 1) & 3;
    }
}

/* ---- slow memory paths ------------------------------------------------------------------------- */

/* A hardware register that is read millions of times means the guest is spinning on hardware that
 * does not exist here: stop and report which function did it. */
static void poll_watchdog(uint32_t a) {
    static uint32_t last_addr;
    static unsigned long repeats;
    if (a == last_addr) {
        if (++repeats == 2000000) rt_fatal("guest is polling hardware register 0x%08X (needs an HLE override)", a);
    } else {
        last_addr = a;
        repeats = 0;
    }
}

/* Hardware registers at 0xCC000000-0xCC007FFF: a plain register file, so the SDK reads back what it
 * wrote. Devices that need behaviour (status bits, interrupts) poke values into it. */
static uint8_t g_mmio[0x10000];

static uint32_t mmio_get(uint32_t off, int size) {
    uint8_t *p = g_mmio + off;
    return size == 1 ? p[0] : size == 2 ? (uint32_t)(p[0] << 8 | p[1])
                                        : (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void mmio_set(uint32_t off, uint32_t v, int size) {
    for (int i = 0; i < size; i++) g_mmio[off + i] = (uint8_t)(v >> ((size - 1 - i) * 8));
}

uint32_t rt_mmio_get(uint32_t off, int size) { return mmio_get(off, size); }
void rt_mmio_set(uint32_t off, uint32_t v, int size) { mmio_set(off, v, size); }

/* ARAM: 16 MB of auxiliary memory reached only by DMA (0xCC005020 main address, 0x5024 ARAM
 * address, 0x5028 count with bit 31 = ARAM->main). Transfers finish instantly and raise the
 * ARAM interrupt; 0x5016 bit 0 reports the ARAM controller as ready. */
#ifndef DSP_CSR
#define DSP_CSR 0x500Au
#endif
#define ARAM_SIZE 0x01000000u
#define AR_MODE 0x5016u
#define OS_INTERRUPT_DSP_ARAM 6
static uint8_t *g_aram;

const uint8_t *rt_aram(void) { return g_aram; }

/* ---- AI DMA: the DSP's audio interface plays a buffer from main memory --------------------------
 * 0x5030/0x5032 start address, 0x5036 bit 15 = enable and bits 0-14 = length in 32-byte blocks,
 * 0x503A = blocks left. When a buffer finishes (real time, 32 kHz stereo) the samples go to the
 * host audio device, AIDINT (CSR 0x08) is raised and the next buffer is latched from the registers. */
#define OS_INTERRUPT_DSP_AI 5
static int g_ai_running;
static uint32_t g_ai_addr, g_ai_len;
static uint64_t g_ai_start, g_ai_end; /* timebase ticks */

static void ai_latch(uint64_t now) {
    g_ai_addr = 0x80000000u | ((mmio_get(0x5030, 2) << 16 | mmio_get(0x5032, 2)) & 0x03FFFFE0u);
    g_ai_len = (mmio_get(0x5036, 2) & 0x7FFF) * 32;
    g_ai_start = now;
    g_ai_end = now + (uint64_t)g_ai_len / 4 * 40500000u / 32000u; /* stereo s16 frames at 32 kHz */
    if (!g_ai_len) g_ai_end = now + 40500000u / 200;
}

void rt_ai_tick(void) {
    int enabled = (mmio_get(0x5036, 2) >> 15) & 1;
    uint64_t now = rt_timebase();
    if (!enabled) { g_ai_running = 0; return; }
    if (!g_ai_running) { g_ai_running = 1; ai_latch(now); return; }
    if (now < g_ai_end) return;
    /* this buffer finished playing */
    static int16_t pcm[0x8000];
    uint32_t frames = g_ai_len / 4;
    if (frames > sizeof pcm / 4) frames = sizeof pcm / 4;
    for (uint32_t i = 0; i < frames * 2; i++) pcm[i] = (int16_t)rd16(g_ai_addr + i * 2);
    video_audio_push(pcm, (int)frames);
    mmio_set(DSP_CSR, mmio_get(DSP_CSR, 2) | 0x08u, 2); /* AIDINT */
    rt_raise_interrupt(OS_INTERRUPT_DSP_AI);
    if (getenv("SFA_AX_DEBUG")) {
        static unsigned long fires;
        static uint64_t t0;
        extern unsigned long g_ax_frames;
        if (!t0) t0 = now;
        if (++fires % 1000 == 0)
            printf("[ai] %lu buffers done, %lu AX frames, %.1f s, buffer %u bytes, late by %.1f ms\n", fires, g_ax_frames,
                   (double)(now - t0) / 40500000.0, g_ai_len, (double)(int64_t)(now - g_ai_end) / 40500.0);
    }
    ai_latch(g_ai_end > now - 40500000u / 20 ? g_ai_end : now); /* keep time unless far behind */
}

uint64_t rt_ai_next_deadline(void) { return g_ai_running ? g_ai_end : 0; }

static uint32_t ai_blocks_left(void) {
    if (!g_ai_running || g_ai_end <= g_ai_start) return 0;
    uint64_t now = rt_timebase();
    if (now >= g_ai_end) return 0;
    return (uint32_t)((g_ai_end - now) * (g_ai_len / 32) / (g_ai_end - g_ai_start));
}

static void aram_dma(void) {
    uint32_t mm = (uint32_t)mmio_get(0x5020, 4) & 0x03FFFFE0u;
    uint32_t ar = (uint32_t)mmio_get(0x5024, 4) & 0x03FFFFE0u;
    uint32_t cnt = (uint32_t)mmio_get(0x5028, 4);
    uint32_t len = cnt & 0x03FFFFE0u;
    if (!g_aram) g_aram = calloc(1, ARAM_SIZE);
    if (getenv("SFA_GX_DEBUG"))
        printf("[aram] %s main %08X aram %08X len 0x%X\n", cnt & 0x80000000u ? "ARAM->main" : "main->ARAM",
               0x80000000u | mm, ar, len);
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = (ar + i) & (ARAM_SIZE - 1), m = 0x80000000u | (mm + i);
        if (!ppc_is_ram(m)) rt_fatal("ARAM DMA outside main RAM: 0x%08X", m);
        if (cnt & 0x80000000u) *ppc_ram(m) = g_aram[a];
        else g_aram[a] = *ppc_ram(m);
    }
    mmio_set(0x500A, mmio_get(0x500A, 2) | 0x20u, 2); /* AR interrupt status */
    rt_raise_interrupt(OS_INTERRUPT_DSP_ARAM);
}

/* fake DSP mailbox (defined below) */
#define DSP_MBOX_IN_H 0x5000u  /* CPU -> DSP */
#define DSP_MBOX_IN_L 0x5002u
#define DSP_CSR 0x500Au
static int dsp_read(uint32_t off, uint32_t *out);
static void dsp_receive(uint32_t mail);
static void dsp_send(uint32_t mail);

uint32_t rt_slow_read(uint32_t a, int size) {
    if ((a & 0xFFFFC000u) == 0xE0000000u) {
        uint8_t *p = g_lc + (a & (PPC_LC_SIZE - 1));
        return size == 1 ? p[0] : size == 2 ? (uint32_t)(p[0] << 8 | p[1])
                                            : (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    }
    if ((a >> 16) != 0xCC00) rt_fatal("read%d from unmapped address 0x%08X", size * 8, a);
    uint32_t off = a & 0xFFFF;
    if (off == 0x6C08) /* AI sample counter: runs at 48 kHz */
        return (uint32_t)(rt_timebase() / (40500000 / 48000));
    uint32_t v;
    if (dsp_read(off, &v)) return v;
    if (off == AR_MODE) return 1; /* ARAM controller initialized */
    if (off == 0x503A) return ai_blocks_left();
    if (off == 0x3014 && size == 4) return gx_pi_wptr_read(); /* CPU FIFO write pointer */
    poll_watchdog(a);
    return mmio_get(off, size);
}

void rt_slow_write(uint32_t a, uint32_t v, int size) {
    if ((a & 0xFFFFC000u) == 0xE0000000u) {
        uint8_t *p = g_lc + (a & (PPC_LC_SIZE - 1));
        for (int i = 0; i < size; i++) p[i] = (uint8_t)(v >> ((size - 1 - i) * 8));
        return;
    }
    if ((a >> 16) != 0xCC00) rt_fatal("write%d 0x%08X to unmapped address 0x%08X", size * 8, v, a);
    uint32_t off = a & 0xFFFF;
    if (off >= 0x8000) { /* GX write-gather pipe */
        if (off < 0x9000) gx_fifo_write(v, size);
        return;
    }
    if (rt_dvd_write(off, v, size)) return;
    if (off == 0x3014 && size == 4) gx_pi_wptr_write(v);
    switch (off) {
    case 0x680C: case 0x6820: case 0x6834: /* EXI channel 0-2 control: TSTART completes at once */
    case 0x6434:                           /* SI communication control: TSTART */
        v &= ~1u;
        break;
    case DSP_MBOX_IN_L: /* the mail is complete; the fake DSP consumes it at once */
        dsp_receive(mmio_get(DSP_MBOX_IN_H, 2) << 16 | (v & 0xFFFF));
        mmio_set(DSP_MBOX_IN_H, mmio_get(DSP_MBOX_IN_H, 2) & 0x7FFF, 2);
        break;
    case DSP_CSR: /* writing 1 to the interrupt bits acknowledges them */
        v = (v & ~0xA8u) | (mmio_get(DSP_CSR, 2) & 0xA8u & ~(v & 0xA8u));
        break;
    }
    mmio_set(off, v, size);
    if ((off == 0x5028 && size == 4) || off == 0x502A) aram_dma(); /* writing the count starts it */
}

/* ---- fake DSP mailbox ---------------------------------------------------------------------------
 * Enough of the DSP side of the mailbox protocol for the SDK's task loader (__DSP_boot_task) and
 * __DSPHandler: greet with 0x8071FEED, accept CPU mails instantly, and once a task's boot sequence
 * ends (0x80F3B002 followed by its argument) report "task initialized" (0xDCD10000) via a DSP
 * interrupt. No microcode runs, so audio stays silent. */
#define DSP_MBOX_IN_H 0x5000u  /* CPU -> DSP */
#define DSP_MBOX_IN_L 0x5002u
#define DSP_MBOX_OUT_H 0x5004u /* DSP -> CPU */
#define DSP_MBOX_OUT_L 0x5006u
#define DSP_CSR 0x500Au
#define DSP_CSR_DSPINT 0x80u
#define OS_INTERRUPT_DSP_DSP 7

static uint32_t g_dsp_out[16];
static unsigned g_dsp_out_head, g_dsp_out_tail;
static int g_dsp_boot_stage; /* 1 = saw 0x80F3B002, next mail ends the boot sequence */
static unsigned long g_dsp_mails_in;

static void dsp_send(uint32_t mail) { g_dsp_out[g_dsp_out_tail++ & 15] = mail; }

static void dsp_interrupt(uint32_t mail) {
    dsp_send(mail);
    mmio_set(DSP_CSR, mmio_get(DSP_CSR, 2) | DSP_CSR_DSPINT, 2);
    rt_raise_interrupt(OS_INTERRUPT_DSP_DSP);
}

static void dsp_receive(uint32_t mail) {
    static int expect_cl; /* the AX microcode: 0xBABE0180, then the command list address */
    g_dsp_mails_in++;
    if (expect_cl) {
        expect_cl = 0;
        rt_ax_command_list(0x80000000u | (mail & 0x03FFFFFFu));
        extern unsigned long g_ax_frames, g_ax_voices_played;
        if (getenv("SFA_AX_DEBUG") && g_ax_frames % 1000 == 1)
            printf("[ax] frame %lu, %lu voice-frames rendered so far\n", g_ax_frames, g_ax_voices_played);
        dsp_interrupt(0xDCD10001u); /* frame done: the task "resumes" and AX may start the next frame */
    } else if (mail == 0xBABE0180u) {
        expect_cl = 1;
    } else if (g_dsp_boot_stage == 1) {
        g_dsp_boot_stage = 0;
        dsp_interrupt(0xDCD10000u); /* task init done */
    } else if (mail == 0x80F3B002u) {
        g_dsp_boot_stage = 1;
    }
}

static int dsp_read(uint32_t off, uint32_t *out) {
    int has = g_dsp_out_head != g_dsp_out_tail;
    if (off == DSP_MBOX_OUT_H) { *out = has ? g_dsp_out[g_dsp_out_head & 15] >> 16 : 0; return 1; }
    if (off == DSP_MBOX_OUT_L) {
        *out = has ? g_dsp_out[g_dsp_out_head++ & 15] & 0xFFFF : 0;
        return 1;
    }
    return 0;
}

/* ---- devices ------------------------------------------------------------------------------------ */

#define VI_DI0 0x2030u /* VI display interrupt 0: bit 15 = interrupt pending */

void rt_vi_retrace(void) { mmio_set(VI_DI0, mmio_get(VI_DI0, 2) | 0x8000u, 2); }

/* ---- loading ----------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* Load the DOL from the disc (its offset is in boot.bin). It must be the one the code was translated
 * from: a different region or revision would run mistranslated code. */
static void load_dol(const uint8_t *boot) {
    uint32_t dol_off = be32(boot + 0x420);
    uint8_t *hdr = rt_disc_alloc(dol_off, 0x100);
    long size = 0x100;
    for (int i = 0; i < 18; i++) {
        uint32_t end = be32(hdr + i * 4) + be32(hdr + 0x90 + i * 4);
        if (be32(hdr + 0x90 + i * 4) && end > (uint32_t)size) size = end;
    }
    free(hdr);
    uint8_t *dol = rt_disc_alloc(dol_off, (uint32_t)size);
    if ((uint32_t)size != RT_DOL_SIZE || crc32_of(dol, (size_t)size) != RT_DOL_CRC32)
        rt_fatal("this disc's main.dol (%ld bytes, CRC32 %08X) isn't the one this program was built from "
                 "(%u bytes, CRC32 %08X): it needs the same Star Fox: Assault USA (GF7E01) disc",
                 size, crc32_of(dol, (size_t)size), RT_DOL_SIZE, RT_DOL_CRC32);
    for (int i = 0; i < 18; i++) {
        uint32_t off = be32(dol + i * 4), addr = be32(dol + 0x48 + i * 4), len = be32(dol + 0x90 + i * 4);
        if (!len) continue;
        if (!ppc_is_ram(addr) || off + len > (uint32_t)size) rt_fatal("bad DOL section %d", i);
        memcpy(ppc_ram(addr), dol + off, len);
    }
    free(dol);
}

/* Recreate what the IPL and apploader leave in low memory before jumping to the DOL entry point.
 * Layout: OSBootInfo at 0x80000000 and the globals around 0x800000C0-0x800000FC / 0x800030xx. */
void rt_boot_setup(const char *root) {
    rt_dvd_init(root);
    uint8_t *boot = rt_disc_alloc(0, 0x440);
    if (memcmp(boot, "GF7E01", 6) != 0)
        rt_fatal("%s is not Star Fox: Assault USA (disc ID %.6s, need GF7E01)", root, (const char *)boot);
    load_dol(boot);
    memcpy(ppc_ram(0x80000000u), boot, 0x20); /* disc ID: game code, maker, disc number, version */

    long size = (long)be32(boot + 0x428);
    uint8_t *fst = rt_disc_alloc(be32(boot + 0x424), (uint32_t)size);
    free(boot);
    uint32_t fst_addr = (0x81800000u - (uint32_t)size) & ~31u;
    memcpy(ppc_ram(fst_addr), fst, (size_t)size);
    free(fst);

    wr32(0x80000020u, 0x0D15EA5Eu);   /* boot magic */
    wr32(0x80000024u, 1);             /* version */
    wr32(0x80000028u, PPC_MEM_SIZE);  /* physical memory size */
    wr32(0x8000002Cu, 0x00000003u);   /* console type: retail */
    wr32(0x80000030u, 0);             /* arena lo: 0 = use __ArenaLo */
    wr32(0x80000034u, fst_addr);      /* arena hi: below the FST */
    wr32(0x80000038u, fst_addr);      /* FST location */
    wr32(0x8000003Cu, (uint32_t)size);/* FST max size */
    wr32(0x800000CCu, 0);             /* TV mode: NTSC */
    wr32(0x800000F0u, PPC_MEM_SIZE);  /* simulated memory size */
    wr32(0x800000F8u, 162000000u);    /* bus clock */
    wr32(0x800000FCu, 486000000u);    /* CPU clock */
    dsp_send(0x8071FEEDu);            /* the DSP greets the task loader after reset */
    printf("[recomp] boot: FST at 0x%08X (%ld bytes), arena hi 0x%08X\n", fst_addr, size, fst_addr);
}

/* ---- self-test: run recompiled library code and compare with the host C library ---------------- */

static GuestFn func_by_name(const char *name) {
    for (unsigned i = 0; i < g_func_count; i++)
        if (!strcmp(g_funcs[i].name, name)) return g_funcs[i].fn;
    rt_fatal("no guest function named %s", name);
    return NULL;
}

static void put_str(uint32_t a, const char *s) { do wr8(a++, (uint8_t)*s); while (*s++); }
static void get_str(uint32_t a, char *out, size_t n) {
    size_t i = 0;
    while (i + 1 < n && (out[i] = (char)rd8(a + (uint32_t)i))) i++;
    out[i] = 0;
}

static int check(const char *what, int ok, const char *got, const char *want) {
    printf("  %-34s %s", what, ok ? "ok" : "FAILED");
    if (!ok || got) printf("   got \"%s\"%s%s%s", got ? got : "", want ? " want \"" : "", want ? want : "", want ? "\"" : "");
    printf("\n");
    return ok;
}

static int selftest(void) {
    Ctx *c = &g_ctx;
    char got[256], want[256];
    int pass = 0, total = 0;
    const uint32_t buf = 0x80500000u, fmt = 0x80500200u, str = 0x80500300u;

    printf("[recomp] self-test (guest code from main.dol)\n");
    func_by_name("__init_registers")(c);
    snprintf(got, sizeof got, "r1=%08X r2=%08X r13=%08X", c->r[1], c->r[2], c->r[13]);
    total++, pass += check("__init_registers sets r1/r2/r13", c->r[1] && c->r[2] && c->r[13], got, NULL);

    c->r[3] = buf; c->r[4] = 0x5A; c->r[5] = 64; c->lr = 0;
    func_by_name("memset")(c);
    int ok = c->r[3] == buf && rd8(buf) == 0x5A && rd8(buf + 63) == 0x5A && rd8(buf + 64) == 0;
    total++, pass += check("memset(buf, 0x5A, 64)", ok, NULL, NULL);

    put_str(str, "Star Fox: Assault");
    c->r[3] = str;
    func_by_name("strlen")(c);
    snprintf(got, sizeof got, "%u", c->r[3]);
    total++, pass += check("strlen(\"Star Fox: Assault\")", c->r[3] == 17, got, "17");

    put_str(fmt, "%s|%d|%5d|%-4d|%08X|%c|%u");
    c->r[3] = buf; c->r[4] = fmt; c->r[5] = str; c->r[6] = 2005; c->r[7] = 42; c->r[8] = (uint32_t)-7;
    c->r[9] = 0xDEADBEEF; c->r[10] = 'Z';
    c->r[1] -= 64; wr32(c->r[1] + 8, 4000000000u); /* 8th variadic argument goes on the stack */
    cr_setbit(c, 6, 0);                             /* no floating-point arguments */
    func_by_name("sprintf")(c);
    c->r[1] += 64;
    get_str(buf, got, sizeof got);
    snprintf(want, sizeof want, "%s|%d|%5d|%-4d|%08X|%c|%u", "Star Fox: Assault", 2005, 42, -7, 0xDEADBEEFu, 'Z', 4000000000u);
    total++, pass += check("sprintf integers and strings", !strcmp(got, want), got, want);

    put_str(fmt, "%.3f|%8.2f|%e|%g");
    c->r[3] = buf; c->r[4] = fmt;
    c->f[1].ps0 = 3.14159265; c->f[2].ps0 = -1234.5; c->f[3].ps0 = 6.02e23; c->f[4].ps0 = 0.0001;
    cr_setbit(c, 6, 1); /* floating-point arguments in f1-f8 */
    func_by_name("sprintf")(c);
    get_str(buf, got, sizeof got);
    snprintf(want, sizeof want, "%.3f|%8.2f|%e|%g", 3.14159265, -1234.5, 6.02e23, 0.0001);
    total++, pass += check("sprintf floating point", !strcmp(got, want), got, want);

    printf("[recomp] self-test: %d/%d passed\n", pass, total);
    return pass == total ? 0 : 1;
}

/* ---- entry point ------------------------------------------------------------------------------- */

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int has_suffix(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return 0;
    for (size_t k = 0; k < e; k++) {
        char a = name[n - e + k], b = ext[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int disc_image_id_ok(const char *path) {
    char id[6] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(id, 1, 6, f);
    fclose(f);
    return got == 6 && memcmp(id, "GF7E01", 6) == 0;
}

/* Look for the game in `dir`: an extracted disc at orig/GF7E01, or a disc image (.iso/.gcm) of
 * Star Fox: Assault USA. Compressed images Dolphin makes (.rvz, .wia, .gcz, .ciso, .wbfs, .nkit)
 * can't be read directly; *other_format names one if seen. */
static int find_game_in(const char *dir, char *out, size_t n, char *other_format, size_t on) {
    char probe[1024];
    snprintf(probe, sizeof probe, "%s/orig/GF7E01/sys/main.dol", dir);
    if (file_exists(probe)) { snprintf(out, n, "orig/GF7E01"); return 1; }
    static const char *const images[] = {".iso", ".gcm"};
    static const char *const others[] = {".rvz", ".wia", ".gcz", ".ciso", ".wbfs", ".nkit.iso"};
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        const char *name = fd.cFileName;
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        const char *name = e->d_name;
#endif
        int other = 0;
        for (size_t k = 0; k < sizeof others / sizeof others[0]; k++) other |= has_suffix(name, others[k]);
        if (other) { snprintf(other_format, on, "%s", name); continue; }
        for (size_t k = 0; k < sizeof images / sizeof images[0] && !found; k++) {
            if (!has_suffix(name, images[k])) continue;
            snprintf(probe, sizeof probe, "%s/%s", dir, name);
            if (disc_image_id_ok(probe)) { snprintf(out, n, "%s", name); found = 1; }
        }
#ifdef _WIN32
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    return found;
}

/* Find the game: in the current folder, else next to the executable (then run from there, so saves
 * go to its user/ folder too; this is how a release runs from a shortcut, a file manager or Steam). */
static const char *find_game(int required) {
    static char found[1024];
    char other[256] = "", dir[1024] = "";
    if (find_game_in(".", found, sizeof found, other, sizeof other)) return found;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (n == 0 || n >= sizeof dir) dir[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    dir[n > 0 ? n : 0] = 0;
#endif
    char *slash = strrchr(dir, '/'), *bslash = strrchr(dir, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) {
        *slash = 0;
        if (find_game_in(dir, found, sizeof found, other, sizeof other)) {
#ifdef _WIN32
            SetCurrentDirectoryA(dir);
#else
            if (chdir(dir) != 0) return NULL;
#endif
            printf("[recomp] running from %s\n", dir);
            return found;
        }
    }
    if (!required) { /* the launcher will ask: keep its settings and saves next to the program */
        if (slash) {
#ifdef _WIN32
            SetCurrentDirectoryA(dir);
#else
            if (chdir(dir) != 0) return NULL;
#endif
        }
        return NULL;
    }
    if (other[0])
        rt_fatal("%s is a compressed disc image, which can't be read directly. In Dolphin, right-click the "
                 "game, choose Convert File..., pick the ISO format, and put the .iso next to this program.",
                 other);
    rt_fatal("Star Fox: Assault wasn't found. Put a disc image of it (.iso or .gcm, USA, GF7E01) next to this "
             "program, or an extracted disc at orig/GF7E01/ (sys/ and files/), or name either on the "
             "command line.");
    return NULL;
}

static void usage(void) {
    printf("usage: sfa [options] [disc image (.iso/.gcm) or extracted disc folder]\n"
           "  (without one: an .iso/.gcm or orig/GF7E01 in this folder or next to the program)\n"
           "  (with neither, a start screen lets you pick the disc and press PLAY)\n"
           "  --play                skip the start screen\n"
           "  --fullscreen          start fullscreen (F11 or Alt+Enter toggles)\n"
           "  --window=WxH          window size (default 1280x960)\n"
           "  --no-video            run without a window (testing)\n"
           "  --stage=N             start stage N of the stage table instead of Mission 1\n"
           "  --auto-press          press buttons automatically (testing)\n"
           "  --auto-keys=a,w,...   scripted button presses, one per 1.5 s (testing)\n"
           "  --dump-frames         save every 15th frame to build/GF7E01/frames\n"
           "  --selftest            check the recompiled code and exit\n");
}

int main(int argc, char **argv) {
    const char *root = NULL, *sdk_log = "build/GF7E01/sdk_trace.txt";
    int run_selftest = 0, dump_frames = 0, no_video = 0, root_given = 0, testing = 0, skip_launcher = 0, fullscreen = 0;
    for (int i = 1; i < argc; i++) {
        int w, h;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--fullscreen")) { fullscreen = 1; video_set_window(1, 0, 0); }
        else if (!strcmp(argv[i], "--play")) skip_launcher = 1;
        else if (!strncmp(argv[i], "--window=", 9) && sscanf(argv[i] + 9, "%dx%d", &w, &h) == 2) video_set_window(0, w, h);
        else if (!strcmp(argv[i], "--selftest")) run_selftest = 1;
        else if (!strncmp(argv[i], "--sdk-log=", 10)) sdk_log = argv[i] + 10;
        else if (!strncmp(argv[i], "--sdk-log-limit=", 16)) g_sdk_log_limit = strtoul(argv[i] + 16, NULL, 0);
        else if (!strcmp(argv[i], "--dump-frames")) dump_frames = testing = 1;
        else if (!strcmp(argv[i], "--no-video")) no_video = 1;
        else if (!strcmp(argv[i], "--auto-press")) { video_set_auto_press(1); testing = 1; }
        else if (!strncmp(argv[i], "--stage=", 8)) { g_start_stage = atoi(argv[i] + 8); testing = 1; }
        else if (!strncmp(argv[i], "--auto-keys=", 12)) { video_set_auto_keys(argv[i] + 12); testing = 1; }
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); usage(); return 2; }
        else { root = argv[i]; root_given = 1; }
    }
    g_no_dialogs = testing || no_video || run_selftest;
    if (!root_given) {
        /* the start screen, unless testing or told to skip it */
        int launcher = !skip_launcher && !testing && !no_video && !run_selftest && !getenv("SFA_NO_LAUNCHER");
        root = find_game(!launcher);
        if (launcher) {
            static char chosen[1024];
            int r = launcher_run(root, &fullscreen, chosen, sizeof chosen);
            if (r == 0) return 0; /* QUIT */
            if (r == 1) {
                root = chosen;
                video_set_window(fullscreen, 0, 0);
            } else if (!root) {
                root = find_game(1); /* no start screen possible: look for the game as before */
            }
        }
    }
    if (!testing && !run_selftest) g_force_60fps = launcher_fps60(); /* the saved choice, with or without the start screen */
    if (!run_selftest) crash_handling_init();
    g_mem = calloc(1, PPC_MEM_SIZE);
    g_lc = calloc(1, PPC_LC_SIZE);
    if (!g_mem || !g_lc) rt_fatal("out of memory");
    watch_init();
    count_init();
    rt_boot_setup(root);
    printf("[recomp] %u guest functions linked\n", g_func_count);
    if (run_selftest) return selftest();

    if (*sdk_log && (g_sdk_log = fopen(sdk_log, "w"))) g_trace_sdk = 1;
    printf("[recomp] booting: __start at 0x%08X (SDK call log: %s)\n", G___start, *sdk_log ? sdk_log : "off");
    gx_init();
    if (!no_video) video_start(dump_frames);
    rt_threads_init(&g_ctx);
    F___start(&g_ctx);
    printf("[recomp] __start returned\n");
    if (g_sdk_log) fclose(g_sdk_log);
    return 0;
}

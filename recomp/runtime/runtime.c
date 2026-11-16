/* runtime.c - host runtime for the recompiled game.
 *
 * Guest memory, the indirect-call dispatcher, tracing and diagnostics, boot setup (the low-memory
 * state the GameCube IPL and apploader leave behind) and the entry point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
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

void rt_dump_trace(void) {
    fprintf(stderr, "[recomp] last functions entered (oldest first):\n");
    uint32_t n = g_trace_pos < RT_TRACE_RING ? g_trace_pos : RT_TRACE_RING;
    for (uint32_t i = g_trace_pos - n; i != g_trace_pos; i++) {
        uint32_t a = g_trace_ring[i & (RT_TRACE_RING - 1)];
        if (i + 24 >= g_trace_pos) fprintf(stderr, "    %08X %s\n", a, rt_func_name(a));
    }
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    va_start(ap, fmt);
    fprintf(stderr, "[recomp] fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    Ctx *c = rt_current_ctx();
    if (!c) c = &g_ctx;
    fprintf(stderr, "[recomp] registers: r1=%08X lr=%08X ctr=%08X r3=%08X r4=%08X r5=%08X msr=%08X\n",
            c->r[1], c->lr, c->ctr, c->r[3], c->r[4], c->r[5], c->msr);
    rt_dump_trace();
    if (g_sdk_log) fflush(g_sdk_log);
    exit(1);
}

void rt_console_putc(char ch) { fputc(ch, stdout); if (ch == '\n') fflush(stdout); }

/* ---- function lookup --------------------------------------------------------------------------- */

#ifdef RT_WATCH
/* Write watchpoint (debug builds only): SFA_WATCH=<hex address>:<hex length>. */
uint32_t g_watch_lo = 0xFFFFFFFFu, g_watch_len = 0;
void rt_watch_hit(uint32_t addr, uint32_t value, int size) {
    static unsigned hits, zero;
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

/* A runtime address inside a linked REL module's code -> the address its code was generated at. */
static uint32_t rel_to_generated(uint32_t addr) {
    for (unsigned i = 0; i < RT_NUM_RELS; i++) {
        const RelInfo *ri = &g_rel_info[i];
        uint32_t base = g_rel[i].sec[ri->text_sec];
        if (g_rel[i].loaded && addr >= base && addr < base + ri->text_size) return addr - base + ri->vbase;
    }
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
            g_rel[i].loaded = 1;
            printf("[rel] linked %s: text 0x%08X, %u sections\n", ri->name, g_rel[i].sec[ri->text_sec], nsec);
        } else {
            g_rel[i].loaded = 0;
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

/* Name of the function containing addr (for return addresses). */
static const char *func_containing(uint32_t addr) {
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
                c->lr, func_containing(c->lr), c->r[3], c->r[4], c->r[5]);
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

static uint8_t *read_file(const char *path, long *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size + 1);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) rt_fatal("cannot read %s", path);
    fclose(f);
    *size_out = size;
    return data;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void load_dol(const char *path) {
    long size;
    uint8_t *dol = read_file(path, &size);
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
    char path[1024];
    long size;

    snprintf(path, sizeof path, "%s/sys/main.dol", root);
    load_dol(path);

    snprintf(path, sizeof path, "%s/sys/boot.bin", root);
    uint8_t *boot = read_file(path, &size);
    memcpy(ppc_ram(0x80000000u), boot, 0x20); /* disc ID: game code, maker, disc number, version */
    free(boot);

    snprintf(path, sizeof path, "%s/sys/fst.bin", root);
    uint8_t *fst = read_file(path, &size);
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
    rt_dvd_init(root);
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

int main(int argc, char **argv) {
    const char *root = "orig/GF7E01", *sdk_log = "build/GF7E01/sdk_trace.txt";
    int run_selftest = 0, dump_frames = 0, no_video = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) run_selftest = 1;
        else if (!strncmp(argv[i], "--sdk-log=", 10)) sdk_log = argv[i] + 10;
        else if (!strncmp(argv[i], "--sdk-log-limit=", 16)) g_sdk_log_limit = strtoul(argv[i] + 16, NULL, 0);
        else if (!strcmp(argv[i], "--dump-frames")) dump_frames = 1;
        else if (!strcmp(argv[i], "--no-video")) no_video = 1;
        else if (!strcmp(argv[i], "--auto-press")) video_set_auto_press(1);
        else root = argv[i];
    }
    g_mem = calloc(1, PPC_MEM_SIZE);
    g_lc = calloc(1, PPC_LC_SIZE);
    if (!g_mem || !g_lc) rt_fatal("out of memory");
    watch_init();
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

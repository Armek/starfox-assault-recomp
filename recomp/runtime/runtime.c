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
    fprintf(stderr, "[recomp] pc trail: r1=%08X lr=%08X ctr=%08X r3=%08X r4=%08X r5=%08X\n",
            g_ctx.r[1], g_ctx.lr, g_ctx.ctr, g_ctx.r[3], g_ctx.r[4], g_ctx.r[5]);
    rt_dump_trace();
    if (g_sdk_log) fflush(g_sdk_log);
    exit(1);
}

void rt_console_putc(char ch) { fputc(ch, stdout); if (ch == '\n') fflush(stdout); }

/* ---- function lookup --------------------------------------------------------------------------- */

static const FuncEntry *find_func(uint32_t addr) {
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_funcs[mid].addr == addr) return &g_funcs[mid];
        if (g_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

const char *rt_func_name(uint32_t addr) {
    const FuncEntry *e = find_func(addr);
    return e ? e->name : "?";
}

/* Name of the function containing addr (for return addresses). */
static const char *func_containing(uint32_t addr) {
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

uint64_t rt_timebase(void) {
    /* Gekko timebase runs at bus clock / 4 = 40.5 MHz. */
    return (uint64_t)((double)clock() / CLOCKS_PER_SEC * 40500000.0);
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

uint32_t rt_slow_read(uint32_t a, int size) {
    if ((a & 0xFFFFC000u) == 0xE0000000u) {
        uint8_t *p = g_lc + (a & (PPC_LC_SIZE - 1));
        return size == 1 ? p[0] : size == 2 ? (uint32_t)(p[0] << 8 | p[1])
                                            : (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    }
    poll_watchdog(a);
    if ((a >> 24) != 0xCC) rt_fatal("read%d from unmapped address 0x%08X", size * 8, a);
    return 0;
}

void rt_slow_write(uint32_t a, uint32_t v, int size) {
    if ((a & 0xFFFFC000u) == 0xE0000000u) {
        uint8_t *p = g_lc + (a & (PPC_LC_SIZE - 1));
        for (int i = 0; i < size; i++) p[i] = (uint8_t)(v >> ((size - 1 - i) * 8));
        return;
    }
    if ((a >> 24) != 0xCC) rt_fatal("write%d 0x%08X to unmapped address 0x%08X", size * 8, v, a);
}

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
    int run_selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) run_selftest = 1;
        else if (!strncmp(argv[i], "--sdk-log=", 10)) sdk_log = argv[i] + 10;
        else if (!strncmp(argv[i], "--sdk-log-limit=", 16)) g_sdk_log_limit = strtoul(argv[i] + 16, NULL, 0);
        else root = argv[i];
    }
    g_mem = calloc(1, PPC_MEM_SIZE);
    g_lc = calloc(1, PPC_LC_SIZE);
    if (!g_mem || !g_lc) rt_fatal("out of memory");
    rt_boot_setup(root);
    printf("[recomp] %u guest functions linked\n", g_func_count);
    if (run_selftest) return selftest();

    if (*sdk_log && (g_sdk_log = fopen(sdk_log, "w"))) g_trace_sdk = 1;
    printf("[recomp] booting: __start at 0x%08X (SDK call log: %s)\n", G___start, *sdk_log ? sdk_log : "off");
    F___start(&g_ctx);
    printf("[recomp] __start returned\n");
    if (g_sdk_log) fclose(g_sdk_log);
    return 0;
}

/* runtime.c - minimal host runtime for the recompiled game.
 *
 * Milestone 1 scope: guest memory, the indirect-call dispatcher, diagnostics for the generated
 * code's runtime hooks, and loading main.dol into guest RAM. Booting the game (entry point,
 * SDK reimplementations, REL loading) comes in later milestones.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include "ppc.h"

typedef struct { uint32_t addr; GuestFn fn; const char *name; } FuncEntry;
extern const unsigned g_func_count;
extern const FuncEntry g_funcs[];

uint8_t *g_mem;
uint8_t *g_lc;

static void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[recomp] fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ---- function lookup ---------------------------------------------------------------------------- */

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

void rt_call(Ctx *c, uint32_t addr) {
    const FuncEntry *e = find_func(addr);
    if (!e) fatal("indirect call to unknown address 0x%08X (lr=0x%08X)", addr, c->lr);
    e->fn(c);
}

void rt_jump(Ctx *c, uint32_t addr) {
    const FuncEntry *e = find_func(addr);
    if (!e) fatal("jump to an address that is not a function start: 0x%08X", addr);
    e->fn(c);
}

/* ---- hooks used by generated code ------------------------------------------------------------- */

void rt_trap(Ctx *c, uint32_t pc) { (void)c; fatal("trap at 0x%08X", pc); }
void rt_illegal(Ctx *c, uint32_t pc, uint32_t insn) { (void)c; fatal("illegal instruction 0x%08X at 0x%08X", insn, pc); }
void rt_syscall(Ctx *c, uint32_t pc) { (void)c; fatal("sc at 0x%08X", pc); }

void rt_hle_unimplemented(Ctx *c, uint32_t addr, const char *name) {
    fprintf(stderr, "[recomp] HLE not implemented: %s (0x%08X), called from 0x%08X\n", name, addr, c->lr);
    c->r[3] = 0; /* return 0 so the caller can keep going while bringing things up */
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

/* ---- slow memory paths ------------------------------------------------------------------------ */

uint32_t rt_slow_read(uint32_t a, int size) {
    if ((a & 0xFFFFC000u) == 0xE0000000u) {
        uint8_t *p = g_lc + (a & (PPC_LC_SIZE - 1));
        return size == 1 ? p[0] : size == 2 ? (uint32_t)(p[0] << 8 | p[1])
                                            : (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    }
    fprintf(stderr, "[recomp] unhandled read%d at 0x%08X\n", size * 8, a);
    return 0;
}

void rt_slow_write(uint32_t a, uint32_t v, int size) {
    if ((a & 0xFFFFC000u) == 0xE0000000u) {
        uint8_t *p = g_lc + (a & (PPC_LC_SIZE - 1));
        for (int i = 0; i < size; i++) p[i] = (uint8_t)(v >> ((size - 1 - i) * 8));
        return;
    }
    fprintf(stderr, "[recomp] unhandled write%d 0x%08X at 0x%08X\n", size * 8, v, a);
}

/* ---- DOL loading ------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void load_dol(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *dol = malloc((size_t)size);
    if (!dol || fread(dol, 1, (size_t)size, f) != (size_t)size) fatal("cannot read %s", path);
    fclose(f);
    int sections = 0;
    for (int i = 0; i < 18; i++) {
        uint32_t off = be32(dol + i * 4), addr = be32(dol + 0x48 + i * 4), len = be32(dol + 0x90 + i * 4);
        if (!len) continue;
        if (!ppc_is_ram(addr) || off + len > (uint32_t)size) fatal("bad DOL section %d", i);
        memcpy(ppc_ram(addr), dol + off, len);
        sections++;
    }
    uint32_t bss = be32(dol + 0xD8), bss_len = be32(dol + 0xDC);
    printf("[recomp] loaded %s: %d sections, bss 0x%08X+0x%X, entry 0x%08X\n",
           path, sections, bss, bss_len, be32(dol + 0xE0));
    free(dol);
}

/* ---- self-test: run recompiled library code and compare with the host C library ----------------- */

static GuestFn func_by_name(const char *name) {
    for (unsigned i = 0; i < g_func_count; i++)
        if (!strcmp(g_funcs[i].name, name)) return g_funcs[i].fn;
    fatal("no guest function named %s", name);
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
    static Ctx c;
    char got[256], want[256];
    int pass = 0, total = 0;
    const uint32_t buf = 0x80500000u, fmt = 0x80500200u, str = 0x80500300u;

    printf("[recomp] self-test (guest code from main.dol)\n");
    func_by_name("__init_registers")(&c);
    snprintf(got, sizeof got, "r1=%08X r2=%08X r13=%08X", c.r[1], c.r[2], c.r[13]);
    total++, pass += check("__init_registers sets r1/r2/r13", c.r[1] && c.r[2] && c.r[13], got, NULL);

    c.r[3] = buf; c.r[4] = 0x5A; c.r[5] = 64; c.lr = 0;
    func_by_name("memset")(&c);
    int ok = c.r[3] == buf && rd8(buf) == 0x5A && rd8(buf + 63) == 0x5A && rd8(buf + 64) == 0;
    total++, pass += check("memset(buf, 0x5A, 64)", ok, NULL, NULL);

    put_str(str, "Star Fox: Assault");
    c.r[3] = str;
    func_by_name("strlen")(&c);
    snprintf(got, sizeof got, "%u", c.r[3]);
    total++, pass += check("strlen(\"Star Fox: Assault\")", c.r[3] == 17, got, "17");

    put_str(fmt, "%s|%d|%5d|%-4d|%08X|%c|%u");
    c.r[3] = buf; c.r[4] = fmt; c.r[5] = str; c.r[6] = 2005; c.r[7] = 42; c.r[8] = (uint32_t)-7;
    c.r[9] = 0xDEADBEEF; c.r[10] = 'Z';
    /* 8th variadic argument goes on the stack (EABI parameter save area at r1+8) */
    c.r[1] -= 64; wr32(c.r[1] + 8, 4000000000u);
    cr_setbit(&c, 6, 0); /* no floating-point arguments */
    func_by_name("sprintf")(&c);
    c.r[1] += 64;
    get_str(buf, got, sizeof got);
    snprintf(want, sizeof want, "%s|%d|%5d|%-4d|%08X|%c|%u", "Star Fox: Assault", 2005, 42, -7, 0xDEADBEEFu, 'Z', 4000000000u);
    total++, pass += check("sprintf integers and strings", !strcmp(got, want), got, want);

    put_str(fmt, "%.3f|%8.2f|%e|%g");
    c.r[3] = buf; c.r[4] = fmt;
    c.f[1].ps0 = 3.14159265; c.f[2].ps0 = -1234.5; c.f[3].ps0 = 6.02e23; c.f[4].ps0 = 0.0001;
    cr_setbit(&c, 6, 1); /* floating-point arguments in f1-f8 */
    func_by_name("sprintf")(&c);
    get_str(buf, got, sizeof got);
    snprintf(want, sizeof want, "%.3f|%8.2f|%e|%g", 3.14159265, -1234.5, 6.02e23, 0.0001);
    total++, pass += check("sprintf floating point", !strcmp(got, want), got, want);

    printf("[recomp] self-test: %d/%d passed\n", pass, total);
    return pass == total ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *dol = "orig/GF7E01/sys/main.dol";
    int run_selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) run_selftest = 1;
        else dol = argv[i];
    }
    g_mem = calloc(1, PPC_MEM_SIZE);
    g_lc = calloc(1, PPC_LC_SIZE);
    if (!g_mem || !g_lc) fatal("out of memory");
    load_dol(dol);
    printf("[recomp] %u guest functions linked\n", g_func_count);
    if (run_selftest) return selftest();
    printf("[recomp] milestone 1 build: booting the game is not implemented yet (try --selftest)\n");
    return 0;
}

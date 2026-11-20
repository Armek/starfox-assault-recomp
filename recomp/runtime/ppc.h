/* ppc.h - CPU state and helpers used by recompiled Gekko (PowerPC 750CL) code.
 *
 * Every recompiled function has the signature `void f_XXXXXXXX(Ctx *c)`, where XXXXXXXX is its
 * original address. Guest memory is big-endian; all loads and stores go through the helpers
 * below, which byte-swap and route special address ranges (hardware registers, locked cache).
 */
#ifndef PPC_H
#define PPC_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define PPC_INLINE static __forceinline
#define PPC_UNLIKELY(x) (x)
#define bswap16(x) _byteswap_ushort(x)
#define bswap32(x) _byteswap_ulong(x)
#define bswap64(x) _byteswap_uint64(x)
#else
#define PPC_INLINE static inline __attribute__((always_inline))
#define PPC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define bswap16(x) __builtin_bswap16(x)
#define bswap32(x) __builtin_bswap32(x)
#define bswap64(x) __builtin_bswap64(x)
#endif

/* ---- CPU state ---------------------------------------------------------------------------- */

typedef struct {
    double ps0; /* the FPR value; paired-single slot 0 */
    double ps1; /* paired-single slot 1 */
} Fpr;

typedef struct Ctx {
    uint32_t r[32];
    Fpr f[32];
    uint32_t cr;
    uint32_t lr, ctr;
    uint32_t xer_ca, xer_so;
    uint32_t fpscr;
    uint32_t msr;
    uint32_t gqr[8];
    uint32_t spr[1024]; /* everything else written by mtspr */
} Ctx;

typedef void (*GuestFn)(Ctx *c);

/* ---- Guest memory --------------------------------------------------------------------------- */

#define PPC_MEM_SIZE 0x01800000u /* 24 MB main RAM */
#define PPC_LC_SIZE 0x4000u      /* 16 KB locked L1 cache at 0xE0000000 */

/* Guest RAM (PPC_MEM_SIZE bytes, mirrored at 0x80000000 and 0xC0000000) lives at a fixed host address,
 * reserved at start (rt_mem_init). Its base is then a constant in every access: with a global pointer the
 * compiler reloaded it after each guest store (a byte store may alias anything), one extra dependent load
 * on every guest memory access, which are most of the game's CPU time. */
#define PPC_MEM_HOST 0x200000000000ull
#define g_mem ((uint8_t *)(uintptr_t)PPC_MEM_HOST)
extern int g_mem_ready; /* set once guest RAM is mapped */
extern uint8_t *g_lc;

/* Slow paths, implemented by the runtime: hardware registers (0xCC000000, including the GX
 * write-gather pipe at 0xCC008000), locked cache and invalid addresses. */
uint32_t rt_slow_read(uint32_t addr, int size);
void rt_slow_write(uint32_t addr, uint32_t value, int size);

/* 0x80000000-0x817FFFFF and 0xC0000000-0xC17FFFFF are plain RAM. */
PPC_INLINE int ppc_is_ram(uint32_t a) {
    return ((a & 0xBE000000u) == 0x80000000u) && ((a & 0x01FFFFFFu) < PPC_MEM_SIZE);
}
PPC_INLINE uint8_t *ppc_ram(uint32_t a) { return g_mem + (a & 0x01FFFFFFu); }

PPC_INLINE uint8_t rd8(uint32_t a) {
    if (PPC_UNLIKELY(!ppc_is_ram(a))) return (uint8_t)rt_slow_read(a, 1);
    return *ppc_ram(a);
}
PPC_INLINE uint16_t rd16(uint32_t a) {
    uint16_t v;
    if (PPC_UNLIKELY(!ppc_is_ram(a))) return (uint16_t)rt_slow_read(a, 2);
    memcpy(&v, ppc_ram(a), 2);
    return bswap16(v);
}
PPC_INLINE uint32_t rd32(uint32_t a) {
    uint32_t v;
    if (PPC_UNLIKELY(!ppc_is_ram(a))) return rt_slow_read(a, 4);
    memcpy(&v, ppc_ram(a), 4);
    return bswap32(v);
}
PPC_INLINE uint64_t rd64(uint32_t a) { return ((uint64_t)rd32(a) << 32) | rd32(a + 4); }

/* Debug builds (-DRT_WATCH): report writes to the range set by SFA_WATCH=<hex addr>:<hex len>. */
#ifdef RT_WATCH
extern uint32_t g_watch_lo, g_watch_len;
void rt_watch_hit(uint32_t addr, uint32_t value, int size);
#define RT_WATCH_CHECK(a, v, n) do { if (PPC_UNLIKELY(((a) & 0x01FFFFFFu) - g_watch_lo < g_watch_len)) rt_watch_hit((a), (v), (n)); } while (0)
#else
#define RT_WATCH_CHECK(a, v, n) ((void)0)
#endif

PPC_INLINE void wr8(uint32_t a, uint8_t v) {
    if (PPC_UNLIKELY(!ppc_is_ram(a))) { rt_slow_write(a, v, 1); return; }
    RT_WATCH_CHECK(a, v, 1);
    *ppc_ram(a) = v;
}
PPC_INLINE void wr16(uint32_t a, uint16_t v) {
    if (PPC_UNLIKELY(!ppc_is_ram(a))) { rt_slow_write(a, v, 2); return; }
    RT_WATCH_CHECK(a, v, 2);
    v = bswap16(v);
    memcpy(ppc_ram(a), &v, 2);
}
PPC_INLINE void wr32(uint32_t a, uint32_t v) {
    if (PPC_UNLIKELY(!ppc_is_ram(a))) { rt_slow_write(a, v, 4); return; }
    RT_WATCH_CHECK(a, v, 4);
    v = bswap32(v);
    memcpy(ppc_ram(a), &v, 4);
}
PPC_INLINE void wr64(uint32_t a, uint64_t v) {
    wr32(a, (uint32_t)(v >> 32));
    wr32(a + 4, (uint32_t)v);
}

PPC_INLINE float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
PPC_INLINE uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
PPC_INLINE double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
PPC_INLINE uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* ---- Integer helpers ------------------------------------------------------------------------- */

PPC_INLINE uint32_t rotl32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
PPC_INLINE uint32_t cntlzw(uint32_t v) {
    uint32_t n = 0;
    if (!v) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}
PPC_INLINE uint32_t ppc_divw(uint32_t a, uint32_t b) {
    if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) return ((int32_t)a < 0 && b == 0) ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
PPC_INLINE uint32_t ppc_divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }
PPC_INLINE uint32_t ppc_sraw(Ctx *c, uint32_t s, uint32_t n) {
    n &= 0x3F;
    if (n >= 32) { c->xer_ca = (int32_t)s < 0; return (int32_t)s < 0 ? 0xFFFFFFFFu : 0; }
    c->xer_ca = ((int32_t)s < 0) && n && (s & ((1u << n) - 1));
    return (uint32_t)((int32_t)s >> n);
}

/* ---- Condition register ---------------------------------------------------------------------- */

PPC_INLINE void cr_set(Ctx *c, int field, uint32_t v) {
    int sh = 28 - field * 4;
    c->cr = (c->cr & ~(0xFu << sh)) | ((v & 0xF) << sh);
}
PPC_INLINE uint32_t cr_bit(Ctx *c, int bit) { return (c->cr >> (31 - bit)) & 1; }
PPC_INLINE void cr_setbit(Ctx *c, int bit, uint32_t v) {
    c->cr = (c->cr & ~(1u << (31 - bit))) | ((v & 1) << (31 - bit));
}
PPC_INLINE void cmp_s(Ctx *c, int field, int32_t a, int32_t b) {
    cr_set(c, field, (a < b ? 8 : a > b ? 4 : 2) | (c->xer_so & 1));
}
PPC_INLINE void cmp_u(Ctx *c, int field, uint32_t a, uint32_t b) {
    cr_set(c, field, (a < b ? 8 : a > b ? 4 : 2) | (c->xer_so & 1));
}
PPC_INLINE void cmp_f(Ctx *c, int field, double a, double b) {
    uint32_t v = (a != a || b != b) ? 1 : a < b ? 8 : a > b ? 4 : 2;
    cr_set(c, field, v);
    c->fpscr = (c->fpscr & ~0xF000u) | (v << 12);
}
#define RC0(c, v) cmp_s((c), 0, (int32_t)(v), 0)

/* ---- Floating point -------------------------------------------------------------------------- */

PPC_INLINE double rs(double v) { return (double)(float)v; } /* round to single */
PPC_INLINE void set_single(Ctx *c, int d, double v) { c->f[d].ps0 = c->f[d].ps1 = rs(v); }
PPC_INLINE void set_fpr_bits(Ctx *c, int d, uint64_t bits) { c->f[d].ps0 = u2d(bits); }
PPC_INLINE uint64_t ppc_fctiw(double v, int trunc) {
    int32_t r;
    if (v != v) r = (int32_t)0x80000000;
    else if (v >= 2147483647.0) r = 0x7FFFFFFF;
    else if (v <= -2147483648.0) r = (int32_t)0x80000000;
    else r = trunc ? (int32_t)v : (int32_t)nearbyint(v);
    return 0xFFF8000000000000ull | (uint32_t)r;
}
PPC_INLINE double ppc_fres(double v) { return rs(1.0 / v); }
PPC_INLINE double ppc_frsqrte(double v) { return 1.0 / sqrt(v); }

/* 2^e for |e| <= 32, built from its exponent bits: exactly ldexp(1.0, e), without the library call (which
 * was 6-7% of the game's CPU time, paid on every quantized paired-single load and store). */
PPC_INLINE double pow2i(int e) { return u2d((uint64_t)(1023 + e) << 52); }

/* Paired-single quantized loads and stores (GQR types: 0 float, 4 u8, 5 u16, 6 s8, 7 s16). */
PPC_INLINE double psq_dequant(uint32_t a, uint32_t type, uint32_t scale, uint32_t *size) {
    double s = pow2i(-(int)((int32_t)(scale << 26) >> 26));
    switch (type) {
    case 4: *size = 1; return rd8(a) * s;
    case 5: *size = 2; return rd16(a) * s;
    case 6: *size = 1; return (int8_t)rd8(a) * s;
    case 7: *size = 2; return (int16_t)rd16(a) * s;
    default: *size = 4; return u2f(rd32(a));
    }
}
PPC_INLINE void psq_l(Ctx *c, int d, uint32_t a, int w, int i) {
    uint32_t g = c->gqr[i], type = (g >> 16) & 7, scale = (g >> 24) & 0x3F, sz;
    c->f[d].ps0 = psq_dequant(a, type, scale, &sz);
    c->f[d].ps1 = w ? 1.0 : psq_dequant(a + sz, type, scale, &sz);
}
PPC_INLINE double psq_clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
PPC_INLINE uint32_t psq_quant(uint32_t a, double v, uint32_t type, uint32_t scale) {
    double s = pow2i((int)((int32_t)(scale << 26) >> 26));
    switch (type) {
    case 4: wr8(a, (uint8_t)psq_clamp(v * s, 0, 255)); return 1;
    case 5: wr16(a, (uint16_t)psq_clamp(v * s, 0, 65535)); return 2;
    case 6: wr8(a, (uint8_t)(int8_t)psq_clamp(v * s, -128, 127)); return 1;
    case 7: wr16(a, (uint16_t)(int16_t)psq_clamp(v * s, -32768, 32767)); return 2;
    default: wr32(a, f2u((float)v)); return 4;
    }
}
PPC_INLINE void psq_st(Ctx *c, int s, uint32_t a, int w, int i) {
    uint32_t g = c->gqr[i], type = g & 7, scale = (g >> 8) & 0x3F;
    uint32_t sz = psq_quant(a, c->f[s].ps0, type, scale);
    if (!w) psq_quant(a + sz, c->f[s].ps1, type, scale);
}

/* ---- Function-entry tracing --------------------------------------------------------------------- */

#define RT_TRACE_RING 256
extern uint32_t g_trace_ring[RT_TRACE_RING]; /* last functions entered, for crash reports */
extern uint32_t g_trace_pos;
extern int g_trace_sdk;                      /* log every Dolphin SDK call */
void rt_sdk_enter(Ctx *c, uint32_t addr);

/* Loop back-edges call RT_SAFEPOINT so the runtime can deliver interrupts to spinning code. */
extern int g_safepoint_budget;
void rt_safepoint(Ctx *c);
#define RT_SAFEPOINT(c) do { if (PPC_UNLIKELY(--g_safepoint_budget <= 0)) rt_safepoint(c); } while (0)

/* SFA_COUNT_CALLS (testing): calls to functions in [g_count_lo, g_count_hi) are counted; empty otherwise */
extern uint32_t g_count_lo, g_count_hi;
void rt_count_call(Ctx *c, uint32_t addr);
#define RT_ENTER(c, a)                                                                       \
    ((void)(g_trace_ring[g_trace_pos++ & (RT_TRACE_RING - 1)] = (a)),                       \
     PPC_UNLIKELY((uint32_t)(a) - g_count_lo < g_count_hi - g_count_lo) ? rt_count_call((c), (a)) : (void)0)
#define RT_ENTER_SDK(c, a) do { RT_ENTER(c, a); if (g_trace_sdk) rt_sdk_enter((c), (a)); } while (0)

/* ---- Runtime services used by generated code ---------------------------------------------------- */

void rt_call(Ctx *c, uint32_t addr);                       /* indirect call (bctrl, blrl) */
void rt_jump(Ctx *c, uint32_t addr);                       /* tail jump to a non-function address */
void rt_trap(Ctx *c, uint32_t pc);                         /* tw/twi taken */
void rt_illegal(Ctx *c, uint32_t pc, uint32_t insn);       /* undecodable instruction */
void rt_syscall(Ctx *c, uint32_t pc);                      /* sc */
void rt_hle_unimplemented(Ctx *c, uint32_t addr, const char *name);
uint64_t rt_timebase(void);
void rt_dcbz(uint32_t addr);
void rt_lswi(Ctx *c, uint32_t ea, int rd, int nb);
void rt_stswi(Ctx *c, uint32_t ea, int rs, int nb);
void rt_icache_invalidate(uint32_t addr);
void rt_set_dec(Ctx *c, uint32_t value);           /* mtdec */
uint32_t rt_get_dec(Ctx *c);                       /* mfdec */

#endif /* PPC_H */

/* gx.c - GX command processor: decodes the stream the game writes to the write-gather pipe at
 * 0xCC008000 (and the display lists it calls), tracks CP/XF/BP state, transforms vertices on
 * the CPU and builds per-frame draw lists for the renderer (video.c).
 *
 * Coverage so far: all register loads, indexed XF loads, display lists, every primitive type,
 * every vertex attribute format, position/texture matrices, perspective/orthographic projection,
 * the viewport, all texture formats (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2,
 * CMPR), TLUT loads, blending, depth, scissor, alpha test and EFB copies to the XFB.
 * Not yet: lighting, multi-stage TEV (stage 0 is approximated), EFB copies to textures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "funcs.h"
#include "runtime.h"
#include "gx.h"

/* ---- state ------------------------------------------------------------------------------------- */
static uint32_t cp[256];
static uint32_t xf[0x1100]; /* 0x0000-0x0FFF memory (matrices, lights), 0x1000-0x10FF registers */
static uint32_t bp[256];
static uint32_t bp_mask = 0xFFFFFF;
static uint8_t tmem_tlut[0x80000]; /* palettes, by TLUT tmem offset */

static GxFrame *g_frame;
static uint32_t g_frame_number;
static unsigned long g_stat_draws, g_stat_unknown;

/* Debug switches, read once (getenv is slow and these are checked on hot paths). */
static int g_dbg = -1;           /* SFA_GX_DEBUG */
static uint32_t g_dbg_texaddr;   /* SFA_GX_TEXADDR, 0 = off */
static void dbg_init(void) {
    if (g_dbg >= 0) return;
    g_dbg = getenv("SFA_GX_DEBUG") != NULL;
    const char *t = getenv("SFA_GX_TEXADDR");
    g_dbg_texaddr = t ? (uint32_t)strtoul(t, NULL, 16) : 0;
}

static float xff(uint32_t addr) { float f; memcpy(&f, &xf[addr], 4); return f; }

/* ---- frame building ------------------------------------------------------------------------ */
static GxFrame *frame(void) {
    if (!g_frame) {
        g_frame = calloc(1, sizeof *g_frame);
        g_frame->number = g_frame_number;
    }
    return g_frame;
}

#define GROW(arr, n, cap, init)                                          \
    do {                                                                 \
        if ((n) == (cap)) {                                              \
            (cap) = (cap) ? (cap) * 2 : (init);                          \
            (arr) = realloc((arr), sizeof *(arr) * (cap));               \
        }                                                                \
    } while (0)

/* ---- textures ---------------------------------------------------------------------------------- */
typedef struct {
    uint32_t addr, fmt, w, h, tlut_hash, content_hash, id, checked_frame;
} TexEntry;
#define TEX_CACHE 4096
static TexEntry g_tex[TEX_CACHE];
static uint32_t g_next_tex_id = 1;

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void tex_tile(uint32_t fmt, uint32_t *tw, uint32_t *th, uint32_t *bytes) {
    switch (fmt) {
    case 0: case 8: *tw = 8; *th = 8; *bytes = 32; break;  /* I4, CI4 */
    case 1: case 2: case 9: *tw = 8; *th = 4; *bytes = 32; break;  /* I8, IA4, CI8 */
    case 6: *tw = 4; *th = 4; *bytes = 64; break;  /* RGBA8 */
    case 14: *tw = 8; *th = 8; *bytes = 32; break; /* CMPR */
    default: *tw = 4; *th = 4; *bytes = 32; break; /* IA8, RGB565, RGB5A3, CI14X2 */
    }
}

static uint32_t tex_size(uint32_t fmt, uint32_t w, uint32_t h) {
    uint32_t tw, th, tb;
    tex_tile(fmt, &tw, &th, &tb);
    return ((w + tw - 1) / tw) * ((h + th - 1) / th) * tb;
}

static void rgb565(uint16_t v, uint8_t *o) {
    o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((v & 31) * 255 / 31);
    o[3] = 255;
}
static void rgb5a3(uint16_t v, uint8_t *o) {
    if (v & 0x8000) {
        o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
        o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
        o[2] = (uint8_t)((v & 31) * 255 / 31);
        o[3] = 255;
    } else {
        o[0] = (uint8_t)(((v >> 8) & 15) * 17);
        o[1] = (uint8_t)(((v >> 4) & 15) * 17);
        o[2] = (uint8_t)((v & 15) * 17);
        o[3] = (uint8_t)(((v >> 12) & 7) * 255 / 7);
    }
}
static void palette(uint32_t tlut_fmt, const uint8_t *pal, uint32_t idx, uint8_t *o) {
    uint16_t v = (uint16_t)(pal[idx * 2] << 8 | pal[idx * 2 + 1]);
    if (tlut_fmt == 0) { o[0] = o[1] = o[2] = (uint8_t)v; o[3] = (uint8_t)(v >> 8); } /* IA8 */
    else if (tlut_fmt == 1) rgb565(v, o);
    else rgb5a3(v, o);
}

/* Decode a texture from guest memory into RGBA8 (top row first). */
static uint8_t *decode_texture(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *pal, uint32_t tlut_fmt) {
    uint8_t *out = calloc((size_t)w * h, 4);
    uint32_t tw, th, tb;
    tex_tile(fmt, &tw, &th, &tb);
    uint32_t src = addr;
    for (uint32_t ty = 0; ty < h; ty += th) {
        for (uint32_t tx = 0; tx < w; tx += tw, src += tb) {
            uint8_t blk[64];
            for (uint32_t i = 0; i < tb; i++) blk[i] = rd8(src + i);
            for (uint32_t y = 0; y < th; y++) {
                for (uint32_t x = 0; x < tw; x++) {
                    uint32_t px = tx + x, py = ty + y;
                    uint8_t c[4] = {0, 0, 0, 0};
                    uint32_t i = y * tw + x;
                    switch (fmt) {
                    case 0: { uint8_t v = (uint8_t)(((blk[i / 2] >> ((i & 1) ? 0 : 4)) & 15) * 17); c[0] = c[1] = c[2] = c[3] = v; break; }
                    case 1: c[0] = c[1] = c[2] = c[3] = blk[i]; break;
                    case 2: c[0] = c[1] = c[2] = (uint8_t)((blk[i] & 15) * 17); c[3] = (uint8_t)((blk[i] >> 4) * 17); break;
                    case 3: c[0] = c[1] = c[2] = blk[i * 2 + 1]; c[3] = blk[i * 2]; break;
                    case 4: rgb565((uint16_t)(blk[i * 2] << 8 | blk[i * 2 + 1]), c); break;
                    case 5: rgb5a3((uint16_t)(blk[i * 2] << 8 | blk[i * 2 + 1]), c); break;
                    case 6: c[3] = blk[i * 2]; c[0] = blk[i * 2 + 1]; c[1] = blk[32 + i * 2]; c[2] = blk[32 + i * 2 + 1]; break;
                    case 8: palette(tlut_fmt, pal, (blk[i / 2] >> ((i & 1) ? 0 : 4)) & 15, c); break;
                    case 9: palette(tlut_fmt, pal, blk[i], c); break;
                    case 10: palette(tlut_fmt, pal, (uint32_t)(blk[i * 2] << 8 | blk[i * 2 + 1]) & 0x3FFF, c); break;
                    case 14: {
                        uint32_t sub = (y / 4) * 2 + (x / 4); /* four 4x4 DXT1 blocks per 8x8 tile */
                        const uint8_t *b = blk + sub * 8;
                        uint16_t c0 = (uint16_t)(b[0] << 8 | b[1]), c1 = (uint16_t)(b[2] << 8 | b[3]);
                        uint8_t pal4[4][4];
                        rgb565(c0, pal4[0]);
                        rgb565(c1, pal4[1]);
                        for (int k = 0; k < 3; k++) {
                            if (c0 > c1) {
                                pal4[2][k] = (uint8_t)((2 * pal4[0][k] + pal4[1][k]) / 3);
                                pal4[3][k] = (uint8_t)((pal4[0][k] + 2 * pal4[1][k]) / 3);
                            } else {
                                pal4[2][k] = (uint8_t)((pal4[0][k] + pal4[1][k]) / 2);
                                pal4[3][k] = 0;
                            }
                        }
                        pal4[2][3] = 255;
                        pal4[3][3] = c0 > c1 ? 255 : 0;
                        uint32_t sel = (b[4 + (y & 3)] >> (6 - 2 * (x & 3))) & 3;
                        memcpy(c, pal4[sel], 4);
                        break;
                    }
                    default: c[0] = 255; c[1] = 0; c[2] = 255; c[3] = 255; break; /* unknown: magenta */
                    }
                    if (px < w && py < h) memcpy(out + ((size_t)py * w + px) * 4, c, 4);
                }
            }
        }
    }
    return out;
}

/* Texture map n -> GxTexture id (decoding and queueing an upload if new or changed). */
static uint32_t texture_for_map(int n) {
    uint32_t base_image0 = n < 4 ? 0x88 + n : 0xA8 + (n - 4);
    uint32_t base_image3 = n < 4 ? 0x94 + n : 0xB4 + (n - 4);
    uint32_t base_tlut = n < 4 ? 0x98 + n : 0xB8 + (n - 4);
    uint32_t img0 = bp[base_image0];
    uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    uint32_t addr = 0x80000000u | ((bp[base_image3] & 0xFFFFFF) << 5);
    if (g_dbg_texaddr && g_dbg_texaddr == addr) {
        static unsigned shown;
        if (shown++ < 6) {
            printf("[gx-texaddr] frame %u map %d fmt %u %ux%u bound by:", g_frame_number, n, fmt, w, h);
            for (uint32_t i = 12; i >= 1; i--) printf(" %s", rt_func_name(g_trace_ring[(g_trace_pos - i) & (RT_TRACE_RING - 1)]));
            printf("\n");
        }
    }
    if (g_dbg) {
        uint32_t img1 = bp[n < 4 ? 0x8C + n : 0xAC + (n - 4)];
        static unsigned n_pre;
        if (((img1 >> 21) & 1) && n_pre++ < 20)
            printf("[gx-preloaded] frame %u map %d uses TMEM %05X (fmt %u %ux%u, image addr %08X)\n", g_frame_number, n,
                   (img1 & 0x7FFF) << 5, fmt, w, h, addr);
    }
    if (!ppc_is_ram(addr)) return 0;
    uint32_t tlut = bp[base_tlut], tlut_fmt = (tlut >> 10) & 3;
    const uint8_t *pal = tmem_tlut + (((tlut & 0x3FF) << 9) & (sizeof tmem_tlut - 1));
    uint32_t tlut_hash = 0;
    if (fmt == 8 || fmt == 9 || fmt == 10) {
        uint32_t entries = fmt == 8 ? 16 : fmt == 9 ? 256 : 4096;
        size_t bytes = entries * 2;
        if ((size_t)(pal - tmem_tlut) + bytes > sizeof tmem_tlut) bytes = sizeof tmem_tlut - (size_t)(pal - tmem_tlut);
        tlut_hash = fnv(pal, bytes, 2166136261u) ^ (tlut_fmt << 30);
    }
    uint32_t key = (addr * 2654435761u) ^ (fmt << 24) ^ (w << 12) ^ h ^ tlut_hash;
    TexEntry *e = NULL;
    for (uint32_t probe = 0; probe < 16; probe++) {
        TexEntry *t = &g_tex[(key + probe) & (TEX_CACHE - 1)];
        if (t->id && t->addr == addr && t->fmt == fmt && t->w == w && t->h == h && t->tlut_hash == tlut_hash) { e = t; break; }
        if (!t->id) { e = t; break; }
    }
    if (!e) e = &g_tex[key & (TEX_CACHE - 1)]; /* evict */
    int fresh = !(e->id && e->addr == addr && e->fmt == fmt && e->w == w && e->h == h && e->tlut_hash == tlut_hash);
    uint32_t size = tex_size(fmt, w, h);
    /* contents are re-hashed at most once per frame */
    if (!fresh && e->checked_frame == g_frame_number) return e->id;
    uint32_t hash = fnv(ppc_ram(addr), (addr & 0x01FFFFFF) + size <= PPC_MEM_SIZE ? size : 0, 2166136261u);
    if (fresh) {
        e->addr = addr; e->fmt = fmt; e->w = w; e->h = h; e->tlut_hash = tlut_hash;
        e->id = g_next_tex_id++;
    } else if (hash == e->content_hash) {
        e->checked_frame = g_frame_number;
        return e->id;
    }
    e->content_hash = hash;
    e->checked_frame = g_frame_number;
    GxFrame *f = frame();
    GROW(f->uploads, f->nuploads, f->cap_uploads, 64);
    GxTextureUpload *u = &f->uploads[f->nuploads++];
    u->id = e->id; u->width = w; u->height = h;
    u->rgba = decode_texture(addr, fmt, w, h, pal, tlut_fmt);
    if (g_dbg) {
        uint32_t nz = 0;
        for (uint32_t q = 0; q < size && q < 4096; q++) nz += rd8(addr + q) != 0;
        printf("[gx-tex] frame %u id %u addr %08X fmt %u %ux%u tlut %03X/%u nonzero %u/%u\n", g_frame_number, e->id, addr,
               fmt, w, h, tlut & 0x3FF, tlut_fmt, nz, size < 4096 ? size : 4096);
        char path[256];
        snprintf(path, sizeof path, "build/GF7E01/textures/tex_%04u_%08X_f%u_%ux%u.bmp", e->id, addr, fmt, w, h);
        FILE *fp = fopen(path, "wb");
        if (fp) { /* 32-bit BMP, top-down */
            uint32_t sz = w * h * 4, v;
            uint8_t hdr[54] = {'B', 'M'};
            v = 54 + sz; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4);
            v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &w, 4);
            int32_t nh = -(int32_t)h; memcpy(hdr + 22, &nh, 4);
            hdr[26] = 1; hdr[28] = 32;
            fwrite(hdr, 1, 54, fp);
            for (uint32_t i = 0; i < w * h; i++) {
                uint8_t *px = u->rgba + i * 4, bgra[4] = {px[2], px[1], px[0], px[3]};
                fwrite(bgra, 1, 4, fp);
            }
            fclose(fp);
        }
    }
    return e->id;
}

/* ---- render state ------------------------------------------------------------------------------ */
/* TEV registers and konst colors share BP 0xE0-0xE7 (bit 23 selects which); RA then BG per index. */
static float g_tevreg[4][4], g_konst[4][4];

static void tev_color_write(uint32_t reg, uint32_t val) {
    int idx = (int)(reg - 0xE0) >> 1, bg = (reg - 0xE0) & 1, konst = (val >> 23) & 1;
    float (*dst)[4] = konst ? g_konst : g_tevreg;
    float lo, hi;
    if (konst) { lo = (val & 0xFF) / 255.f; hi = ((val >> 12) & 0xFF) / 255.f; }
    else { lo = ((int32_t)(val << 21) >> 21) / 255.f; hi = ((int32_t)((val >> 12) << 21) >> 21) / 255.f; }
    if (!bg) { dst[idx][0] = lo; dst[idx][3] = hi; } /* RA: red, alpha */
    else { dst[idx][2] = lo; dst[idx][1] = hi; }     /* BG: blue, green */
}

static void current_state(GxState *s) {
    memset(s, 0, sizeof *s);
    GxTevConfig *t = &s->tev;
    t->nstages = (uint8_t)(((bp[0x00] >> 10) & 15) + 1);
    for (int i = 0; i < t->nstages; i++) {
        t->color_env[i] = bp[0xC0 + 2 * i] & 0xFFFFFF;
        t->alpha_env[i] = bp[0xC1 + 2 * i] & 0xFFFFFF;
    }
    for (int i = 0; i < (t->nstages + 1) / 2; i++) t->tref[i] = bp[0x28 + i] & 0xFFFFFF;
    for (int i = 0; i < 8; i++) t->ksel[i] = bp[0xF6 + i] & 0xFFFFFF;
    t->alpha_cmp = bp[0xF3] & 0xFF0000;
    s->alpha_ref[0] = (bp[0xF3] & 0xFF) / 255.f;
    s->alpha_ref[1] = ((bp[0xF3] >> 8) & 0xFF) / 255.f;
    memcpy(s->konst, g_konst, sizeof s->konst);
    memcpy(s->reg, g_tevreg, sizeof s->reg);
    /* textures referenced by enabled stages */
    for (int i = 0; i < t->nstages; i++) {
        uint32_t tr = (t->tref[i / 2] >> ((i & 1) * 12)) & 0xFFF;
        int map = tr & 7;
        if (!((tr >> 6) & 1) || s->texture[map]) continue;
        s->texture[map] = texture_for_map(map);
        uint32_t mode0 = bp[map < 4 ? 0x80 + map : 0xA0 + (map - 4)];
        s->wrap_s[map] = mode0 & 3;
        s->wrap_t[map] = (mode0 >> 2) & 3;
        s->linear[map] = (mode0 >> 4) & 1;
    }
    uint32_t bm = bp[0x41];
    s->blend_enable = bm & 1;
    s->color_update = (bm >> 3) & 1;
    s->alpha_update = (bm >> 4) & 1;
    s->blend_dst = (bm >> 5) & 7;
    s->blend_src = (bm >> 8) & 7;
    s->blend_sub = (bm >> 11) & 1;
    uint32_t zm = bp[0x40];
    s->z_enable = zm & 1;
    s->z_func = (zm >> 1) & 7;
    s->z_update = (zm >> 4) & 1;
    s->cull = 0; /* culling disabled during bring-up */
    uint32_t tl = bp[0x20], br = bp[0x21];
    int x0 = (int)((tl >> 12) & 0x7FF) - 342, y0 = (int)(tl & 0x7FF) - 342;
    int x1 = (int)((br >> 12) & 0x7FF) - 342, y1 = (int)(br & 0x7FF) - 342;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EFB_WIDTH - 1) x1 = EFB_WIDTH - 1;
    if (y1 > EFB_HEIGHT - 1) y1 = EFB_HEIGHT - 1;
    if (x1 < x0 || y1 < y0) { x0 = y0 = 0; x1 = EFB_WIDTH - 1; y1 = EFB_HEIGHT - 1; }
    s->scissor[0] = (uint16_t)x0; s->scissor[1] = (uint16_t)y0; s->scissor[2] = (uint16_t)x1; s->scissor[3] = (uint16_t)y1;
}

/* ---- vertex decoding --------------------------------------------------------------------------- */
typedef struct {
    float pos[3];
    float col[2][4];
    float tex[8][2];
    int pnmtx, texmtx[8];
    int has_col[2];
} RawVertex;

/* Inline vertex data that arrived through the write-gather pipe lives in a host buffer; it is
 * addressed through a fake window at STAGE_BASE so the readers below work for both. */
#define STAGE_BASE 0x70000000u
static const uint8_t *g_stage;
static uint8_t vrd8(uint32_t a) {
    if (a >= STAGE_BASE && a < 0x80000000u) return g_stage[a - STAGE_BASE];
    return rd8(a);
}
static uint16_t vrd16(uint32_t a) { return (uint16_t)(vrd8(a) << 8 | vrd8(a + 1)); }
static uint32_t vrd32(uint32_t a) { return (uint32_t)vrd16(a) << 16 | vrd16(a + 2); }
#define rd8 vrd8
#define rd16 vrd16
#define rd32 vrd32

static float read_comp(uint32_t *a, int fmt, int shift) {
    float v;
    switch (fmt) {
    case 0: v = rd8(*a); *a += 1; break;
    case 1: v = (int8_t)rd8(*a); *a += 1; break;
    case 2: v = rd16(*a); *a += 2; break;
    case 3: v = (int16_t)rd16(*a); *a += 2; break;
    default: v = u2f(rd32(*a)); *a += 4; return v;
    }
    return v / (float)(1 << shift);
}

static void read_color(uint32_t *a, int fmt, float *c) {
    uint32_t v;
    switch (fmt) {
    case 0: v = rd16(*a); *a += 2; c[0] = ((v >> 11) & 31) / 31.f; c[1] = ((v >> 5) & 63) / 63.f; c[2] = (v & 31) / 31.f; c[3] = 1; break;
    case 1: c[0] = rd8(*a) / 255.f; c[1] = rd8(*a + 1) / 255.f; c[2] = rd8(*a + 2) / 255.f; c[3] = 1; *a += 3; break;
    case 2: c[0] = rd8(*a) / 255.f; c[1] = rd8(*a + 1) / 255.f; c[2] = rd8(*a + 2) / 255.f; c[3] = 1; *a += 4; break;
    case 3: v = rd16(*a); *a += 2; c[0] = ((v >> 12) & 15) / 15.f; c[1] = ((v >> 8) & 15) / 15.f; c[2] = ((v >> 4) & 15) / 15.f; c[3] = (v & 15) / 15.f; break;
    case 4: v = (uint32_t)rd8(*a) << 16 | rd8(*a + 1) << 8 | rd8(*a + 2); *a += 3;
        c[0] = ((v >> 18) & 63) / 63.f; c[1] = ((v >> 12) & 63) / 63.f; c[2] = ((v >> 6) & 63) / 63.f; c[3] = (v & 63) / 63.f; break;
    default: c[0] = rd8(*a) / 255.f; c[1] = rd8(*a + 1) / 255.f; c[2] = rd8(*a + 2) / 255.f; c[3] = rd8(*a + 3) / 255.f; *a += 4; break;
    }
}

static const int k_comp_size[8] = {1, 1, 2, 2, 4, 4, 4, 4};
static const int k_color_size[8] = {2, 3, 4, 2, 3, 4, 4, 4};

typedef struct {
    int pnmtx, texmtx[8], pos, nrm, col[2], tex[8];
    int pos_cnt, pos_fmt, pos_shift, nrm_cnt, nrm_fmt, nrm_idx3, col_fmt[2], tex_cnt[8], tex_fmt[8], tex_shift[8];
} VtxFormat;

static void vtx_format(int vat, VtxFormat *f) {
    uint32_t lo = cp[0x50], hi = cp[0x60], a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];
    f->pnmtx = lo & 1;
    for (int i = 0; i < 8; i++) f->texmtx[i] = (lo >> (1 + i)) & 1;
    f->pos = (lo >> 9) & 3;
    f->nrm = (lo >> 11) & 3;
    f->col[0] = (lo >> 13) & 3;
    f->col[1] = (lo >> 15) & 3;
    for (int i = 0; i < 8; i++) f->tex[i] = (hi >> (i * 2)) & 3;
    f->pos_cnt = a & 1; f->pos_fmt = (a >> 1) & 7; f->pos_shift = (a >> 4) & 31;
    f->nrm_cnt = (a >> 9) & 1; f->nrm_fmt = (a >> 10) & 7; f->nrm_idx3 = (a >> 31) & 1;
    f->col_fmt[0] = (a >> 14) & 7; f->col_fmt[1] = (a >> 18) & 7;
    f->tex_cnt[0] = (a >> 21) & 1; f->tex_fmt[0] = (a >> 22) & 7; f->tex_shift[0] = (a >> 25) & 31;
    f->tex_cnt[1] = b & 1; f->tex_fmt[1] = (b >> 1) & 7; f->tex_shift[1] = (b >> 4) & 31;
    f->tex_cnt[2] = (b >> 9) & 1; f->tex_fmt[2] = (b >> 10) & 7; f->tex_shift[2] = (b >> 13) & 31;
    f->tex_cnt[3] = (b >> 18) & 1; f->tex_fmt[3] = (b >> 19) & 7; f->tex_shift[3] = (b >> 22) & 31;
    f->tex_cnt[4] = (b >> 27) & 1; f->tex_fmt[4] = (b >> 28) & 7; f->tex_shift[4] = c & 31;
    f->tex_cnt[5] = (c >> 5) & 1; f->tex_fmt[5] = (c >> 6) & 7; f->tex_shift[5] = (c >> 9) & 31;
    f->tex_cnt[6] = (c >> 14) & 1; f->tex_fmt[6] = (c >> 15) & 7; f->tex_shift[6] = (c >> 18) & 31;
    f->tex_cnt[7] = (c >> 23) & 1; f->tex_fmt[7] = (c >> 24) & 7; f->tex_shift[7] = (c >> 27) & 31;
}

static int attr_size(int mode, int direct_size) { return mode == 0 ? 0 : mode == 1 ? direct_size : mode == 2 ? 1 : 2; }

static int vtx_size(const VtxFormat *f) {
    int n = f->pnmtx;
    for (int i = 0; i < 8; i++) n += f->texmtx[i];
    n += attr_size(f->pos, (f->pos_cnt ? 3 : 2) * k_comp_size[f->pos_fmt]);
    int nrm_comps = f->nrm_cnt ? 9 : 3;
    if (f->nrm >= 2 && f->nrm_cnt && f->nrm_idx3) n += 3 * (f->nrm == 2 ? 1 : 2);
    else n += attr_size(f->nrm, nrm_comps * k_comp_size[f->nrm_fmt]);
    for (int i = 0; i < 2; i++) n += attr_size(f->col[i], k_color_size[f->col_fmt[i]]);
    for (int i = 0; i < 8; i++) n += attr_size(f->tex[i], (f->tex_cnt[i] ? 2 : 1) * k_comp_size[f->tex_fmt[i]]);
    return n;
}

/* Address of an attribute: either inline in the stream (*p advances) or indexed into an array. */
static uint32_t attr_addr(int mode, int array, uint32_t *p, int direct_size) {
    if (mode == 1) { uint32_t a = *p; *p += (uint32_t)direct_size; return a; }
    uint32_t idx = mode == 2 ? rd8(*p) : rd16(*p);
    *p += mode == 2 ? 1 : 2;
    return 0x80000000u | ((cp[0xA0 + array] & 0x03FFFFFF) + idx * (cp[0xB0 + array] & 0xFF));
}

static void read_vertex(const VtxFormat *f, uint32_t *p, RawVertex *v) {
    uint32_t mi_a = xf[0x1018], mi_b = xf[0x1019];
    v->pnmtx = f->pnmtx ? rd8((*p)++) : (int)(mi_a & 63);
    for (int i = 0; i < 8; i++) {
        int def = i < 4 ? (int)((mi_a >> (6 + 6 * i)) & 63) : (int)((mi_b >> (6 * (i - 4))) & 63);
        v->texmtx[i] = f->texmtx[i] ? rd8((*p)++) : def;
    }
    if (f->pos) {
        int comps = f->pos_cnt ? 3 : 2;
        uint32_t a = attr_addr(f->pos, 0, p, comps * k_comp_size[f->pos_fmt]);
        for (int i = 0; i < comps; i++) v->pos[i] = read_comp(&a, f->pos_fmt, f->pos_shift);
        if (comps == 2) v->pos[2] = 0;
    }
    if (f->nrm) { /* normals are skipped until lighting is implemented */
        int comps = f->nrm_cnt ? 9 : 3;
        if (f->nrm >= 2 && f->nrm_cnt && f->nrm_idx3) *p += 3 * (f->nrm == 2 ? 1 : 2);
        else attr_addr(f->nrm, 1, p, comps * k_comp_size[f->nrm_fmt]);
    }
    for (int i = 0; i < 2; i++) {
        v->has_col[i] = 0;
        if (!f->col[i]) continue;
        uint32_t a = attr_addr(f->col[i], 2 + i, p, k_color_size[f->col_fmt[i]]);
        read_color(&a, f->col_fmt[i], v->col[i]);
        v->has_col[i] = 1;
    }
    for (int i = 0; i < 8; i++) {
        if (!f->tex[i]) continue;
        int comps = f->tex_cnt[i] ? 2 : 1;
        uint32_t a = attr_addr(f->tex[i], 4 + i, p, comps * k_comp_size[f->tex_fmt[i]]);
        v->tex[i][0] = read_comp(&a, f->tex_fmt[i], f->tex_shift[i]);
        v->tex[i][1] = comps == 2 ? read_comp(&a, f->tex_fmt[i], f->tex_shift[i]) : 0;
    }
}

static void unpack_color(uint32_t rgba, float *c) {
    c[0] = (rgba >> 24) / 255.f; c[1] = ((rgba >> 16) & 255) / 255.f; c[2] = ((rgba >> 8) & 255) / 255.f; c[3] = (rgba & 255) / 255.f;
}

/* Everything transform() needs that is constant for a whole draw, decoded once per draw. */
typedef struct {
    float p[6];          /* projection */
    int ortho;
    float sx, sy, sz, ox, oy, oz;
    /* color channels: [channel][rgb/alpha] -> constant value, or a vertex color times a factor */
    struct { int vtx; int src; int light; float konst[4]; float amb[4]; int amb_vtx; } ch[2][2];
    uint32_t ntexgen;
    struct { int src_row, proj, abc1; } tg[8];
} XfSetup;

static void xf_setup(XfSetup *s, const RawVertex *first) {
    for (int i = 0; i < 6; i++) s->p[i] = xff(0x1020 + i);
    s->ortho = xf[0x1026] & 1;
    s->sx = xff(0x101A); s->sy = xff(0x101B); s->sz = xff(0x101C);
    s->ox = xff(0x101D) - 342; s->oy = xff(0x101E) - 342; s->oz = xff(0x101F);
    for (int ch = 0; ch < 2; ch++)
        for (int part = 0; part < 2; part++) {
            uint32_t ctrl = xf[(part ? 0x1010 : 0x100E) + ch];
            s->ch[ch][part].vtx = ctrl & 1;
            s->ch[ch][part].light = (ctrl >> 1) & 1;
            s->ch[ch][part].amb_vtx = (ctrl >> 6) & 1;
            unpack_color(xf[0x100C + ch], s->ch[ch][part].konst);
            unpack_color(xf[0x100A + ch], s->ch[ch][part].amb);
            s->ch[ch][part].src = first->has_col[ch] ? ch : (first->has_col[0] ? 0 : -1);
        }
    s->ntexgen = xf[0x103F] & 15;
    for (uint32_t tc = 0; tc < 8; tc++) {
        uint32_t g = xf[0x1040 + tc];
        s->tg[tc].src_row = (g >> 7) & 31;
        s->tg[tc].proj = (g >> 1) & 1;
        s->tg[tc].abc1 = (g >> 2) & 1;
    }
}

static inline float channel_value(const XfSetup *s, const RawVertex *v, int ch, int part, int comp) {
    const typeof(s->ch[0][0]) *c = &s->ch[ch][part];
    float m = c->vtx && c->src >= 0 ? v->col[c->src][comp] : c->konst[comp];
    if (!c->light) return m;
    /* no per-light model yet: material x (ambient + a fixed directional estimate) */
    float a = c->amb_vtx && c->src >= 0 ? v->col[c->src][comp] : c->amb[comp];
    float lit = a + 0.6f;
    return m * (lit > 1.f ? 1.f : lit);
}

/* Position -> clip space (via the position matrix, projection and viewport); colors; texcoords. */
static void transform(const XfSetup *s, const RawVertex *v, GxVertex *o) {
    const float *M = (const float *)&xf[(uint32_t)v->pnmtx * 4];
    float x = v->pos[0], y = v->pos[1], z = v->pos[2];
    float vx = M[0] * x + M[1] * y + M[2] * z + M[3];
    float vy = M[4] * x + M[5] * y + M[6] * z + M[7];
    float vz = M[8] * x + M[9] * y + M[10] * z + M[11];
    const float *p = s->p;
    float cx, cy, cz, cw;
    if (s->ortho) { cx = p[0] * vx + p[1]; cy = p[2] * vy + p[3]; cz = p[4] * vz + p[5]; cw = 1; }
    else { cx = p[0] * vx + p[1] * vz; cy = p[2] * vy + p[3] * vz; cz = p[4] * vz + p[5]; cw = -vz; }
    /* viewport (applied in clip space so perspective-correct interpolation and clipping still work) */
    float wx = cx * s->sx + s->ox * cw, wy = cy * s->sy + s->oy * cw, wz = (cz * s->sz + s->oz * cw) * (1.0f / 16777215.0f);
    o->x = wx * (2.0f / EFB_WIDTH) - cw;
    o->y = cw - wy * (2.0f / EFB_HEIGHT);
    o->z = wz * 2.0f - cw;
    o->w = cw;

    for (int k = 0; k < 3; k++) {
        o->c0[k] = channel_value(s, v, 0, 0, k);
        o->c1[k] = channel_value(s, v, 1, 0, k);
    }
    o->c0[3] = channel_value(s, v, 0, 1, 3);
    o->c1[3] = channel_value(s, v, 1, 1, 3);

    /* texture coordinates: each texgen's source row, then its texture matrix */
    for (uint32_t tc = 0; tc < 8; tc++) {
        if (tc >= s->ntexgen) { o->uv[tc][0] = o->uv[tc][1] = 0; continue; }
        int src_row = s->tg[tc].src_row;
        float in0 = 0, in1 = 0, in2 = 1;
        if (src_row == 0) { in0 = x; in1 = y; in2 = z; }
        else if (src_row >= 5 && src_row <= 12) { in0 = v->tex[src_row - 5][0]; in1 = v->tex[src_row - 5][1]; }
        if (!s->tg[tc].abc1) in2 = 1; /* AB11 */
        const float *T = (const float *)&xf[(uint32_t)v->texmtx[tc] * 4];
        float s_ = T[0] * in0 + T[1] * in1 + T[2] * in2 + T[3];
        float t_ = T[4] * in0 + T[5] * in1 + T[6] * in2 + T[7];
        if (s->tg[tc].proj) {
            float q = T[8] * in0 + T[9] * in1 + T[10] * in2 + T[11];
            if (q != 0) { float iq = 1.0f / q; s_ *= iq; t_ *= iq; }
        }
        o->uv[tc][0] = s_;
        o->uv[tc][1] = t_;
    }
}

/* Render state is rebuilt only when a BP register changes (or a new frame starts); each rebuild
 * gets a new serial, so batching compares serials instead of whole states. */
static GxState g_state;
static uint32_t g_state_serial;
static int g_state_valid;

static void emit(GxPrimKind kind, const GxState *st, uint32_t serial, const GxVertex *vs, const uint32_t *idx, uint32_t n) {
    if (!n) return;
    GxFrame *f = frame();
    GxBatch *last = f->nbatches ? &f->batches[f->nbatches - 1] : NULL;
    int same = last && last->kind == kind && last->first + last->count == f->nverts &&
               !(f->nclears && f->clears[f->nclears - 1].at_batch == f->nbatches) &&
               (last->serial == serial || !memcmp(&last->state, st, sizeof *st));
    if (!same) {
        GROW(f->batches, f->nbatches, f->cap_batches, 256);
        last = &f->batches[f->nbatches++];
        last->state = *st;
        last->kind = kind;
        last->first = f->nverts;
        last->count = 0;
    }
    last->serial = serial;
    if (f->nverts + n > f->cap_verts) {
        while (f->nverts + n > f->cap_verts) f->cap_verts = f->cap_verts ? f->cap_verts * 2 : 4096;
        f->verts = realloc(f->verts, sizeof *f->verts * f->cap_verts);
    }
    GxVertex *dst = f->verts + f->nverts;
    for (uint32_t i = 0; i < n; i++) dst[i] = vs[idx[i]];
    f->nverts += n;
    last->count += n;
}

/* SFA_GX_DEBUG=1: print each distinct draw configuration once (for finding unhandled GX features). */
static void debug_draw_config(void) {
    static int enabled = -1;
    static uint32_t seen[512];
    static unsigned nseen;
    if (enabled < 0) enabled = g_dbg;
    if (!enabled) return;
    uint32_t tref = bp[0x28], map = tref & 7;
    uint32_t img0 = bp[map < 4 ? 0x88 + map : 0xA8 + (map - 4)], img3 = bp[map < 4 ? 0x94 + map : 0xB4 + (map - 4)];
    uint32_t nstages = ((bp[0x00] >> 10) & 15) + 1, ntex = bp[0x00] & 15;
    uint32_t key = fnv((const uint8_t *)&bp[0xC0], 8, 2166136261u) ^ (tref * 31) ^ (nstages << 24) ^ (img0 >> 20) * 977 ^ (bp[0xC0 + 2] * 7);
    for (unsigned i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 512) seen[nseen++] = key;
    printf("[gx-cfg] frame %u stages=%u ntex=%u tref0=%03X C0=%06X A0=%06X C1=%06X A1=%06X tex fmt=%u %ux%u addr=%08X kc=%06X\n",
           g_frame_number, nstages, ntex, tref & 0xFFF, bp[0xC0], bp[0xC1], bp[0xC2], bp[0xC3], (img0 >> 20) & 15, (img0 & 0x3FF) + 1,
           ((img0 >> 10) & 0x3FF) + 1, (img3 & 0xFFFFFF) << 5, bp[0xF6]);
}

/* SFA_PROFILE: time spent in each stage of draw(), reported with the guest profile. */
static uint64_t g_t_state, g_t_vtx, g_t_emit, g_t_calls, g_t_verts;
static int g_prof_on = -1;
static uint64_t ticks(void) { /* stage timing only when profiling: the clock call is not free */
    if (g_prof_on < 0) g_prof_on = getenv("SFA_PROFILE") != NULL;
    return g_prof_on ? rt_timebase() : 0;
}
void gx_profile_report(void) {
    if (!g_t_calls) return;
    printf("[prof] gx: %llu draws, %llu vertices; state %.0f ms, vertices %.0f ms, batching %.0f ms (per 10 s)\n",
           (unsigned long long)g_t_calls, (unsigned long long)g_t_verts, g_t_state / 40500.0, g_t_vtx / 40500.0,
           g_t_emit / 40500.0);
    g_t_state = g_t_vtx = g_t_emit = g_t_calls = g_t_verts = 0;
}

static void draw(uint8_t opcode, uint32_t p, uint32_t count) {
    debug_draw_config();
    uint64_t t0 = ticks();
    VtxFormat fmt;
    vtx_format(opcode & 7, &fmt);
    if (!g_state_valid) {
        current_state(&g_state);
        g_state_serial++;
        g_state_valid = 1;
    }
    uint64_t t1 = ticks();
    static GxVertex *vs;
    static uint32_t *idx, cap;
    if (count * 6 > cap) {
        cap = count * 6 + 64;
        vs = realloc(vs, sizeof *vs * cap);
        idx = realloc(idx, sizeof *idx * cap);
    }
    XfSetup xs;
    for (uint32_t i = 0; i < count; i++) {
        RawVertex rv;
        memset(&rv, 0, sizeof rv);
        read_vertex(&fmt, &p, &rv);
        if (i == 0) xf_setup(&xs, &rv);
        transform(&xs, &rv, &vs[i]);
    }
    uint64_t t2 = ticks();
    g_t_state += t1 - t0;
    g_t_vtx += t2 - t1;
    g_t_calls++;
    g_t_verts += count;
    uint32_t n = 0;
    GxPrimKind kind = GX_PRIM_TRIANGLES;
    switch (opcode & 0xF8) {
    case 0x80: case 0x88: /* quads */
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            idx[n++] = i; idx[n++] = i + 1; idx[n++] = i + 2;
            idx[n++] = i; idx[n++] = i + 2; idx[n++] = i + 3;
        }
        break;
    case 0x90: for (uint32_t i = 0; i + 2 < count; i += 3) { idx[n++] = i; idx[n++] = i + 1; idx[n++] = i + 2; } break;
    case 0x98: /* strip */
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) { idx[n++] = i + 1; idx[n++] = i; idx[n++] = i + 2; }
            else { idx[n++] = i; idx[n++] = i + 1; idx[n++] = i + 2; }
        }
        break;
    case 0xA0: for (uint32_t i = 1; i + 1 < count; i++) { idx[n++] = 0; idx[n++] = i; idx[n++] = i + 1; } break;
    case 0xA8: kind = GX_PRIM_LINES; for (uint32_t i = 0; i + 1 < count; i += 2) { idx[n++] = i; idx[n++] = i + 1; } break;
    case 0xB0: kind = GX_PRIM_LINES; for (uint32_t i = 0; i + 1 < count; i++) { idx[n++] = i; idx[n++] = i + 1; } break;
    default: kind = GX_PRIM_POINTS; for (uint32_t i = 0; i < count; i++) idx[n++] = i; break;
    }
    uint64_t t3 = ticks();
    emit(kind, &g_state, g_state_serial, vs, idx, n);
    g_t_emit += ticks() - t3;
    g_stat_draws++;
}

/* ---- BP side effects ----------------------------------------------------------------------- */
static void efb_copy(uint32_t v) {
    uint32_t tl = bp[0x49], wh = bp[0x4A];
    int x = (int)(tl & 0x3FF), y = (int)((tl >> 10) & 0x3FF), w = (int)(wh & 0x3FF) + 1, h = (int)((wh >> 10) & 0x3FF) + 1;
    int to_xfb = (v >> 14) & 1, clear = (v >> 11) & 1;
    if (!to_xfb && g_dbg)
        printf("[gx-copy] frame %u EFB (%d,%d %dx%d) -> texture %08X fmt %u half %u\n", g_frame_number, x, y, w, h,
               0x80000000u | ((bp[0x4B] & 0xFFFFFF) << 5), (v >> 4) & 15 | ((v >> 3) & 1) << 4, (v >> 9) & 1);
    if (to_xfb) {
        GxFrame *f = frame();
        f->copy_rect[0] = (uint16_t)x; f->copy_rect[1] = (uint16_t)y; f->copy_rect[2] = (uint16_t)w; f->copy_rect[3] = (uint16_t)h;
        static int dump_frame = -2;
        if (dump_frame == -2) dump_frame = getenv("SFA_GX_FRAMEDUMP") ? atoi(getenv("SFA_GX_FRAMEDUMP")) : -1;
        if ((int)f->number == dump_frame) { /* list every batch of one frame */
            printf("[frame %u] %u batches, %u vertices\n", f->number, f->nbatches, f->nverts);
            for (uint32_t b = 0; b < f->nbatches; b++) {
                const GxBatch *bt = &f->batches[b];
                const GxState *s = &bt->state;
                const GxVertex *v0 = &f->verts[bt->first];
                printf("  #%u kind %d verts %u stages %u tex[0..2] %u %u %u blend %u(%u,%u) zen %u aref %.2f/%.2f acmp %06X "
                       "C0 %06X A0 %06X tref0 %03X reg0 %.2f,%.2f,%.2f,%.2f k0 %.2f,%.2f,%.2f,%.2f c0 %.2f,%.2f,%.2f,%.2f "
                       "pos %.1f,%.1f w %.2f uv %.3f,%.3f\n",
                       b, bt->kind, bt->count, s->tev.nstages, s->texture[0], s->texture[1], s->texture[2], s->blend_enable,
                       s->blend_src, s->blend_dst, s->z_enable, s->alpha_ref[0], s->alpha_ref[1], s->tev.alpha_cmp,
                       s->tev.color_env[0], s->tev.alpha_env[0], s->tev.tref[0] & 0xFFF, s->reg[0][0], s->reg[0][1],
                       s->reg[0][2], s->reg[0][3], s->konst[0][0], s->konst[0][1], s->konst[0][2], s->konst[0][3],
                       v0->c0[0], v0->c0[1], v0->c0[2], v0->c0[3], v0->x / v0->w, v0->y / v0->w, v0->w, v0->uv[0][0], v0->uv[0][1]);
            }
        }
        g_frame = NULL;
        g_frame_number++;
        g_state_valid = 0; /* re-check textures each frame */
        video_submit(f);
    }
    if (clear) {
        GxFrame *f = frame();
        GROW(f->clears, f->nclears, f->cap_clears, 8);
        GxClear *cl = &f->clears[f->nclears++];
        uint32_t ar = bp[0x4F], gb = bp[0x50];
        cl->at_batch = f->nbatches;
        cl->color[0] = (ar & 255) / 255.f; cl->color[1] = ((gb >> 8) & 255) / 255.f;
        cl->color[2] = (gb & 255) / 255.f; cl->color[3] = ((ar >> 8) & 255) / 255.f;
        cl->depth = (bp[0x51] & 0xFFFFFF) / 16777215.f;
        cl->clear_color = 1;
        cl->clear_depth = (bp[0x40] >> 4) & 1;
    }
}

static void bp_write(uint32_t word) {
    uint32_t reg = word >> 24, val = word & 0xFFFFFF;
    if (reg == 0xFE) { bp_mask = val; return; }
    bp[reg] = (bp[reg] & ~bp_mask) | (val & bp_mask);
    g_state_valid = 0;
    bp_mask = 0xFFFFFF;
    switch (reg) {
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        tev_color_write(reg, val);
        break;
    case 0x47: case 0x48: /* PE token: GXReadDrawSync reads it back from PE_TOKEN */
        rt_mmio_set(0x100E, val & 0xFFFF, 2);
        break;
    case 0x52:
        efb_copy(bp[0x52]);
        break;
    case 0x63: /* TX_LOADBLOCK3: preload a texture into TMEM */
        if (g_dbg) {
            static unsigned n;
            if (n++ < 20)
                printf("[gx-preload] frame %u src %08X tmem even %05X odd %05X count %u (32B)\n", g_frame_number,
                       0x80000000u | ((bp[0x60] & 0xFFFFFF) << 5), (bp[0x61] & 0x7FFF) << 5, (bp[0x62] & 0x7FFF) << 5,
                       val & 0x7FFF);
        }
        break;
    case 0x65: { /* load TLUT: from main memory (BP 0x64) into TMEM */
        uint32_t src = 0x80000000u | ((bp[0x64] & 0xFFFFFF) << 5);
        uint32_t dst = ((val & 0x3FF) << 9) & (sizeof tmem_tlut - 1), bytes = ((val >> 10) & 0x7FF) * 32;
        for (uint32_t i = 0; i < bytes && dst + i < sizeof tmem_tlut; i++) tmem_tlut[dst + i] = rd8(src + i);
        break;
    }
    default:
        break;
    }
}

/* ---- command parser ---------------------------------------------------------------------- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static size_t run_commands(const uint8_t *b, size_t n, uint32_t guest_addr, int depth);

static void call_display_list(uint32_t addr, uint32_t size, int depth) {
    if (depth > 4 || !size) return;
    addr = 0x80000000u | (addr & 0x03FFFFFF);
    if (!ppc_is_ram(addr) || !ppc_is_ram(addr + size - 1)) return;
    run_commands(ppc_ram(addr), size, addr, depth + 1);
}

/* Parse complete commands from b[0..n); returns bytes consumed. `guest_addr` is the guest
 * address of b[0] when b lives in guest memory (display lists), else 0. */
static size_t run_commands(const uint8_t *b, size_t n, uint32_t guest_addr, int depth) {
    size_t i = 0;
    while (i < n) {
        uint8_t op = b[i];
        size_t need;
        if (op == 0x00 || op == 0x48 || op == 0x44) { i++; continue; }
        if (op == 0x08) need = 6;
        else if (op == 0x10) { if (n - i < 5) break; need = 5 + 4 * (((be32(b + i + 1) >> 16) & 15) + 1); }
        else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38 || op == 0x61) need = 5;
        else if (op == 0x40) need = 9;
        else if (op >= 0x80 && op < 0xC0) {
            if (n - i < 3) break;
            VtxFormat f;
            vtx_format(op & 7, &f);
            need = 3 + (size_t)((b[i + 1] << 8) | b[i + 2]) * (size_t)vtx_size(&f);
        } else {
            g_stat_unknown++;
            if (g_stat_unknown <= 10) printf("[gx] unknown command 0x%02X (depth %d)\n", op, depth);
            i++;
            continue;
        }
        if (n - i < need) break;
        const uint8_t *c = b + i;
        switch (op) {
        case 0x08: cp[c[1]] = be32(c + 2); break;
        case 0x10: {
            uint32_t hdr = be32(c + 1), addr = hdr & 0xFFFF, cnt = ((hdr >> 16) & 15) + 1;
            for (uint32_t k = 0; k < cnt; k++)
                if (addr + k < 0x1100) xf[addr + k] = be32(c + 5 + 4 * k);
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: {
            uint32_t v = be32(c + 1), arr = 12 + ((op - 0x20) >> 3), index = v >> 16, len = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
            uint32_t src = 0x80000000u | ((cp[0xA0 + arr] & 0x03FFFFFF) + index * (cp[0xB0 + arr] & 0xFF));
            for (uint32_t k = 0; k < len; k++) xf[(addr + k) & 0xFFF] = rd32(src + 4 * k);
            break;
        }
        case 0x40: call_display_list(be32(c + 1), be32(c + 5), depth); break;
        case 0x61:
            if (guest_addr && g_dbg_texaddr) { /* where does a display list set this texture address? */
                uint32_t w = be32(c + 1), r = w >> 24, want = g_dbg_texaddr;
                static unsigned shown;
                if (((r >= 0x94 && r <= 0x97) || (r >= 0xB4 && r <= 0xB7)) && (w & 0xFFFFFF) == ((want & 0x01FFFFFF) >> 5) && shown++ < 8)
                    printf("[gx-texaddr] display list at %08X: BP %02X image address field at %08X\n", guest_addr,
                           r, guest_addr + (uint32_t)i + 2);
            }
            bp_write(be32(c + 1));
            break;
        default: { /* primitive */
            uint32_t count = (uint32_t)((c[1] << 8) | c[2]);
            if (guest_addr) {
                draw(op, guest_addr + (uint32_t)i + 3, count);
            } else { /* vertices are in the host FIFO buffer: read them through the stage window */
                g_stage = c + 3;
                draw(op, STAGE_BASE, count);
                g_stage = NULL;
            }
            break;
        }
        }
        i += need;
    }
    return i;
}

/* ---- write-gather pipe ---------------------------------------------------------------------- */
static uint8_t *g_fifo;
static size_t g_fifo_len, g_fifo_cap;

/* ---- display list recording -----------------------------------------------------------------
 * GXBeginDisplayList points the CPU FIFO (PI registers 0x300C base / 0x3010 end / 0x3014 write
 * pointer) at a RAM buffer while the GPU FIFO (CP 0x20/0x22 base) stays where it was. Pipe writes
 * then go to that buffer instead of being executed; GXEndDisplayList reads the write pointer back
 * to learn the list's size. */
static uint32_t g_dl_ptr; /* physical write pointer while recording */

void gx_pi_wptr_write(uint32_t v) { g_dl_ptr = v & 0x03FFFFE0u; }

uint32_t gx_pi_wptr_read(void) {
    /* the write-gather pipe drains in 32-byte bursts: pad the tail with NOPs */
    while (g_dl_ptr & 31) wr8(0x80000000u | g_dl_ptr++, 0);
    return g_dl_ptr & 0x03FFFFE0u;
}

static int recording_display_list(void) {
    uint32_t pi_base = rt_mmio_get(0x300C, 4) & 0x03FFFFFFu;
    uint32_t cp_base = (rt_mmio_get(0x20, 2) | rt_mmio_get(0x22, 2) << 16) & 0x03FFFFFFu;
    return pi_base && cp_base && pi_base != cp_base;
}

void gx_fifo_write(uint32_t value, int size) {
    if (recording_display_list()) {
        for (int i = size - 1; i >= 0; i--) wr8(0x80000000u | g_dl_ptr++, (uint8_t)(value >> (i * 8)));
        return;
    }
    if (g_fifo_len + 8 > g_fifo_cap) {
        g_fifo_cap = g_fifo_cap ? g_fifo_cap * 2 : 1 << 16;
        g_fifo = realloc(g_fifo, g_fifo_cap);
    }
    for (int i = size - 1; i >= 0; i--) g_fifo[g_fifo_len++] = (uint8_t)(value >> (i * 8));
    size_t used = run_commands(g_fifo, g_fifo_len, 0, 0);
    if (used) {
        memmove(g_fifo, g_fifo + used, g_fifo_len - used);
        g_fifo_len -= used;
    }
}

void gx_init(void) {
    dbg_init();
    bp[0x41] = 0x18; /* color and alpha update */
    bp[0x40] = 0x17; /* z test LEQUAL with update */
}

/* gx.c - GX command processor: decodes the stream the game writes to the write-gather pipe at
 * 0xCC008000 (and the display lists it calls), tracks CP/XF/BP state, transforms vertices on
 * the CPU and builds per-frame draw lists for the renderer (video.c).
 *
 * Coverage so far: all register loads, indexed XF loads, display lists, every primitive type,
 * every vertex attribute format, position/texture matrices, perspective/orthographic projection,
 * the viewport, all texture formats (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2,
 * CMPR), TLUT loads, blending, depth, scissor, alpha test, lighting, multi-stage TEV, and EFB copies
 * to the XFB and to textures (on the GPU). Not yet: indirect texturing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include "funcs.h"
#include "runtime.h"
#include "gx.h"

/* ---- state ------------------------------------------------------------------------------------- */
static uint32_t cp[256];
static uint32_t g_cp_serial = 1; /* bumped when a CP register changes */
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
        g_frame = video_new_frame(g_frame_number);
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

static uint64_t ticks(void);
static uint64_t g_t_decode, g_t_ndecode, g_t_decode_px; /* texture decode time (profiling) */

/* EFB copies to texture kept on the GPU (see efb_copy_to_texture): the texture at `addr` is the
 * renderer's texture `id`. Guest memory there is not written, so it keeps its old bytes: their hash
 * at copy time tells whether the CPU has since put something else there (then memory wins). */
typedef struct {
    uint32_t addr, id, size, ram_hash, checked_frame;
} GpuCopy;
#define GPU_COPIES 32
static GpuCopy g_gpu_copies[GPU_COPIES];
static int g_ngpu_copies; /* live entries */

static uint32_t gpu_copy_for(uint32_t addr) {
    for (int i = 0; i < GPU_COPIES; i++) {
        GpuCopy *c = &g_gpu_copies[i];
        if (!c->id || c->addr != addr) continue;
        if (c->checked_frame != g_frame_number) {
            uint32_t hash = fnv(ppc_ram(addr), c->size, 2166136261u);
            if (hash != c->ram_hash) { /* the CPU wrote new data there */
                c->id = 0;
                g_ngpu_copies--;
                return 0;
            }
            c->checked_frame = g_frame_number;
        }
        return c->id;
    }
    return 0;
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
            Ctx *gc = rt_current_ctx(); /* with SFA_GX_SYNC=1 this is the guest thread drawing: walk its stack */
            if (gc) {
                printf("[gx-texaddr]   guest call stack (innermost first): %s", rt_func_containing(gc->lr));
                uint32_t sp = gc->r[1];
                for (int k = 0; k < 16 && sp && ppc_is_ram(sp); k++) {
                    uint32_t next = rd32(sp);
                    if (!ppc_is_ram(next) || next <= sp) break;
                    printf(" < %s", rt_func_containing(rd32(next + 4)));
                    sp = next;
                }
                printf("\n");
            }
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
    if (g_ngpu_copies) {
        uint32_t id = gpu_copy_for(addr);
        if (id) return id;
    }
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
    uint64_t td = ticks();
    u->rgba = decode_texture(addr, fmt, w, h, pal, tlut_fmt);
    g_t_decode += ticks() - td;
    g_t_ndecode++;
    g_t_decode_px += (uint64_t)w * h;
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
    float nrm[3];
    int pnmtx, texmtx[8];
    int has_col[2], has_nrm;
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

/* Vertex attributes are read through host pointers: vspan() resolves a guest span once (the
 * write-gather staging buffer or RAM), falling back to a byte-by-byte copy for anything else. */
static const uint8_t *vspan(uint32_t a, uint32_t n, uint8_t *tmp) {
    if (a - STAGE_BASE < 0x10000000u) return g_stage + (a - STAGE_BASE);
    if (ppc_is_ram(a) && ppc_is_ram(a + n - 1) && (a & 0x01FFFFFFu) < ((a + n - 1) & 0x01FFFFFFu) + 1) return ppc_ram(a);
    for (uint32_t i = 0; i < n; i++) tmp[i] = vrd8(a + i);
    return tmp;
}
static inline uint32_t be16p(const uint8_t *q) { return (uint32_t)q[0] << 8 | q[1]; }
static inline uint32_t be32p(const uint8_t *q) { return (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]; }

/* n components of format fmt from q, times scale (1 / 2^shift; ignored for floats). */
static inline void read_comps(const uint8_t *q, int fmt, float scale, int n, float *out) {
    switch (fmt) {
    case 0: for (int i = 0; i < n; i++) out[i] = q[i] * scale; break;
    case 1: for (int i = 0; i < n; i++) out[i] = (int8_t)q[i] * scale; break;
    case 2: for (int i = 0; i < n; i++) out[i] = be16p(q + 2 * i) * scale; break;
    case 3: for (int i = 0; i < n; i++) out[i] = (int16_t)be16p(q + 2 * i) * scale; break;
    default: for (int i = 0; i < n; i++) out[i] = u2f(be32p(q + 4 * i)); break;
    }
}

static inline void read_color(const uint8_t *q, int fmt, float *c) {
    uint32_t v;
    switch (fmt) {
    case 0: v = be16p(q); c[0] = ((v >> 11) & 31) / 31.f; c[1] = ((v >> 5) & 63) / 63.f; c[2] = (v & 31) / 31.f; c[3] = 1; break;
    case 1: case 2: c[0] = q[0] / 255.f; c[1] = q[1] / 255.f; c[2] = q[2] / 255.f; c[3] = 1; break;
    case 3: v = be16p(q); c[0] = ((v >> 12) & 15) / 15.f; c[1] = ((v >> 8) & 15) / 15.f; c[2] = ((v >> 4) & 15) / 15.f; c[3] = (v & 15) / 15.f; break;
    case 4: v = (uint32_t)q[0] << 16 | q[1] << 8 | q[2];
        c[0] = ((v >> 18) & 63) / 63.f; c[1] = ((v >> 12) & 63) / 63.f; c[2] = ((v >> 6) & 63) / 63.f; c[3] = (v & 63) / 63.f; break;
    default: c[0] = q[0] / 255.f; c[1] = q[1] / 255.f; c[2] = q[2] / 255.f; c[3] = q[3] / 255.f; break;
    }
}

static const int k_comp_size[8] = {1, 1, 2, 2, 4, 4, 4, 4};
static const int k_color_size[8] = {2, 3, 4, 2, 3, 4, 4, 4};

typedef struct {
    int pnmtx, texmtx[8], pos, nrm, col[2], tex[8];
    int pos_cnt, pos_fmt, pos_shift, nrm_cnt, nrm_fmt, nrm_idx3, col_fmt[2], tex_cnt[8], tex_fmt[8], tex_shift[8];
    float pos_scale, nrm_scale, tex_scale[8];
    int size; /* bytes per vertex in the stream */
} VtxFormat;

static int vtx_size(const VtxFormat *f);

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
    f->pos_scale = 1.0f / (float)(1u << f->pos_shift);
    /* s8 normals have 6 fractional bits, s16 have 14 */
    f->nrm_scale = f->nrm_fmt <= 1 ? 1.0f / 64 : f->nrm_fmt <= 3 ? 1.0f / 16384 : 1.0f;
    for (int i = 0; i < 8; i++) f->tex_scale[i] = 1.0f / (float)(1u << f->tex_shift[i]);
    f->size = vtx_size(f);
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

/* What reading a vertex needs besides its bytes: the vertex format, the attribute arrays (CP 0xA0 + n base,
 * 0xB0 + n stride) and the default matrix indices (XF 0x1018/0x1019). Snapshotted per draw configuration,
 * so vertices can be read later on a worker thread. */
typedef struct {
    VtxFormat f;
    uint32_t base[12], stride[12];
    uint32_t mi_a, mi_b;
} ReadCtx;

static void read_ctx(ReadCtx *rc, const VtxFormat *f) {
    rc->f = *f;
    for (int i = 0; i < 12; i++) {
        rc->base[i] = 0x80000000u | (cp[0xA0 + i] & 0x03FFFFFF);
        rc->stride[i] = cp[0xB0 + i] & 0xFF;
    }
    rc->mi_a = xf[0x1018];
    rc->mi_b = xf[0x1019];
}

/* An attribute's data: inline in the stream at s + *off (which advances), or indexed into an array. */
static const uint8_t *attr_ptr(const ReadCtx *rc, int mode, int array, const uint8_t *s, int *off, int direct_size,
                               uint8_t *tmp) {
    if (mode == 1) { const uint8_t *q = s + *off; *off += direct_size; return q; }
    uint32_t idx = mode == 2 ? s[*off] : be16p(s + *off);
    *off += mode == 2 ? 1 : 2;
    uint32_t a = rc->base[array] + idx * rc->stride[array];
    return vspan(a, (uint32_t)(direct_size > 0 ? direct_size : 1), tmp);
}

/* Reads one vertex from its bytes s (rc->f.size of them). Fields for attributes the format lacks are left
 * as they are, except the flags and texcoords transform() looks at. */
static void read_vertex(const ReadCtx *rc, const uint8_t *s, RawVertex *v) {
    const VtxFormat *f = &rc->f;
    uint8_t abuf[64];
    int off = 0;
    uint32_t mi_a = rc->mi_a, mi_b = rc->mi_b;
    v->pnmtx = f->pnmtx ? s[off++] : (int)(mi_a & 63);
    for (int i = 0; i < 8; i++) {
        int def = i < 4 ? (int)((mi_a >> (6 + 6 * i)) & 63) : (int)((mi_b >> (6 * (i - 4))) & 63);
        v->texmtx[i] = f->texmtx[i] ? s[off++] : def;
    }
    v->pos[0] = v->pos[1] = v->pos[2] = 0;
    if (f->pos) {
        int comps = f->pos_cnt ? 3 : 2;
        const uint8_t *q = attr_ptr(rc, f->pos, 0, s, &off, comps * k_comp_size[f->pos_fmt], abuf);
        read_comps(q, f->pos_fmt, f->pos_scale, comps, v->pos);
    }
    v->has_nrm = 0;
    if (f->nrm) { /* the normal (binormal/tangent of NBT are skipped: no bump mapping yet) */
        int comps = f->nrm_cnt ? 9 : 3;
        const uint8_t *q;
        if (f->nrm >= 2 && f->nrm_cnt && f->nrm_idx3) { /* three separate indices; the first is the normal */
            q = attr_ptr(rc, f->nrm, 1, s, &off, 3 * k_comp_size[f->nrm_fmt], abuf);
            off += 2 * (f->nrm == 2 ? 1 : 2);
        } else {
            q = attr_ptr(rc, f->nrm, 1, s, &off, comps * k_comp_size[f->nrm_fmt], abuf);
        }
        read_comps(q, f->nrm_fmt, f->nrm_scale, 3, v->nrm);
        v->has_nrm = 1;
    }
    for (int i = 0; i < 2; i++) {
        v->has_col[i] = 0;
        if (!f->col[i]) continue;
        const uint8_t *q = attr_ptr(rc, f->col[i], 2 + i, s, &off, k_color_size[f->col_fmt[i]], abuf);
        read_color(q, f->col_fmt[i], v->col[i]);
        v->has_col[i] = 1;
    }
    for (int i = 0; i < 8; i++) {
        v->tex[i][0] = v->tex[i][1] = 0;
        if (!f->tex[i]) continue;
        int comps = f->tex_cnt[i] ? 2 : 1;
        const uint8_t *q = attr_ptr(rc, f->tex[i], 4 + i, s, &off, comps * k_comp_size[f->tex_fmt[i]], abuf);
        read_comps(q, f->tex_fmt[i], f->tex_scale[i], comps, v->tex[i]);
    }
    (void)off; /* off == f->size */
}

static void unpack_color(uint32_t rgba, float *c) {
    c[0] = (rgba >> 24) / 255.f; c[1] = ((rgba >> 16) & 255) / 255.f; c[2] = ((rgba >> 8) & 255) / 255.f; c[3] = (rgba & 255) / 255.f;
}

/* Everything transform() needs that is constant for a whole draw, decoded once per draw. */
typedef struct {
    float color[4];
    float pos[3], dir[3];
    float a[3], k[3]; /* angle (cos) and distance attenuation coefficients */
} XfLight;

typedef struct {
    int vtx_mat, vtx_amb, light, mask, diffuse, atten, spot;
    float mat[4], amb[4];
} XfChannel;

typedef struct {
    float p[6];          /* projection */
    int ortho;
    float sx, sy, sz, ox, oy, oz;
    XfChannel ch[2][2];  /* [channel 0/1][color/alpha] */
    int any_light;
    XfLight light[8];
    uint32_t ntexgen;
    struct { int src_row, proj, abc1; } tg[8];
} XfSetup;

static void xf_setup(XfSetup *s, const RawVertex *first) {
    (void)first;
    for (int i = 0; i < 6; i++) s->p[i] = xff(0x1020 + i);
    s->ortho = xf[0x1026] & 1;
    s->sx = xff(0x101A); s->sy = xff(0x101B); s->sz = xff(0x101C);
    s->ox = xff(0x101D) - 342; s->oy = xff(0x101E) - 342; s->oz = xff(0x101F);
    /* channel controls: 0 MatSrc, 1 LightFunc, 2-5 lights 0-3, 6 AmbSrc, 7-8 DiffuseAtten,
     * 9 AttnEnable, 10 AttnSelect (1 = spot), 11-14 lights 4-7 */
    s->any_light = 0;
    for (int ch = 0; ch < 2; ch++)
        for (int part = 0; part < 2; part++) {
            uint32_t ctrl = xf[(part ? 0x1010 : 0x100E) + ch];
            XfChannel *c = &s->ch[ch][part];
            c->vtx_mat = ctrl & 1;
            c->light = (ctrl >> 1) & 1;
            c->mask = (int)(((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4));
            c->vtx_amb = (ctrl >> 6) & 1;
            c->diffuse = (ctrl >> 7) & 3;
            c->atten = (ctrl >> 9) & 1;
            c->spot = (ctrl >> 10) & 1;
            unpack_color(xf[0x100C + ch], c->mat);
            unpack_color(xf[0x100A + ch], c->amb);
            if (c->light && c->mask) s->any_light |= c->mask;
        }
    for (int l = 0; l < 8; l++) {
        if (!(s->any_light & (1 << l))) continue;
        uint32_t b = 0x600u + (uint32_t)l * 16;
        XfLight *L = &s->light[l];
        unpack_color(xf[b + 3], L->color);
        for (int i = 0; i < 3; i++) {
            L->a[i] = xff(b + 4 + (uint32_t)i);
            L->k[i] = xff(b + 7 + (uint32_t)i);
            L->pos[i] = xff(b + 10 + (uint32_t)i);
            L->dir[i] = xff(b + 13 + (uint32_t)i);
        }
    }
    s->ntexgen = xf[0x103F] & 15;
    for (uint32_t tc = 0; tc < 8; tc++) {
        uint32_t g = xf[0x1040 + tc];
        s->tg[tc].src_row = (g >> 7) & 31;
        s->tg[tc].proj = (g >> 1) & 1;
        s->tg[tc].abc1 = (g >> 2) & 1;
    }
}

/* One color channel part (rgb or alpha) for one vertex, with the GX lighting model:
 * out = material * clamp(ambient + sum over enabled lights of color * diffuse * attenuation). */
/* Per-light factor (attenuation * diffuse) for one channel part's light setup. */
static void light_factors(const XfSetup *s, const XfChannel *c, const float *vpos, const float *n, float *fac) {
    for (int l = 0; l < 8; l++) {
        if (!(c->mask & (1 << l))) continue;
        const XfLight *L = &s->light[l];
        float ld[3] = {L->pos[0] - vpos[0], L->pos[1] - vpos[1], L->pos[2] - vpos[2]};
        float d2 = ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2];
        float d = sqrtf(d2);
        if (d > 0) { ld[0] /= d; ld[1] /= d; ld[2] /= d; }
        float attn = 1.0f;
        if (c->atten) {
            if (c->spot) { /* spot: angle attenuation over distance attenuation */
                float cs = ld[0] * L->dir[0] + ld[1] * L->dir[1] + ld[2] * L->dir[2];
                if (cs < 0) cs = 0;
                float num = L->a[0] + L->a[1] * cs + L->a[2] * cs * cs;
                float den = L->k[0] + L->k[1] * d + L->k[2] * d2;
                attn = num > 0 && den > 0 ? num / den : 0;
            } else { /* specular: light direction is the half-angle vector, position holds direction */
                float hl = sqrtf(L->pos[0] * L->pos[0] + L->pos[1] * L->pos[1] + L->pos[2] * L->pos[2]);
                float nl = hl > 0 ? (n[0] * L->pos[0] + n[1] * L->pos[1] + n[2] * L->pos[2]) / hl : 0;
                if (nl <= 0) { attn = 0; }
                else {
                    float nh = n[0] * L->dir[0] + n[1] * L->dir[1] + n[2] * L->dir[2];
                    if (nh < 0) nh = 0;
                    float num = L->a[0] + L->a[1] * nh + L->a[2] * nh * nh;
                    float den = L->k[0] + L->k[1] * nh + L->k[2] * nh * nh;
                    attn = num > 0 && den > 0 ? num / den : 0;
                }
                ld[0] = L->pos[0]; ld[1] = L->pos[1]; ld[2] = L->pos[2];
                if (hl > 0) { ld[0] /= hl; ld[1] /= hl; ld[2] /= hl; }
            }
        }
        float diff = 1.0f;
        if (c->diffuse) {
            diff = n[0] * ld[0] + n[1] * ld[1] + n[2] * ld[2];
            if (c->diffuse == 2 && diff < 0) diff = 0; /* clamp */
        }
        fac[l] = attn * diff;
    }
}

static int same_light_setup(const XfChannel *a, const XfChannel *b) {
    return a->mask == b->mask && a->diffuse == b->diffuse && a->atten == b->atten && a->spot == b->spot;
}

/* One color channel part (rgb or alpha); fac holds this part's light factors when lit. */
static void light_channel(const XfSetup *s, const RawVertex *v, int ch, int part, const float *fac, float *out) {
    const XfChannel *c = &s->ch[ch][part];
    int src = v->has_col[ch] ? ch : (v->has_col[0] ? 0 : -1);
    const float *mat = c->vtx_mat && src >= 0 ? v->col[src] : c->mat;
    int k0 = part ? 3 : 0, k1 = part ? 4 : 3;
    if (!c->light) {
        for (int k = k0; k < k1; k++) out[k] = mat[k];
        return;
    }
    const float *amb = c->vtx_amb && src >= 0 ? v->col[src] : c->amb;
    float acc[4] = {amb[0], amb[1], amb[2], amb[3]};
    for (int l = 0; l < 8; l++) {
        if (!(c->mask & (1 << l))) continue;
        const XfLight *L = &s->light[l];
        for (int k = k0; k < k1; k++) acc[k] += L->color[k] * fac[l];
    }
    for (int k = k0; k < k1; k++) {
        float a = acc[k] < 0 ? 0 : acc[k] > 1 ? 1 : acc[k];
        out[k] = mat[k] * a;
    }
}

/* The XF matrix memory transform() reads, copied when it changes so vertices can be transformed later
 * (on worker threads, see "Deferred vertex transform"): position/texture matrices xf[0x000..0x110) (row
 * index up to 63, 3 rows of 4), then normal matrices xf[0x400..0x468) (index up to 31, 3x3). */
#define MTX_PT_WORDS 0x110u
#define MTX_NRM_WORDS 0x68u
#define MTX_WORDS (MTX_PT_WORDS + MTX_NRM_WORDS)

/* Position -> clip space (via the position matrix, projection and viewport); colors; texcoords.
 * mx: position/texture matrices, mn: normal matrices (live XF memory, or a snapshot as laid out above). */
static void transform(const XfSetup *s, const uint32_t *mx, const uint32_t *mn, const RawVertex *v, GxVertex *o) {
    const float *M = (const float *)&mx[(uint32_t)v->pnmtx * 4];
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

    /* lighting works in view space: transform the normal by the normal matrix */
    float vpos[3] = {vx, vy, vz}, n[3] = {0, 0, 1};
    if (s->any_light && v->has_nrm) {
        const float *N = (const float *)&mn[((uint32_t)v->pnmtx & 31) * 3];
        n[0] = N[0] * v->nrm[0] + N[1] * v->nrm[1] + N[2] * v->nrm[2];
        n[1] = N[3] * v->nrm[0] + N[4] * v->nrm[1] + N[5] * v->nrm[2];
        n[2] = N[6] * v->nrm[0] + N[7] * v->nrm[1] + N[8] * v->nrm[2];
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0) { n[0] /= len; n[1] /= len; n[2] /= len; }
    }
    for (int ch = 0; ch < 2; ch++) {
        const XfChannel *cc = &s->ch[ch][0], *ca = &s->ch[ch][1];
        float fc[8], fa[8];
        const float *pa = fa;
        if (cc->light) light_factors(s, cc, vpos, n, fc);
        if (ca->light) {
            if (cc->light && same_light_setup(cc, ca)) pa = fc;
            else light_factors(s, ca, vpos, n, fa);
        }
        float *out = ch ? o->c1 : o->c0;
        light_channel(s, v, ch, 0, fc, out);
        light_channel(s, v, ch, 1, pa, out);
    }

    /* texture coordinates: each texgen's source row, then its texture matrix */
    for (uint32_t tc = 0; tc < 8; tc++) {
        if (tc >= s->ntexgen) { o->uv[tc][0] = o->uv[tc][1] = 0; continue; }
        int src_row = s->tg[tc].src_row;
        float in0 = 0, in1 = 0, in2 = 1;
        if (src_row == 0) { in0 = x; in1 = y; in2 = z; }
        else if (src_row >= 5 && src_row <= 12) { in0 = v->tex[src_row - 5][0]; in1 = v->tex[src_row - 5][1]; }
        if (!s->tg[tc].abc1) in2 = 1; /* AB11 */
        const float *T = (const float *)&mx[(uint32_t)v->texmtx[tc] * 4];
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
static uint32_t g_xf_serial = 1; /* bumped by writes to XF lights and registers (0x600 and up) */
static uint32_t g_state_serial;
static int g_state_valid;

static void gx_drain(void);

/* Adds n vertices to the frame's current batch (or a new one) and returns where they go in f->verts; the
 * caller (the deferred transform) fills them in. */
static uint32_t emit(GxPrimKind kind, const GxState *st, uint32_t serial, uint32_t n) {
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
        gx_drain(); /* the workers write into f->verts: let them finish before it moves */
        while (f->nverts + n > f->cap_verts) f->cap_verts = f->cap_verts ? f->cap_verts * 2 : 4096;
        f->verts = realloc(f->verts, sizeof *f->verts * f->cap_verts);
    }
    uint32_t dst = f->nverts;
    f->nverts += n;
    last->count += n;
    return dst;
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
static uint64_t g_t_state, g_t_vtx, g_t_emit, g_t_calls, g_t_verts, g_t_copy, g_t_ncopy;
static uint64_t g_t_sync, g_t_ringfull, g_t_gxbusy; /* guest waiting in GXDrawDone / on a full ring; GX thread working */
uint64_t g_t_render; /* renderer thread: time in render() (video.c) */
static uint64_t g_t_xform; /* wall time of the deferred vertex transform's flushes */
static int g_nworkers;     /* its worker threads */
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
    printf("[prof] gx: %llu texture decodes (%.1f Mpixels), %.0f ms (per 10 s)\n", (unsigned long long)g_t_ndecode,
           g_t_decode_px / 1e6, g_t_decode / 40500.0);
    g_t_decode = g_t_ndecode = g_t_decode_px = 0;
    static uint32_t last_frames;
    printf("[prof] split: GX thread busy %.0f ms, renderer %.0f ms; guest waited on GXDrawDone %.0f ms, on a full ring %.0f ms (per 10 s)\n",
           g_t_gxbusy / 40500.0, g_t_render / 40500.0, g_t_sync / 40500.0, g_t_ringfull / 40500.0);
    printf("[prof] gx: vertex transform %.0f ms of GX thread time (%d workers; per 10 s)\n", g_t_xform / 40500.0,
           g_nworkers);
    g_t_gxbusy = g_t_render = g_t_sync = g_t_ringfull = g_t_xform = 0;
    printf("[prof] gx: %u frames (%.1f fps); %llu EFB-to-texture copies, guest waited %.0f ms (per 10 s)\n",
           g_frame_number - last_frames, (g_frame_number - last_frames) / 10.0, (unsigned long long)g_t_ncopy,
           g_t_copy / 40500.0);
    last_frames = g_frame_number;
    g_t_state = g_t_vtx = g_t_emit = g_t_calls = g_t_verts = g_t_copy = g_t_ncopy = 0;
}

/* The setup unit turns texcoords into texel units with SU_SSIZE/SU_TSIZE (BP 0x30 + 2n / 0x31 + 2n, size - 1
 * for texcoord n). GX normally loads the size of the texture the coordinate is used with, so normalized
 * coordinates work; with GXSetTexCoordScaleManually a game can set any scale and send other units (the
 * system-font text uses texel coordinates with a scale of 1). The renderer samples normalized coordinates,
 * so rescale each texcoord by its SU size over the size of the texture it's sampled from. */
/* The setup unit turns texcoords into texel units with SU_SSIZE/SU_TSIZE (BP 0x30 + 2n / 0x31 + 2n, size - 1
 * for texcoord n). GX normally loads the size of the texture the coordinate is used with, so normalized
 * coordinates work; with GXSetTexCoordScaleManually a game can set any scale and send other units (the
 * system-font text uses texel coordinates with a scale of 1). The renderer samples normalized coordinates,
 * so rescale each texcoord by its SU size over the size of the texture it's sampled from. Returns a mask of
 * the texcoords that need it, with their factors in ks/kt. */
static uint32_t texcoord_scale_factors(float *ks, float *kt) {
    uint32_t need = 0, nstages = ((bp[0x00] >> 10) & 15) + 1;
    for (uint32_t k = 0; k < nstages; k++) {
        uint32_t tref = bp[0x28 + k / 2] >> ((k & 1) ? 12 : 0);
        if (!((tref >> 6) & 1)) continue;
        uint32_t map = tref & 7, coord = (tref >> 3) & 7;
        if (need & (1u << coord)) continue;
        uint32_t img0 = bp[map < 4 ? 0x88 + map : 0xA8 + (map - 4)];
        uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1;
        uint32_t ss = (bp[0x30 + 2 * coord] & 0xFFFF) + 1, ts = (bp[0x31 + 2 * coord] & 0xFFFF) + 1;
        if (ss == w && ts == h) continue;
        ks[coord] = (float)ss / (float)w;
        kt[coord] = (float)ts / (float)h;
        need |= 1u << coord;
    }
    return need;
}

/* Vertices a primitive of `count` vertices becomes (as triangles, lines or points), and its kind. */
static uint32_t prim_out_count(uint8_t opcode, uint32_t count, GxPrimKind *kind) {
    *kind = GX_PRIM_TRIANGLES;
    switch (opcode & 0xF8) {
    case 0x80: case 0x88: return count / 4 * 6; /* quads */
    case 0x90: return count / 3 * 3;
    case 0x98: case 0xA0: return count >= 3 ? (count - 2) * 3 : 0; /* strip, fan */
    case 0xA8: *kind = GX_PRIM_LINES; return count / 2 * 2;
    case 0xB0: *kind = GX_PRIM_LINES; return count >= 2 ? (count - 1) * 2 : 0;
    default: *kind = GX_PRIM_POINTS; return count;
    }
}

/* Writes a primitive's transformed vertices `vs` out as triangles, lines or points. */
static void prim_expand(uint8_t opcode, uint32_t count, const GxVertex *vs, GxVertex *out) {
    uint32_t n = 0;
    switch (opcode & 0xF8) {
    case 0x80: case 0x88:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            out[n++] = vs[i]; out[n++] = vs[i + 1]; out[n++] = vs[i + 2];
            out[n++] = vs[i]; out[n++] = vs[i + 2]; out[n++] = vs[i + 3];
        }
        break;
    case 0x90: for (uint32_t i = 0; i + 2 < count; i += 3) { out[n++] = vs[i]; out[n++] = vs[i + 1]; out[n++] = vs[i + 2]; } break;
    case 0x98:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) { out[n++] = vs[i + 1]; out[n++] = vs[i]; out[n++] = vs[i + 2]; }
            else { out[n++] = vs[i]; out[n++] = vs[i + 1]; out[n++] = vs[i + 2]; }
        }
        break;
    case 0xA0: for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = vs[0]; out[n++] = vs[i]; out[n++] = vs[i + 1]; } break;
    case 0xA8: for (uint32_t i = 0; i + 1 < count; i += 2) { out[n++] = vs[i]; out[n++] = vs[i + 1]; } break;
    case 0xB0: for (uint32_t i = 0; i + 1 < count; i++) { out[n++] = vs[i]; out[n++] = vs[i + 1]; } break;
    default: for (uint32_t i = 0; i < count; i++) out[n++] = vs[i]; break;
    }
}

/* ---- Deferred vertex work -----------------------------------------------------------------------
 * Reading vertices (indexed lookups into the game's arrays) and transforming them (matrices, lighting,
 * texgen) is most of the GX thread's work: Mission 10 has about 9,000 draws and 43,000 vertices a frame.
 * So draw() only copies each draw's vertex bytes, notes what reading and transforming them needs (the
 * read context, the XF setup and a copy of the matrices, each snapshotted when it changes), reserves the
 * vertices' place in the frame and publishes the draw as a job. Worker threads take published jobs while
 * the GX thread goes on with the commands. Each vertex gets exactly the same arithmetic as before, so the
 * output is identical.
 *
 * Workers read the attribute arrays from guest RAM a little later than the GX thread would have. The
 * game may change an array once the GPU is past the draws that use it, which it learns only through
 * GXDrawDone and PE tokens, so the GX thread finishes all published jobs before either reports progress
 * (gx_drain), and before the frame goes to the renderer or anything else reads f->verts
 * (gx_flush_vertices).
 *
 * The job, byte and snapshot arrays have fixed sizes (workers read them while the GX thread adds to them);
 * when one fills up, or f->verts must grow, the GX thread first waits for the published jobs.
 * SFA_GX_WORKERS=n sets the number of worker threads (0 = the GX thread alone); SFA_GX_VERIFY=1 redoes every
 * flush on one thread and reports any difference; SFA_GX_DEFER=0 does it all inline in draw() as before. */
typedef struct {
    uint32_t bytes, count;      /* vertex bytes in g_bytes, vertex count */
    uint32_t dst;               /* first output vertex in the frame */
    uint32_t rc, xs, mx;        /* indices of the read context, XF setup and matrix snapshots */
    uint32_t scale_need;        /* texcoords to rescale (see texcoord_scale_factors) */
    float ks[8], kt[8];
    uint8_t opcode;
} VtxJob;

#define BYTES_CAP (16u << 20) /* a draw has at most 65535 vertices of at most ~130 bytes */
#define JOB_CAP 16384u
#define RC_CAP 4096u
#define XS_CAP 2048u
#define MX_CAP 4096u
static uint8_t *g_bytes;
static uint32_t g_nbytes;
static VtxJob *g_jobs;
static uint32_t g_njobs;
static ReadCtx *g_rc_snap;
static uint32_t g_nrc, g_rc_key[3];       /* (vat + 1, g_cp_serial, g_xf_serial) of the last snapshot */
static XfSetup *g_xs_snap;
static uint32_t g_nxs, g_xs_snap_serial;  /* g_xf_serial the last snapshot was taken at (0 = none) */
static uint32_t *g_mx_snap;
static uint32_t g_nmx, g_mx_snap_serial;  /* g_mtx_serial the last snapshot was taken at (0 = none) */
static uint32_t g_mtx_serial = 1;         /* bumped by writes to XF matrix memory */
static GxFrame *g_job_frame;              /* the frame the pending jobs write into */
static SDL_atomic_t g_pub, g_claim, g_done, g_sleepers; /* jobs published, taken, finished; idle workers */
static SDL_sem *g_work_sem;
#define CLAIM 8 /* jobs a thread takes at a time */

typedef struct { GxVertex *vs; RawVertex *raw; uint32_t cap; } JobBuf;

static void run_job(const VtxJob *jb, JobBuf *b) {
    if (jb->count > b->cap) {
        b->cap = jb->count < 1024 ? 1024 : jb->count;
        b->vs = realloc(b->vs, sizeof *b->vs * b->cap);
        b->raw = realloc(b->raw, sizeof *b->raw * b->cap);
    }
    const ReadCtx *rc = &g_rc_snap[jb->rc];
    const XfSetup *xs = &g_xs_snap[jb->xs];
    const uint32_t *mx = &g_mx_snap[(size_t)jb->mx * MTX_WORDS];
    const uint8_t *s = g_bytes + jb->bytes;
    uint32_t size = (uint32_t)rc->f.size;
    for (uint32_t i = 0; i < jb->count; i++) {
        read_vertex(rc, s + i * size, &b->raw[i]);
        transform(xs, mx, mx + MTX_PT_WORDS, &b->raw[i], &b->vs[i]);
    }
    for (uint32_t c = 0; jb->scale_need && c < 8; c++) {
        if (!(jb->scale_need & (1u << c))) continue;
        for (uint32_t i = 0; i < jb->count; i++) { b->vs[i].uv[c][0] *= jb->ks[c]; b->vs[i].uv[c][1] *= jb->kt[c]; }
    }
    prim_expand(jb->opcode, jb->count, b->vs, g_job_frame->verts + jb->dst);
}

/* Takes and runs published jobs until none are left; returns whether it ran any. */
static int work_some(JobBuf *b) {
    int ran = 0;
    for (;;) {
        int c = SDL_AtomicGet(&g_claim), p = SDL_AtomicGet(&g_pub);
        if (c >= p) return ran;
        int n = p - c < CLAIM ? p - c : CLAIM;
        if (!SDL_AtomicCAS(&g_claim, c, c + n)) continue;
        for (int j = c; j < c + n; j++) run_job(&g_jobs[j], b);
        SDL_AtomicAdd(&g_done, n);
        ran = 1;
    }
}

static int gx_worker(void *arg) {
    (void)arg;
    JobBuf b = {0};
    for (;;) {
        if (work_some(&b)) continue;
        int spins = 0;
        while (SDL_AtomicGet(&g_claim) >= SDL_AtomicGet(&g_pub) && spins++ < 2000) SDL_CPUPauseInstruction();
        if (SDL_AtomicGet(&g_claim) >= SDL_AtomicGet(&g_pub)) {
            SDL_AtomicIncRef(&g_sleepers);
            if (SDL_AtomicGet(&g_claim) >= SDL_AtomicGet(&g_pub)) SDL_SemWaitTimeout(g_work_sem, 2);
            SDL_AtomicDecRef(&g_sleepers);
        }
    }
    return 0;
}

static void wake_workers(void) {
    int s = SDL_AtomicGet(&g_sleepers);
    if (s > 0 && (int)SDL_SemValue(g_work_sem) < s) SDL_SemPost(g_work_sem);
}

/* Makes the jobs so far available to the workers (in groups, to keep the atomics off the hot path). */
static void publish_jobs(int force) {
    int pub = SDL_AtomicGet(&g_pub);
    if (!force && (int)g_njobs - pub < CLAIM) return;
    SDL_AtomicSet(&g_pub, (int)g_njobs);
    if (g_nworkers && (int)g_njobs - SDL_AtomicGet(&g_claim) >= 2 * CLAIM) wake_workers();
}

static JobBuf g_gx_buf; /* the GX thread's own buffers */

/* Waits until every job so far has run (helping with them). Afterwards f->verts may move, and the game
 * may be told the GPU is past these draws. */
static void gx_drain(void) {
    if (!g_njobs) return;
    publish_jobs(1);
    for (int k = 0; k < g_nworkers; k++) wake_workers();
    work_some(&g_gx_buf);
    while (SDL_AtomicGet(&g_done) < (int)g_njobs) SDL_CPUPauseInstruction();
}

/* Finishes every pending vertex and starts the job arrays over. Call before anything reads f->verts. */
static void gx_flush_vertices(void) {
    if (!g_njobs) return;
    uint64_t t0 = ticks();
    gx_drain();
    static int verify = -1;
    if (verify < 0) verify = getenv("SFA_GX_VERIFY") != NULL;
    if (verify) { /* redo on this thread and compare */
        GxFrame *f = g_job_frame;
        uint32_t lo = g_jobs[0].dst, hi = f->nverts;
        GxVertex *copy = malloc(sizeof *copy * (hi - lo + 1));
        memcpy(copy, f->verts + lo, sizeof *copy * (hi - lo));
        for (uint32_t j = 0; j < g_njobs; j++) run_job(&g_jobs[j], &g_gx_buf);
        static unsigned long checked, bad, next_report;
        checked += hi - lo;
        if (memcmp(copy, f->verts + lo, sizeof *copy * (hi - lo))) bad++;
        if (bad || checked >= next_report) {
            printf("[gx] verify: %lu vertices checked, %lu flushes differed\n", checked, bad);
            next_report = checked + 10000000;
        }
        free(copy);
    }
    /* start over: hide the old jobs from the workers before resetting what they take from */
    SDL_AtomicSet(&g_pub, 0);
    SDL_AtomicSet(&g_claim, 0);
    SDL_AtomicSet(&g_done, 0);
    g_njobs = g_nbytes = g_nrc = g_nxs = g_nmx = 0;
    g_rc_key[0] = 0;
    g_xs_snap_serial = g_mx_snap_serial = 0; /* the next draw snapshots again */
    g_job_frame = NULL;
    if (t0) g_t_xform += ticks() - t0;
}

/* Copies n vertex bytes from guest address p (RAM, or the write-gather staging buffer) to dst. */
static void copy_vertex_bytes(uint8_t *dst, uint32_t p, uint32_t n) {
    if (!n) return;
    uint8_t tmp[1];
    if (p - STAGE_BASE < 0x10000000u || (ppc_is_ram(p) && ppc_is_ram(p + n - 1))) {
        const uint8_t *src = vspan(p, n, tmp);
        if (src != tmp) { memcpy(dst, src, n); return; }
    }
    for (uint32_t i = 0; i < n; i++) dst[i] = vrd8(p + i);
}

static void gx_workers_init(void) {
    g_bytes = malloc(BYTES_CAP);
    g_jobs = malloc(sizeof *g_jobs * JOB_CAP);
    g_rc_snap = malloc(sizeof *g_rc_snap * RC_CAP);
    g_xs_snap = malloc(sizeof *g_xs_snap * XS_CAP);
    g_mx_snap = malloc(sizeof *g_mx_snap * MTX_WORDS * MX_CAP);
    if (!g_bytes || !g_jobs || !g_rc_snap || !g_xs_snap || !g_mx_snap) rt_fatal("out of memory for the vertex job arrays");
    int n = SDL_GetCPUCount() - 3; /* leave the game, GX and render threads a core each */
    const char *e = getenv("SFA_GX_WORKERS");
    if (e) n = atoi(e);
    if (n < 0) n = 0;
    if (n > 4 && !e) n = 4;
    if (n > 8) n = 8;
    g_work_sem = SDL_CreateSemaphore(0);
    for (int k = 0; k < n && g_work_sem; k++) {
        if (!SDL_CreateThread(gx_worker, "gx-vtx", NULL)) break;
        g_nworkers = k + 1;
    }
    printf("[gx] vertex work on %d worker thread%s and the GX thread\n", g_nworkers, g_nworkers == 1 ? "" : "s");
}

static void draw(uint8_t opcode, uint32_t p, uint32_t count) {
    debug_draw_config();
    if (g_dbg) { /* indirect texturing usage: GEN_MODE bits 16-18 = indirect stages; BP 0x10+ per TEV stage */
        uint32_t nind = (bp[0x00] >> 16) & 7, nst = ((bp[0x00] >> 10) & 15) + 1, cmd_stages = 0;
        for (uint32_t i = 0; i < nst; i++) if (bp[0x10 + i] & 0x1FFFFF) cmd_stages++;
        static unsigned long draws, ind_draws, last_frame = ~0ul;
        static uint32_t seen[64];
        static unsigned nseen;
        draws++;
        if (nind || cmd_stages) {
            ind_draws++;
            uint32_t key = nind << 28 ^ bp[0x10] ^ bp[0x27] << 4 ^ bp[0x0E] * 31;
            int known = 0;
            for (unsigned k = 0; k < nseen; k++) known |= seen[k] == key;
            if (!known && nseen < 64) {
                seen[nseen++] = key;
                printf("[gx-ind] frame %u: %u indirect stages, %u TEV stages use them; IND_CMD0 %06X IREF %06X "
                       "IMTX0 %06X/%06X/%06X SS0 %06X\n", g_frame_number, nind, cmd_stages, bp[0x10], bp[0x27],
                       bp[0x06], bp[0x07], bp[0x08], bp[0x25]);
            }
        }
        if (g_frame_number != last_frame && g_frame_number % 300 == 0) {
            last_frame = g_frame_number;
            printf("[gx-ind] by frame %u: %lu of %lu draws use indirect texturing\n", g_frame_number, ind_draws, draws);
        }
    }
    uint64_t t0 = ticks();
    static VtxFormat fmt_cache[8];
    static uint32_t fmt_serial[8];
    int vat = opcode & 7;
    if (fmt_serial[vat] != g_cp_serial) { vtx_format(vat, &fmt_cache[vat]); fmt_serial[vat] = g_cp_serial; }
    const VtxFormat *fmtp = &fmt_cache[vat];
    if (!g_state_valid) {
        current_state(&g_state);
        g_state_serial++;
        g_state_valid = 1;
    }
    uint64_t t1 = ticks();
    uint32_t size = (uint32_t)fmtp->size;
    GxPrimKind kind;
    uint32_t n = prim_out_count(opcode, count, &kind);
    if (!n) return; /* nothing to draw (the caller skips the vertex data) */
    GxFrame *f = frame();
    static int defer = -1;
    if (defer < 0) defer = !getenv("SFA_GX_DEFER") || atoi(getenv("SFA_GX_DEFER")) != 0;
    if (!defer) { /* SFA_GX_DEFER=0: read and transform right here on the GX thread, as before (for comparison) */
        static GxVertex *vs;
        static RawVertex *raw;
        static uint32_t cap;
        static XfSetup xs;
        static uint32_t xs_serial;
        if (count > cap) {
            cap = count + 64;
            vs = realloc(vs, sizeof *vs * cap);
            raw = realloc(raw, sizeof *raw * cap);
        }
        ReadCtx rc;
        read_ctx(&rc, fmtp);
        for (uint32_t i = 0; i < count; i++) {
            uint8_t sbuf[256];
            read_vertex(&rc, vspan(p + i * size, size, sbuf), &raw[i]);
            if (i == 0 && xs_serial != g_xf_serial) { xf_setup(&xs, &raw[0]); xs_serial = g_xf_serial; }
            transform(&xs, xf, &xf[0x400], &raw[i], &vs[i]);
        }
        float ks[8], kt[8];
        uint32_t need = texcoord_scale_factors(ks, kt);
        for (uint32_t c = 0; need && c < 8; c++) {
            if (!(need & (1u << c))) continue;
            for (uint32_t i = 0; i < count; i++) { vs[i].uv[c][0] *= ks[c]; vs[i].uv[c][1] *= kt[c]; }
        }
        uint64_t t2 = ticks();
        g_t_state += t1 - t0;
        g_t_vtx += t2 - t1;
        g_t_calls++;
        g_t_verts += count;
        uint32_t dst = emit(kind, &g_state, g_state_serial, n);
        prim_expand(opcode, count, vs, f->verts + dst);
        g_t_emit += ticks() - t2;
        g_stat_draws++;
        return;
    }
    if (g_job_frame && g_job_frame != f) gx_flush_vertices();
    /* room in the fixed-size job arrays (workers read them, so they can't move) */
    if (g_nbytes + count * size > BYTES_CAP || g_njobs == JOB_CAP || g_nrc == RC_CAP || g_nxs == XS_CAP ||
        g_nmx == MX_CAP)
        gx_flush_vertices();
    g_job_frame = f;
    /* snapshot what reading and transforming the vertices needs, when it has changed */
    if (g_rc_key[0] != (uint32_t)vat + 1 || g_rc_key[1] != g_cp_serial || g_rc_key[2] != g_xf_serial) {
        read_ctx(&g_rc_snap[g_nrc++], fmtp);
        g_rc_key[0] = (uint32_t)vat + 1;
        g_rc_key[1] = g_cp_serial;
        g_rc_key[2] = g_xf_serial;
    }
    if (g_xs_snap_serial != g_xf_serial) {
        xf_setup(&g_xs_snap[g_nxs++], NULL);
        g_xs_snap_serial = g_xf_serial;
    }
    if (g_mx_snap_serial != g_mtx_serial) {
        uint32_t *m = &g_mx_snap[(size_t)g_nmx++ * MTX_WORDS];
        memcpy(m, &xf[0], MTX_PT_WORDS * 4);
        memcpy(m + MTX_PT_WORDS, &xf[0x400], MTX_NRM_WORDS * 4);
        g_mx_snap_serial = g_mtx_serial;
    }
    VtxJob *jb = &g_jobs[g_njobs]; /* counted (and published) once emit() has placed it */
    jb->bytes = g_nbytes;
    jb->count = count;
    jb->rc = g_nrc - 1;
    jb->xs = g_nxs - 1;
    jb->mx = g_nmx - 1;
    jb->opcode = opcode;
    jb->scale_need = texcoord_scale_factors(jb->ks, jb->kt);
    copy_vertex_bytes(g_bytes + g_nbytes, p, count * size);
    g_nbytes += count * size;
    uint64_t t2 = ticks();
    g_t_state += t1 - t0;
    g_t_vtx += t2 - t1;
    g_t_calls++;
    g_t_verts += count;
    jb->dst = emit(kind, &g_state, g_state_serial, n);
    g_njobs++;
    publish_jobs(0);
    g_t_emit += ticks() - t2;
    g_stat_draws++;
}

/* ---- BP side effects ----------------------------------------------------------------------- */
/* Encode read-back EFB pixels (RGBA8, top row first) into a GX texture in guest memory, tiled the
 * way the texture decoder expects. fmt is the copy format: 0 R4/I4, 1 R8/I8, 2 RA4/IA4, 3 RA8/IA8,
 * 4 RGB565, 5 RGB5A3, 6 RGBA8, 7 A8, 8 R8, 9 G8, 10 B8, 11 RG8, 12 GB8. */
static void encode_copy(const uint8_t *px, uint32_t w, uint32_t h, uint32_t fmt, int intensity, uint32_t dst) {
    uint32_t tw, th, tb, tf;
    static const uint32_t as_tex[13] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3}; /* texture layout per copy format */
    tf = fmt < 13 ? as_tex[fmt] : 6;
    tex_tile(tf, &tw, &th, &tb);
    for (uint32_t ty = 0; ty < h; ty += th)
        for (uint32_t tx = 0; tx < w; tx += tw, dst += tb) {
            uint8_t blk[64] = {0};
            for (uint32_t yy = 0; yy < th; yy++)
                for (uint32_t xx = 0; xx < tw; xx++) {
                    uint32_t sx = tx + xx < w ? tx + xx : w - 1, sy = ty + yy < h ? ty + yy : h - 1;
                    const uint8_t *c = px + ((size_t)sy * w + sx) * 4;
                    uint8_t r = c[0], g = c[1], b = c[2], a = c[3];
                    uint8_t I = intensity ? (uint8_t)((66 * r + 129 * g + 25 * b + 128) / 256 + 16) : r;
                    uint32_t i = yy * tw + xx;
                    switch (fmt) {
                    case 0: blk[i / 2] |= (uint8_t)((I >> 4) << ((i & 1) ? 0 : 4)); break;
                    case 1: case 8: blk[i] = I; break;
                    case 7: blk[i] = a; break;
                    case 9: blk[i] = g; break;
                    case 10: blk[i] = b; break;
                    case 2: blk[i] = (uint8_t)((a & 0xF0) | (I >> 4)); break;
                    case 3: blk[i * 2] = a; blk[i * 2 + 1] = I; break;
                    case 11: blk[i * 2] = g; blk[i * 2 + 1] = r; break;
                    case 12: blk[i * 2] = b; blk[i * 2 + 1] = g; break;
                    case 4: { uint16_t v16 = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
                              blk[i * 2] = (uint8_t)(v16 >> 8); blk[i * 2 + 1] = (uint8_t)v16; break; }
                    case 5: { uint16_t v16 = a >= 0xE0 ? (uint16_t)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3)
                                                       : (uint16_t)((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4);
                              blk[i * 2] = (uint8_t)(v16 >> 8); blk[i * 2 + 1] = (uint8_t)v16; break; }
                    default: blk[i * 2] = a; blk[i * 2 + 1] = r; blk[32 + i * 2] = g; blk[32 + i * 2 + 1] = b; break;
                    }
                }
            for (uint32_t k = 0; k < tb; k++) wr8(dst + k, blk[k]);
        }
}

/* EFB copy to texture, on the GPU: a marker in the frame where the renderer converts the rectangle
 * into a texture of its own, and an entry that makes texture lookups at the destination use it. No
 * waiting: reading the pixels back stalled the CPU and GPU once or twice a frame (Mission 1's comm
 * portrait and an effect are copies, made every frame). */
static void efb_copy_gpu(uint32_t v, int x, int y, int w, int h) {
    static const uint32_t as_tex[13] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3}; /* texture layout per copy format */
    uint32_t fmt = ((v >> 4) & 7) + ((v >> 3) & 1) * 8, half = (v >> 9) & 1, intensity = (v >> 15) & 1;
    uint32_t dst = 0x80000000u | ((bp[0x4B] & 0xFFFFFF) << 5);
    uint32_t ow = half ? (uint32_t)w / 2 : (uint32_t)w, oh = half ? (uint32_t)h / 2 : (uint32_t)h;
    uint32_t size = tex_size(fmt < 13 ? as_tex[fmt] : 6, ow, oh);
    if (!ppc_is_ram(dst) || !ow || !oh || (dst & 0x01FFFFFF) + size > PPC_MEM_SIZE) return;
    GpuCopy *c = NULL;
    for (int i = 0; i < GPU_COPIES && !c; i++)
        if (g_gpu_copies[i].id && g_gpu_copies[i].addr == dst) c = &g_gpu_copies[i];
    if (!c) {
        static int next_evict;
        for (int i = 0; i < GPU_COPIES && !c; i++)
            if (!g_gpu_copies[i].id) c = &g_gpu_copies[i];
        if (!c) { c = &g_gpu_copies[next_evict]; next_evict = (next_evict + 1) % GPU_COPIES; g_ngpu_copies--; }
        c->addr = dst;
        c->id = g_next_tex_id++;
        g_ngpu_copies++;
    }
    c->size = size;
    c->ram_hash = fnv(ppc_ram(dst), size, 2166136261u);
    c->checked_frame = g_frame_number;
    GxFrame *f = frame();
    GROW(f->efb_copies, f->nefb_copies, f->cap_efb_copies, 8);
    GxEfbCopy *e = &f->efb_copies[f->nefb_copies++];
    e->at_batch = f->nbatches;
    e->seq = f->nevents++;
    e->id = c->id;
    e->x = (uint16_t)x; e->y = (uint16_t)y; e->w = (uint16_t)w; e->h = (uint16_t)h;
    e->out_w = (uint16_t)ow; e->out_h = (uint16_t)oh;
    e->fmt = (uint8_t)fmt; e->half = (uint8_t)half; e->intensity = (uint8_t)intensity;
    g_t_ncopy++;
    g_state_valid = 0; /* texture lookups at dst now give the copy */
}

/* EFB copy to texture with SFA_EFB_READBACK: render everything drawn so far (a frame segment), read
 * the rectangle back and write it to guest memory in the requested format. Slow (the CPU waits for the
 * GPU), but the guest can read the result. */
static void efb_copy_to_texture(uint32_t v, int x, int y, int w, int h) {
    static int readback = -1;
    if (readback < 0) readback = getenv("SFA_EFB_READBACK") != NULL;
    if (!readback) { efb_copy_gpu(v, x, y, w, h); return; }
    gx_flush_vertices(); /* the renderer draws this segment now */
    GxFrame *f = frame();
    GxCopyRequest req = {(uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, malloc((size_t)w * h * 4), 0};
    f->copy = &req;
    g_frame = NULL;
    g_state_valid = 0;
    uint64_t tc0 = ticks();
    video_copy_segment(f);
    g_t_copy += ticks() - tc0;
    g_t_ncopy++;
    uint32_t fmt = ((v >> 4) & 7) + ((v >> 3) & 1) * 8, half = (v >> 9) & 1, intensity = (v >> 15) & 1;
    uint32_t dst = 0x80000000u | ((bp[0x4B] & 0xFFFFFF) << 5);
    uint32_t ow = (uint32_t)w, oh = (uint32_t)h;
    uint8_t *px = req.rgba;
    if (half) { /* 2x2 box filter */
        ow = (uint32_t)w / 2; oh = (uint32_t)h / 2;
        for (uint32_t yy = 0; yy < oh; yy++)
            for (uint32_t xx = 0; xx < ow; xx++)
                for (int k = 0; k < 4; k++) {
                    const uint8_t *s0 = req.rgba + ((size_t)(yy * 2) * (uint32_t)w + xx * 2) * 4;
                    const uint8_t *s1 = s0 + (size_t)w * 4;
                    px[((size_t)yy * ow + xx) * 4 + k] = (uint8_t)((s0[k] + s0[4 + k] + s1[k] + s1[4 + k] + 2) / 4);
                }
    }
    if (ppc_is_ram(dst) && ow && oh) encode_copy(px, ow, oh, fmt, (int)intensity, dst);
    if (g_dbg && g_frame_number % 120 == 0) { /* save what was copied, for inspection */
        char path[256];
        snprintf(path, sizeof path, "build/GF7E01/textures/copy_%05u_%08X_f%u_%ux%u.bmp", g_frame_number, dst, fmt, ow, oh);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            uint32_t sz = ow * oh * 4, t;
            uint8_t hdr[54] = {'B', 'M'};
            t = 54 + sz; memcpy(hdr + 2, &t, 4);
            t = 54; memcpy(hdr + 10, &t, 4);
            t = 40; memcpy(hdr + 14, &t, 4);
            memcpy(hdr + 18, &ow, 4);
            int32_t nh = -(int32_t)oh; memcpy(hdr + 22, &nh, 4);
            hdr[26] = 1; hdr[28] = 32;
            fwrite(hdr, 1, 54, fp);
            for (uint32_t i = 0; i < ow * oh; i++) { uint8_t b4[4] = {px[i*4+2], px[i*4+1], px[i*4], 255}; fwrite(b4, 1, 4, fp); }
            fclose(fp);
        }
    }
    free(req.rgba);
}

static void efb_copy(uint32_t v) {
    uint32_t tl = bp[0x49], wh = bp[0x4A];
    int x = (int)(tl & 0x3FF), y = (int)((tl >> 10) & 0x3FF), w = (int)(wh & 0x3FF) + 1, h = (int)((wh >> 10) & 0x3FF) + 1;
    int to_xfb = (v >> 14) & 1, clear = (v >> 11) & 1;
    if (!to_xfb && g_dbg)
        printf("[gx-copy] frame %u EFB (%d,%d %dx%d) -> texture %08X fmt %u half %u\n", g_frame_number, x, y, w, h,
               0x80000000u | ((bp[0x4B] & 0xFFFFFF) << 5), ((v >> 4) & 7) + ((v >> 3) & 1) * 8, (v >> 9) & 1);
    if (!to_xfb) efb_copy_to_texture(v, x, y, w, h);
    if (to_xfb) {
        gx_flush_vertices(); /* the frame goes to the renderer: its vertices must be done */
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
        cl->seq = f->nevents++;
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
    uint32_t old = bp[reg];
    bp[reg] = (bp[reg] & ~bp_mask) | (val & bp_mask);
    /* The game rewrites most registers before every draw, usually with the same value: only a change
     * (or a register that acts as a command, like a TLUT load or the TEV color writes) needs a new
     * render state. */
    if (bp[reg] != old || reg == 0x52 || (reg >= 0x63 && reg <= 0x67) || (reg >= 0xE0 && reg <= 0xE7))
        g_state_valid = 0;
    bp_mask = 0xFFFFFF;
    switch (reg) {
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        tev_color_write(reg, val);
        break;
    case 0x47: case 0x48: /* PE token: GXReadDrawSync reads it back from PE_TOKEN */
        gx_drain(); /* the game may reuse vertex arrays once it sees the token: workers must be done with them */
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
        case 0x08: {
            uint32_t v = be32(c + 2);
            if (cp[c[1]] != v) { cp[c[1]] = v; g_cp_serial++; } /* vertex formats are cached by this serial */
            break;
        }
        case 0x10: {
            uint32_t hdr = be32(c + 1), addr = hdr & 0xFFFF, cnt = ((hdr >> 16) & 15) + 1;
            for (uint32_t k = 0; k < cnt; k++)
                if (addr + k < 0x1100) xf[addr + k] = be32(c + 5 + 4 * k);
            if (addr < 0x468) g_mtx_serial++; /* matrices: the next draw snapshots them */
            if (addr + cnt > 0x600) g_xf_serial++; /* lights or registers: per-draw setup changes */
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: {
            uint32_t v = be32(c + 1), arr = 12 + ((op - 0x20) >> 3), index = v >> 16, len = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
            uint32_t src = 0x80000000u | ((cp[0xA0 + arr] & 0x03FFFFFF) + index * (cp[0xB0 + arr] & 0xFF));
            for (uint32_t k = 0; k < len; k++) xf[(addr + k) & 0xFFF] = rd32(src + 4 * k);
            if (addr < 0x468) g_mtx_serial++;
            if (addr + len > 0x600) g_xf_serial++;
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

/* Append bytes to the parse buffer and execute every complete command in it. */
static void fifo_consume(const uint8_t *bytes, size_t n) {
    if (g_fifo_len + n > g_fifo_cap) {
        while (g_fifo_len + n > g_fifo_cap) g_fifo_cap = g_fifo_cap ? g_fifo_cap * 2 : 1 << 16;
        g_fifo = realloc(g_fifo, g_fifo_cap);
    }
    memcpy(g_fifo + g_fifo_len, bytes, n);
    g_fifo_len += n;
    size_t used = run_commands(g_fifo, g_fifo_len, 0, 0);
    if (used) {
        memmove(g_fifo, g_fifo + used, g_fifo_len - used);
        g_fifo_len -= used;
    }
}

/* ---- GX thread -------------------------------------------------------------------------------
 * Like the real GPU, the commands run on their own thread: the game's thread only appends pipe
 * bytes to a ring (and records display lists, which must happen in program order), and the GX
 * thread parses and executes them: vertex transform, batching, EFB copies (waiting for the
 * renderer) and frame submission. The game observes GX progress only through the PE token
 * register, which it polls, and GXDrawDone, which waits until everything queued so far has run.
 * SFA_GX_SYNC=1 runs the commands on the game's thread instead (the old behaviour). */
#define GX_RING (8u << 20)
static uint8_t *g_ring;
static SDL_atomic_t g_ring_w, g_ring_r, g_ring_done; /* bytes written, taken, fully executed */
static SDL_atomic_t g_gx_sleeping, g_sync_wanted; /* GX thread asleep; GXDrawDone waiting */
static SDL_sem *g_gx_wake;
static int g_gx_threaded;
static uint32_t g_ring_wlocal; /* producer's private copy of g_ring_w */

static int gx_thread(void *arg) {
    (void)arg;
    static uint8_t chunk[1 << 16];
    uint32_t r = 0;
    for (;;) {
        uint32_t w = (uint32_t)SDL_AtomicGet(&g_ring_w);
        if (w == r && (uint32_t)SDL_AtomicGet(&g_ring_done) != r) {
            /* all taken: finish the vertex jobs, then report everything as executed (GXDrawDone waits for it) */
            gx_drain();
            SDL_AtomicSet(&g_ring_done, (int)r);
            continue;
        }
        if (w == r) {
            /* nothing queued: spin briefly (the game writes in bursts), then sleep until woken */
            int spins = 0;
            while ((uint32_t)SDL_AtomicGet(&g_ring_w) == r && spins++ < 2000) SDL_CPUPauseInstruction();
            if ((uint32_t)SDL_AtomicGet(&g_ring_w) == r) {
                SDL_AtomicSet(&g_gx_sleeping, 1);
                if ((uint32_t)SDL_AtomicGet(&g_ring_w) == r) SDL_SemWaitTimeout(g_gx_wake, 2);
                SDL_AtomicSet(&g_gx_sleeping, 0);
            }
            continue;
        }
        uint32_t n = w - r;
        if (n > sizeof chunk) n = sizeof chunk;
        for (uint32_t k = 0; k < n; k++) chunk[k] = g_ring[(r + k) & (GX_RING - 1)];
        r += n;
        SDL_AtomicSet(&g_ring_r, (int)r);
        uint64_t t0 = ticks();
        fifo_consume(chunk, n);
        if (SDL_AtomicGet(&g_sync_wanted)) { /* GXDrawDone is waiting: finish the vertex jobs and say so */
            gx_drain();
            SDL_AtomicSet(&g_ring_done, (int)r);
        }
        if (t0) g_t_gxbusy += ticks() - t0;
    }
    return 0;
}

static void gx_wake(void) {
    if (SDL_AtomicGet(&g_gx_sleeping)) SDL_SemPost(g_gx_wake);
}

/* Wait until the GX thread has executed everything queued so far (GXDrawDone). */
void gx_sync(void) {
    if (!g_gx_threaded) { gx_drain(); return; } /* commands ran here; their vertex jobs may not have */
    uint32_t target = g_ring_wlocal;
    uint64_t t0 = ticks();
    SDL_AtomicIncRef(&g_sync_wanted);
    while ((uint32_t)SDL_AtomicGet(&g_ring_done) != target) {
        gx_wake();
        SDL_Delay(0);
    }
    SDL_AtomicDecRef(&g_sync_wanted);
    if (t0) g_t_sync += ticks() - t0;
}

void gx_fifo_write(uint32_t value, int size) {
    if (recording_display_list()) {
        for (int i = size - 1; i >= 0; i--) wr8(0x80000000u | g_dl_ptr++, (uint8_t)(value >> (i * 8)));
        return;
    }
    uint8_t b[4];
    for (int i = 0; i < size; i++) b[i] = (uint8_t)(value >> ((size - 1 - i) * 8));
    if (!g_gx_threaded) {
        fifo_consume(b, (size_t)size);
        return;
    }
    uint32_t w = g_ring_wlocal;
    if (w + (uint32_t)size - (uint32_t)SDL_AtomicGet(&g_ring_r) > GX_RING) { /* ring full: let it drain */
        uint64_t t0 = ticks();
        while (w + (uint32_t)size - (uint32_t)SDL_AtomicGet(&g_ring_r) > GX_RING) {
            gx_wake();
            SDL_Delay(0);
        }
        if (t0) g_t_ringfull += ticks() - t0;
    }
    for (int i = 0; i < size; i++) g_ring[(w + (uint32_t)i) & (GX_RING - 1)] = b[i];
    g_ring_wlocal = w + (uint32_t)size;
    SDL_AtomicSet(&g_ring_w, (int)g_ring_wlocal);
    gx_wake();
}

void gx_init(void) {
    dbg_init();
    bp[0x41] = 0x18; /* color and alpha update */
    bp[0x40] = 0x17; /* z test LEQUAL with update */
    if (!getenv("SFA_GX_SYNC")) {
        g_ring = malloc(GX_RING);
        g_gx_wake = SDL_CreateSemaphore(0);
        if (g_ring && g_gx_wake && SDL_CreateThread(gx_thread, "gx", NULL)) g_gx_threaded = 1;
    }
    printf("[gx] commands run on %s\n", g_gx_threaded ? "their own thread" : "the game's thread");
    gx_workers_init();
}

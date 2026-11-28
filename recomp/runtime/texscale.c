/* Automatic texture upscaling: the 2xBR algorithm (Hyllian's xBR, level 2), written here from its
 * published description. Each source pixel becomes 2x2; where it sits on an edge between two colors,
 * the corners facing the edge are blended toward the color across it, so diagonal and curved edges
 * come out smooth instead of blocky. 4x is 2x twice. RGBA8 in and out, top row first. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "texpack.h"

/* A pixel's "weight" for comparing: luma-like (Hyllian's 14.352 R + 28.176 G + 5.472 B, scaled) plus
 * alpha, so transparent edges count as edges. */
static int32_t weight(const uint8_t *p) { return (14352 * p[0] + 28176 * p[1] + 5472 * p[2]) / 1000 + 48 * p[3]; }
#define EQ_LIMIT (15 * 255) /* "about the same color" */

static void blend(uint8_t *dst, const uint8_t *src, int amount /* of 256 */) {
    for (int k = 0; k < 4; k++) dst[k] = (uint8_t)(dst[k] + ((src[k] - dst[k]) * amount) / 256);
}

typedef struct {
    const uint8_t *px;    /* source RGBA */
    const int32_t *wt;    /* weights */
    int w, h;
} Src;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int at(const Src *s, int x, int y) { return clampi(y, 0, s->h - 1) * s->w + clampi(x, 0, s->w - 1); }

/* One corner of the 2x2 output of pixel E. The names follow the 5x5 neighbourhood
 *        A1 B1 C1
 *     A0 A  B  C  C4
 *     D0 D  E  F  F4
 *     G0 G  H  I  I4
 *        G5 H5 I5
 * rotated so that the corner being filled (out[n3]) faces I. */
static void corner(const Src *s, uint8_t out[4][4], int E, int I, int H, int F, int G, int C, int D, int B, int H5,
                   int F4, int I4, int I5, int n1, int n2, int n3) {
    const int32_t *w = s->wt;
    const uint8_t *p = s->px;
#define DF(a, b) abs(w[a] - w[b])
#define EQ(a, b) (DF(a, b) < EQ_LIMIT)
#define SAME(a, b) (!memcmp(p + (a) * 4, p + (b) * 4, 4))
    if (SAME(E, H) || SAME(E, F)) return;
    int e = DF(E, C) + DF(E, G) + DF(I, H5) + DF(I, F4) + (DF(H, F) << 2);
    int i = DF(H, D) + DF(H, I5) + DF(F, I4) + DF(F, B) + (DF(E, I) << 2);
    const uint8_t *px = p + (DF(E, F) <= DF(E, H) ? F : H) * 4;
    if (e < i && ((!EQ(F, B) && !EQ(H, D)) || (EQ(E, I) && !EQ(F, I4) && !EQ(H, I5)) || EQ(E, G) || EQ(E, C))) {
        int ke = DF(F, G), ki = DF(H, C);
        int ex2 = !SAME(E, C) && !SAME(B, C), ex3 = !SAME(E, G) && !SAME(D, G);
        if ((ke << 1) <= ki && ex3 && ke >= (ki << 1) && ex2) { /* both shallow and steep: a wide corner */
            blend(out[n3], px, 224);
            blend(out[n2], px, 64);
            memcpy(out[n1], out[n2], 4);
        } else if ((ke << 1) <= ki && ex3) { /* a shallow edge: reaches the neighbouring corner too */
            blend(out[n3], px, 192);
            blend(out[n2], px, 64);
        } else if (ke >= (ki << 1) && ex2) { /* a steep edge */
            blend(out[n3], px, 192);
            blend(out[n1], px, 64);
        } else { /* a 45-degree edge */
            blend(out[n3], px, 128);
        }
    } else if (e <= i) {
        blend(out[n3], px, 128);
    }
#undef DF
#undef EQ
#undef SAME
}

static uint8_t *scale2x(const uint8_t *src, int w, int h) {
    int32_t *wt = malloc(sizeof *wt * (size_t)w * h);
    uint8_t *dst = malloc((size_t)w * h * 16);
    if (!wt || !dst) { free(wt); free(dst); return NULL; }
    for (int k = 0; k < w * h; k++) wt[k] = weight(src + k * 4);
    Src s = {src, wt, w, h};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
#define P(dx, dy) at(&s, x + (dx), y + (dy))
            int A = P(-1, -1), B = P(0, -1), C = P(1, -1), D = P(-1, 0), E = P(0, 0), F = P(1, 0);
            int G = P(-1, 1), H = P(0, 1), I = P(1, 1);
            int A1 = P(-1, -2), B1 = P(0, -2), C1 = P(1, -2), A0 = P(-2, -1), C4 = P(2, -1), D0 = P(-2, 0);
            int F4 = P(2, 0), G0 = P(-2, 1), I4 = P(2, 1), G5 = P(-1, 2), H5 = P(0, 2), I5 = P(1, 2);
#undef P
            uint8_t out[4][4]; /* 0 top left, 1 top right, 2 bottom left, 3 bottom right */
            for (int k = 0; k < 4; k++) memcpy(out[k], src + E * 4, 4);
            corner(&s, out, E, I, H, F, G, C, D, B, H5, F4, I4, I5, 1, 2, 3);      /* bottom right */
            corner(&s, out, E, C, F, B, I, A, H, D, F4, B1, C1, C4, 0, 3, 1);      /* top right */
            corner(&s, out, E, A, B, D, C, G, F, H, B1, D0, A0, A1, 2, 1, 0);      /* top left */
            corner(&s, out, E, G, D, H, A, I, B, F, D0, H5, G5, G0, 3, 0, 2);      /* bottom left */
            uint8_t *o = dst + ((size_t)(y * 2) * (w * 2) + x * 2) * 4;
            memcpy(o, out[0], 4);
            memcpy(o + 4, out[1], 4);
            memcpy(o + (size_t)w * 8, out[2], 4);
            memcpy(o + (size_t)w * 8 + 4, out[3], 4);
        }
    free(wt);
    return dst;
}

uint8_t *texscale(const uint8_t *rgba, uint32_t w, uint32_t h, int factor, uint32_t *ow, uint32_t *oh) {
    uint8_t *cur = NULL;
    const uint8_t *in = rgba;
    for (int f = 1; f < factor; f *= 2) {
        uint8_t *next = scale2x(in, (int)w, (int)h);
        free(cur);
        if (!next) return NULL;
        cur = next;
        in = cur;
        w *= 2; h *= 2;
    }
    *ow = w; *oh = h;
    return cur;
}

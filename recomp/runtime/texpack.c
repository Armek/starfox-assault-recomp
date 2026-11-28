/* HD texture packs (see texpack.h): Dolphin texture names, a PNG reader/writer with its own inflate,
 * and the index of a pack folder. */
#include "texpack.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <dirent.h>
#define mkdir_one(p) mkdir((p), 0755)
#endif

#define PACK_DIR "user/GF7E01/textures"
#define DUMP_DIR "user/GF7E01/dump/textures"

/* ---- XXH64 (the hash Dolphin names textures by) ----------------------------------------------- */
#define P1 11400714785074694791ULL
#define P2 14029467366897019727ULL
#define P3 1609587929392839161ULL
#define P4 9650029242287828579ULL
#define P5 2870177450012600261ULL
static uint64_t rotl(uint64_t x, int r) { return x << r | x >> (64 - r); }
static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t xround(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
static uint64_t xmerge(uint64_t acc, uint64_t v) { return (acc ^ xround(0, v)) * P1 + P4; }

uint64_t xxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = data, *end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        for (; p + 32 <= end; p += 32) {
            v1 = xround(v1, le64(p));
            v2 = xround(v2, le64(p + 8));
            v3 = xround(v3, le64(p + 16));
            v4 = xround(v4, le64(p + 24));
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xmerge(xmerge(xmerge(xmerge(h, v1), v2), v3), v4);
    } else {
        h = seed + P5;
    }
    h += len;
    for (; p + 8 <= end; p += 8) h = rotl(h ^ xround(0, le64(p)), 27) * P1 + P4;
    if (p + 4 <= end) { h = rotl(h ^ (uint64_t)le32(p) * P1, 23) * P2 + P3; p += 4; }
    for (; p < end; p++) h = rotl(h ^ *p * P5, 11) * P1;
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

void texpack_name(char *out, size_t cap, uint32_t w, uint32_t h, int mipmapped, uint32_t fmt, const uint8_t *data,
                  size_t size, const uint8_t *tlut, size_t tlut_bytes) {
    char tl[24] = "";
    if (tlut && (fmt == 8 || fmt == 9 || fmt == 10)) {
        /* Dolphin hashes only the palette entries the texture uses (lowest to highest index) */
        uint32_t lo = 0xFFFF, hi = 0;
        if (fmt == 8)
            for (size_t i = 0; i < size; i++) {
                uint32_t a = data[i] >> 4, b = data[i] & 15;
                lo = a < lo ? a : lo; lo = b < lo ? b : lo; hi = a > hi ? a : hi; hi = b > hi ? b : hi;
            }
        else if (fmt == 9)
            for (size_t i = 0; i < size; i++) { if (data[i] < lo) lo = data[i]; if (data[i] > hi) hi = data[i]; }
        else
            for (size_t i = 0; i + 1 < size; i += 2) {
                uint32_t v = (uint32_t)(data[i] << 8 | data[i + 1]) & 0x3FFF;
                lo = v < lo ? v : lo; hi = v > hi ? v : hi;
            }
        if (lo > hi) lo = hi = 0;
        size_t off = (size_t)lo * 2, n = (size_t)(hi + 1 - lo) * 2;
        if (off > tlut_bytes) off = tlut_bytes;
        if (off + n > tlut_bytes) n = tlut_bytes - off;
        snprintf(tl, sizeof tl, "_%016llx", (unsigned long long)xxh64(tlut + off, n, 0));
    }
    snprintf(out, cap, "tex1_%ux%u%s_%016llx%s_%u", w, h, mipmapped ? "_m" : "",
             (unsigned long long)xxh64(data, size, 0), tl, fmt);
}

/* ---- inflate (RFC 1951) ----------------------------------------------------------------------- */
typedef struct {
    const uint8_t *in;
    size_t inlen, inpos;
    uint32_t bitbuf;
    int bitcnt, err;
    uint8_t *out;
    size_t outlen, outcap;
} Inf;
typedef struct { short count[16], symbol[288]; } Huff;

static int getbits(Inf *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}
static int put(Inf *s, uint8_t b) {
    if (s->outlen == s->outcap) {
        size_t n = s->outcap ? s->outcap * 2 : 65536;
        uint8_t *p = realloc(s->out, n);
        if (!p) { s->err = 1; return 0; }
        s->out = p; s->outcap = n;
    }
    s->out[s->outlen++] = b;
    return 1;
}
static int hdecode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1;
        if (s->err) return -1;
    }
    s->err = 1;
    return -1;
}
static void hbuild(Huff *h, const short *len, int n) {
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
}
static int codes(Inf *s, const Huff *lc, const Huff *dc) {
    static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = hdecode(s, lc);
        if (s->err || sym < 0) return 0;
        if (sym < 256) { if (!put(s, (uint8_t)sym)) return 0; continue; }
        if (sym == 256) return 1;
        sym -= 257;
        if (sym >= 29) return 0;
        int len = lbase[sym] + getbits(s, lext[sym]);
        int d = hdecode(s, dc);
        if (s->err || d < 0 || d >= 30) return 0;
        size_t dist = (size_t)(dbase[d] + getbits(s, dext[d]));
        if (s->err || dist > s->outlen) return 0;
        while (len--) if (!put(s, s->out[s->outlen - dist])) return 0;
    }
}
static uint8_t *inflate_buf(const uint8_t *in, size_t inlen, size_t *outlen) {
    Inf s = {in, inlen, 0, 0, 0, 0, NULL, 0, 0};
    int last;
    do {
        last = getbits(&s, 1);
        int type = getbits(&s, 2);
        if (s.err) break;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) { s.err = 1; break; }
            size_t n = s.in[s.inpos] | s.in[s.inpos + 1] << 8;
            s.inpos += 4;
            if (s.inpos + n > s.inlen) { s.err = 1; break; }
            for (size_t i = 0; i < n; i++) put(&s, s.in[s.inpos + i]);
            s.inpos += n;
        } else if (type == 1 || type == 2) {
            static Huff flc, fdc;
            static int fixed_ready;
            Huff dlc, ddc;
            const Huff *lc = &flc, *dc = &fdc;
            short len[320];
            if (type == 1) {
                if (!fixed_ready) {
                    int i = 0;
                    for (; i < 144; i++) len[i] = 8;
                    for (; i < 256; i++) len[i] = 9;
                    for (; i < 280; i++) len[i] = 7;
                    for (; i < 288; i++) len[i] = 8;
                    hbuild(&flc, len, 288);
                    for (i = 0; i < 30; i++) len[i] = 5;
                    hbuild(&fdc, len, 30);
                    fixed_ready = 1;
                }
            } else {
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                int nlen = getbits(&s, 5) + 257, ndist = getbits(&s, 5) + 1, ncode = getbits(&s, 4) + 4;
                if (s.err || nlen > 286 || ndist > 30) { s.err = 1; break; }
                memset(len, 0, sizeof len);
                for (int i = 0; i < ncode; i++) len[order[i]] = (short)getbits(&s, 3);
                Huff cl;
                hbuild(&cl, len, 19);
                int i = 0;
                while (i < nlen + ndist && !s.err) {
                    int sym = hdecode(&s, &cl), rep = 0;
                    short v = 0;
                    if (sym < 0) { s.err = 1; break; }
                    if (sym < 16) { len[i++] = (short)sym; continue; }
                    if (sym == 16) { if (!i) { s.err = 1; break; } v = len[i - 1]; rep = 3 + getbits(&s, 2); }
                    else if (sym == 17) rep = 3 + getbits(&s, 3);
                    else rep = 11 + getbits(&s, 7);
                    if (i + rep > nlen + ndist) { s.err = 1; break; }
                    while (rep--) len[i++] = v;
                }
                if (s.err) break;
                hbuild(&dlc, len, nlen);
                hbuild(&ddc, len + nlen, ndist);
                lc = &dlc; dc = &ddc;
            }
            if (!codes(&s, lc, dc)) s.err = 1;
        } else {
            s.err = 1;
        }
    } while (!last && !s.err);
    if (s.err) { free(s.out); return NULL; }
    *outlen = s.outlen;
    return s.out;
}

/* ---- PNG --------------------------------------------------------------------------------------- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 ? malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

uint8_t *png_read(const char *path, uint32_t *ow, uint32_t *oh) {
    size_t n;
    uint8_t *file = read_file(path, &n), *idat = NULL, *raw = NULL, *rgba = NULL;
    static const uint8_t sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t plte[256][4];
    uint32_t w = 0, h = 0, depth = 0, ctype = 0, interlace = 0, npal = 0;
    size_t nidat = 0;
    int trns_gray = -1, trns_rgb[3] = {-1, -1, -1};
    if (!file || n < 8 || memcmp(file, sig, 8)) goto done;
    for (int i = 0; i < 256; i++) plte[i][0] = plte[i][1] = plte[i][2] = 0, plte[i][3] = 255;
    for (size_t p = 8; p + 12 <= n;) {
        uint32_t len = be32(file + p);
        const uint8_t *type = file + p + 4, *d = file + p + 8;
        if (len > n - p - 12) goto done;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(d); h = be32(d + 4); depth = d[8]; ctype = d[9]; interlace = d[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = len / 3 > 256 ? 256 : len / 3;
            for (uint32_t i = 0; i < npal; i++) memcpy(plte[i], d + i * 3, 3);
        } else if (!memcmp(type, "tRNS", 4)) {
            if (ctype == 3) for (uint32_t i = 0; i < len && i < 256; i++) plte[i][3] = d[i];
            else if (ctype == 0 && len >= 2) trns_gray = d[0] << 8 | d[1];
            else if (ctype == 2 && len >= 6) for (int k = 0; k < 3; k++) trns_rgb[k] = d[k * 2] << 8 | d[k * 2 + 1];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *q = realloc(idat, nidat + len);
            if (!q) goto done;
            idat = q;
            memcpy(idat + nidat, d, len);
            nidat += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        p += 12 + len;
    }
    static const int chans[7] = {1, 0, 3, 1, 2, 0, 4};
    if (!w || !h || w > 16384 || h > 16384 || ctype > 6 || !chans[ctype] || interlace || nidat < 2) {
        if (interlace) fprintf(stderr, "[texpack] %s: interlaced PNGs aren't supported; save it without interlacing\n", path);
        goto done;
    }
    if (!(depth == 8 || depth == 16 || ((ctype == 0 || ctype == 3) && (depth == 1 || depth == 2 || depth == 4)))) goto done;
    int ch = chans[ctype];
    size_t stride = ((size_t)w * ch * depth + 7) / 8, bpp = (size_t)(ch * depth + 7) / 8, rawlen;
    raw = inflate_buf(idat + 2, nidat - 2, &rawlen); /* skip the zlib header */
    if (!raw || rawlen < (stride + 1) * h) goto done;
    for (uint32_t y = 0; y < h; y++) { /* undo the row filters */
        uint8_t *row = raw + y * (stride + 1) + 1, *prev = y ? row - (stride + 1) : NULL;
        int ft = row[-1];
        for (size_t x = 0; x < stride; x++) {
            int a = x >= bpp ? row[x - bpp] : 0, b = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
            switch (ft) {
            case 1: row[x] = (uint8_t)(row[x] + a); break;
            case 2: row[x] = (uint8_t)(row[x] + b); break;
            case 3: row[x] = (uint8_t)(row[x] + ((a + b) >> 1)); break;
            case 4: {
                int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
                row[x] = (uint8_t)(row[x] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
                break;
            }
            default: break;
            }
        }
    }
    rgba = malloc((size_t)w * h * 4);
    if (!rgba) goto done;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = raw + y * (stride + 1) + 1;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;
            int v[4];
            for (int k = 0; k < ch; k++) {
                if (depth == 16) v[k] = row[(x * ch + k) * 2] << 8 | row[(x * ch + k) * 2 + 1];
                else if (depth == 8) v[k] = row[x * ch + k];
                else v[k] = (row[x * depth / 8] >> (8 - depth - (x * depth) % 8)) & ((1 << depth) - 1);
            }
            int full = depth == 16 ? 65535 : (1 << depth) - 1;
#define TO8(val) ((uint8_t)((val) * 255 / full))
            switch (ctype) {
            case 0: o[0] = o[1] = o[2] = TO8(v[0]); o[3] = v[0] == trns_gray ? 0 : 255; break;
            case 2:
                o[0] = TO8(v[0]); o[1] = TO8(v[1]); o[2] = TO8(v[2]);
                o[3] = v[0] == trns_rgb[0] && v[1] == trns_rgb[1] && v[2] == trns_rgb[2] ? 0 : 255;
                break;
            case 3: memcpy(o, plte[v[0] & 255], 4); break;
            case 4: o[0] = o[1] = o[2] = TO8(v[0]); o[3] = TO8(v[1]); break;
            default: o[0] = TO8(v[0]); o[1] = TO8(v[1]); o[2] = TO8(v[2]); o[3] = TO8(v[3]); break;
            }
#undef TO8
        }
    }
    *ow = w; *oh = h;
done:
    free(file); free(idat); free(raw);
    return rgba;
}

static uint32_t crc_table[256];
static uint32_t crc32_buf(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t len) {
    uint8_t hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(d, 1, len, f);
    uint32_t c = crc32_buf(crc32_buf(0, (const uint8_t *)type, 4), d, len);
    put32(hdr, c);
    fwrite(hdr, 1, 4, f);
}

/* PNG with "up" filtered rows in stored (uncompressed) deflate blocks: simple, and fine for dumps. */
int png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    static const uint8_t sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {0};
    put32(ihdr, w); put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6;
    chunk(f, "IHDR", ihdr, 13);
    size_t stride = (size_t)w * 4 + 1, rawlen = stride * h, nblocks = (rawlen + 65534) / 65535;
    uint8_t *raw = malloc(rawlen), *z = malloc(rawlen + nblocks * 5 + 6);
    if (!raw || !z) { free(raw); free(z); fclose(f); return 0; }
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + y * stride;
        row[0] = 0;
        memcpy(row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    for (size_t off = 0; off < rawlen || !rawlen; ) {
        size_t n = rawlen - off > 65535 ? 65535 : rawlen - off;
        z[zn++] = off + n >= rawlen ? 1 : 0;
        z[zn++] = (uint8_t)n; z[zn++] = (uint8_t)(n >> 8);
        z[zn++] = (uint8_t)~n; z[zn++] = (uint8_t)(~n >> 8);
        memcpy(z + zn, raw + off, n);
        zn += n; off += n;
        if (!rawlen) break;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawlen; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    put32(z + zn, b << 16 | a);
    zn += 4;
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    free(raw); free(z);
    int ok = !ferror(f);
    fclose(f);
    return ok;
}

/* ---- the pack index ---------------------------------------------------------------------------- */
typedef struct {
    char *name, *path; /* lowercase file name without .png; its path */
} PackEntry;
static PackEntry *g_entries;
static uint32_t g_nentries, g_cap;
static uint32_t *g_table, g_tsize; /* open addressing: entry index + 1 */
static int g_ready, g_dump;

static uint64_t name_hash(const char *s) { return xxh64(s, strlen(s), 0); }

static void add_file(const char *dir, const char *file) {
    size_t n = strlen(file);
    char lower[256];
    if (n < 9 || n >= sizeof lower) return;
    for (size_t i = 0; i <= n; i++) lower[i] = (char)tolower((unsigned char)file[i]);
    if (strncmp(lower, "tex1_", 5) || strcmp(lower + n - 4, ".png")) return;
    lower[n - 4] = 0;
    if (g_nentries == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_entries = realloc(g_entries, sizeof *g_entries * g_cap);
    }
    char *path = malloc(strlen(dir) + n + 2);
    sprintf(path, "%s/%s", dir, file);
    g_entries[g_nentries].name = malloc(n - 3);
    memcpy(g_entries[g_nentries].name, lower, n - 3);
    g_entries[g_nentries].path = path;
    g_nentries++;
}

static void scan(const char *dir, int depth) {
    if (depth > 8) return;
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof pat, "%s/*", dir);
    struct _finddata_t fd;
    intptr_t hd = _findfirst(pat, &fd);
    if (hd == -1) return;
    do {
        if (fd.name[0] == '.') continue;
        if (fd.attrib & _A_SUBDIR) {
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", dir, fd.name);
            scan(sub, depth + 1);
        } else {
            add_file(dir, fd.name);
        }
    } while (_findnext(hd, &fd) == 0);
    _findclose(hd);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(sub, &st)) continue;
        if (S_ISDIR(st.st_mode)) scan(sub, depth + 1);
        else add_file(dir, e->d_name);
    }
    closedir(d);
#endif
}

static int dump_from_cfg(void) {
    FILE *f = fopen("user/launcher.cfg", "r");
    int on = 0;
    char line[512];
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "dump_textures=", 14)) on = atoi(line + 14) != 0;
    fclose(f);
    return on;
}

void texpack_init(void) {
    if (g_ready) return;
    g_ready = 1;
    const char *e = getenv("SFA_DUMP_TEXTURES");
    g_dump = e ? atoi(e) != 0 : dump_from_cfg();
    const char *off = getenv("SFA_TEXTURE_PACK");
    if (!(off && !atoi(off))) scan(PACK_DIR, 0);
    if (g_nentries) {
        g_tsize = 1;
        while (g_tsize < g_nentries * 2) g_tsize <<= 1;
        g_table = calloc(g_tsize, sizeof *g_table);
        uint32_t dups = 0;
        for (uint32_t i = 0; i < g_nentries; i++) {
            uint32_t k = (uint32_t)name_hash(g_entries[i].name) & (g_tsize - 1);
            int dup = 0;
            while (g_table[k]) {
                if (!strcmp(g_entries[g_table[k] - 1].name, g_entries[i].name)) { dup = 1; break; }
                k = (k + 1) & (g_tsize - 1);
            }
            if (dup) dups++;
            else g_table[k] = i + 1;
        }
        printf("[texpack] %u replacement textures in %s%s\n", g_nentries - dups, PACK_DIR, dups ? " (some names twice: the first is used)" : "");
    }
    if (g_dump) {
        mkdir_one("user"); mkdir_one("user/GF7E01"); mkdir_one("user/GF7E01/dump"); mkdir_one(DUMP_DIR);
        printf("[texpack] saving the game's textures to %s\n", DUMP_DIR);
    }
    fflush(stdout);
}

int texpack_active(void) { return g_nentries || g_dump; }
int texpack_dumping(void) { return g_dump; }

static const PackEntry *lookup(const char *name) {
    if (!g_tsize) return NULL;
    for (uint32_t k = (uint32_t)name_hash(name) & (g_tsize - 1); g_table[k]; k = (k + 1) & (g_tsize - 1))
        if (!strcmp(g_entries[g_table[k] - 1].name, name)) return &g_entries[g_table[k] - 1];
    return NULL;
}

uint8_t *texpack_find(const char *name, uint32_t *w, uint32_t *h) {
    if (!g_nentries) return NULL;
    const PackEntry *e = lookup(name);
    char alt[160];
    if (!e) { /* the same texture with and without the mipmap mark */
        const char *m = strstr(name, "_m_");
        if (m && m - name < 20) snprintf(alt, sizeof alt, "%.*s%s", (int)(m - name), name, m + 2);
        else {
            const char *u = strchr(name + 5, '_');
            if (u) snprintf(alt, sizeof alt, "%.*s_m%s", (int)(u - name), name, u);
            else alt[0] = 0;
        }
        if (alt[0]) e = lookup(alt);
    }
    if (!e) { /* palette textures: "$" stands for any texture hash ("tex1_WxH[_m]_$_<palette>_<fmt>") */
        const char *p = name + 5;
        const char *a = strchr(p, '_');
        if (a && !strncmp(a, "_m_", 3)) a += 2;
        const char *b = a ? strchr(a + 1, '_') : NULL, *c = b ? strchr(b + 1, '_') : NULL;
        if (a && b && c) {
            snprintf(alt, sizeof alt, "%.*s_$%s", (int)(a - name), name, b);
            e = lookup(alt);
        }
    }
    if (!e) return NULL;
    uint8_t *rgba = png_read(e->path, w, h);
    if (!rgba) {
        static unsigned warned;
        if (warned++ < 8) printf("[texpack] couldn't read %s (it should be a non-interlaced PNG)\n", e->path);
    }
    return rgba;
}

/* names already saved this run */
static uint64_t *g_dumped;
static uint32_t g_ndumped, g_dsize;

void texpack_dump(const char *name, const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (!g_dump) return;
    uint64_t key = name_hash(name) | 1;
    if (g_ndumped * 2 >= g_dsize) {
        uint32_t ns = g_dsize ? g_dsize * 2 : 4096;
        uint64_t *nt = calloc(ns, sizeof *nt);
        for (uint32_t i = 0; i < g_dsize; i++)
            if (g_dumped[i]) {
                uint32_t k = (uint32_t)g_dumped[i] & (ns - 1);
                while (nt[k]) k = (k + 1) & (ns - 1);
                nt[k] = g_dumped[i];
            }
        free(g_dumped);
        g_dumped = nt; g_dsize = ns;
    }
    uint32_t k = (uint32_t)key & (g_dsize - 1);
    while (g_dumped[k]) {
        if (g_dumped[k] == key) return;
        k = (k + 1) & (g_dsize - 1);
    }
    g_dumped[k] = key;
    g_ndumped++;
    char path[512];
    snprintf(path, sizeof path, DUMP_DIR "/%s.png", name);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; } /* saved on an earlier run */
    png_write(path, rgba, w, h);
}

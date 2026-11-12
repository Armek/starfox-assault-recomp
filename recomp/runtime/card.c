/* card.c - the memory card: HLE of the CARD API the game uses, backed by .gci files on the host.
 *
 * Slot A (channel 0) holds a 59-block card (4 Mbit, 8 KB sectors); slot B reports no card. Every
 * save is one .gci file in user/GF7E01/card_a/: the 64-byte directory entry (CARDDir) followed by
 * the file's blocks, the same format Dolphin and other GameCube save tools import and export.
 * Asynchronous calls finish immediately and call their callback with the final result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "funcs.h"
#include "runtime.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#define mkdir_p(p) mkdir((p), 0755)
#endif

#define CARD_RESULT_READY 0
#define CARD_RESULT_NOCARD (-3)
#define CARD_RESULT_NOFILE (-4)
#define CARD_RESULT_EXIST (-7)
#define CARD_RESULT_NOENT (-8)
#define CARD_RESULT_INSSPACE (-9)
#define CARD_RESULT_LIMIT (-11)
#define CARD_RESULT_FATAL_ERROR (-128)

#define SECTOR 8192u
#define USER_BLOCKS 59u
#define MAX_FILES 127
#define DIR_SIZE 64

typedef struct {
    int used;
    uint8_t dir[DIR_SIZE];   /* CARDDir, big-endian, as stored in the .gci header */
    uint8_t *data;           /* blocks * SECTOR bytes */
    uint32_t blocks;
    char path[512];
} CardFile;

static CardFile g_files[MAX_FILES];
static char g_dir[512];
static int g_inited;
static int32_t g_result[2] = {CARD_RESULT_NOCARD, CARD_RESULT_NOCARD};

static uint16_t be16p(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void load_gci(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        CardFile *cf = &g_files[i];
        if (cf->used) continue;
        if (size < DIR_SIZE || fread(cf->dir, 1, DIR_SIZE, f) != DIR_SIZE) break;
        cf->blocks = be16p(cf->dir + 0x38);
        if ((long)(DIR_SIZE + cf->blocks * SECTOR) > size) break;
        cf->data = calloc(cf->blocks, SECTOR);
        if (fread(cf->data, 1, cf->blocks * SECTOR, f) != cf->blocks * SECTOR) { free(cf->data); break; }
        snprintf(cf->path, sizeof cf->path, "%s", path);
        cf->used = 1;
        printf("[card] slot A: %.32s (%u blocks)\n", (const char *)cf->dir + 8, cf->blocks);
        break;
    }
    fclose(f);
}

static void card_init(void) {
    if (g_inited) return;
    g_inited = 1;
    mkdir_p("user");
    mkdir_p("user/GF7E01");
    snprintf(g_dir, sizeof g_dir, "user/GF7E01/card_a");
    mkdir_p(g_dir);
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char pattern[600];
    snprintf(pattern, sizeof pattern, "%s\\*.gci", g_dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char p[600];
            snprintf(p, sizeof p, "%s/%s", g_dir, fd.cFileName);
            load_gci(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(g_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcmp(e->d_name + n - 4, ".gci")) {
                char p[600];
                snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
                load_gci(p);
            }
        }
        closedir(d);
    }
#endif
}

static void save_file(CardFile *cf) {
    if (!cf->path[0]) {
        char name[33];
        memcpy(name, cf->dir + 8, 32);
        name[32] = 0;
        for (char *q = name; *q; q++)
            if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '.' || *q == '-' || *q == '_')) *q = '_';
        snprintf(cf->path, sizeof cf->path, "%s/%.4s%.2s-%s.gci", g_dir, (const char *)cf->dir, (const char *)cf->dir + 4, name);
    }
    FILE *f = fopen(cf->path, "wb");
    if (!f) { fprintf(stderr, "[card] cannot write %s\n", cf->path); return; }
    fwrite(cf->dir, 1, DIR_SIZE, f);
    fwrite(cf->data, 1, cf->blocks * SECTOR, f);
    fclose(f);
}

static uint32_t used_blocks(void) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_FILES; i++) if (g_files[i].used) n += g_files[i].blocks;
    return n;
}

/* The current game's code and maker (from the disc ID at 0x80000000). */
static int ours(const CardFile *cf) {
    for (int k = 0; k < 6; k++) if (cf->dir[k] != rd8(0x80000000u + (uint32_t)k)) return 0;
    return 1;
}

static int find(uint32_t name_addr) {
    char name[33];
    for (int k = 0; k < 32; k++) name[k] = (char)rd8(name_addr + (uint32_t)k);
    name[32] = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (g_files[i].used && ours(&g_files[i]) && !strncmp((const char *)g_files[i].dir + 8, name, 32)) return i;
    return -1;
}

/* Finish a call: record the result, run the async callback (chan, result) if any, return it. */
static void finish(Ctx *c, int32_t chan, int32_t result, uint32_t callback) {
    if (chan >= 0 && chan < 2) g_result[chan] = result;
    if (callback) {
        c->r[3] = (uint32_t)chan;
        c->r[4] = (uint32_t)result;
        rt_call(c, callback);
    }
    c->r[3] = (uint32_t)result;
}

static void fill_file_info(uint32_t fi, int32_t chan, int fileNo) {
    wr32(fi + 0x0, (uint32_t)chan);
    wr32(fi + 0x4, (uint32_t)fileNo);
    wr32(fi + 0x8, 0);
    wr32(fi + 0xC, g_files[fileNo].blocks * SECTOR);
    wr16(fi + 0x10, (uint16_t)(5 + fileNo)); /* "first block": any stable value */
}

/* s32 CARDProbeEx(s32 chan, s32 *memSize, s32 *sectorSize) */
HLE(CARDProbeEx) {
    card_init();
    int32_t chan = (int32_t)c->r[3];
    if (chan != 0) { c->r[3] = (uint32_t)CARD_RESULT_NOCARD; return; }
    if (c->r[4]) wr32(c->r[4], 4);     /* Mbit: a 59-block card */
    if (c->r[5]) wr32(c->r[5], SECTOR);
    c->r[3] = CARD_RESULT_READY;
}

/* s32 CARDMountAsync(s32 chan, void *workArea, CARDCallback detach, CARDCallback attach) */
HLE(CARDMountAsync) {
    card_init();
    int32_t chan = (int32_t)c->r[3];
    finish(c, chan, chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD, c->r[6]);
}
HLE(CARDUnmount) { int32_t chan = (int32_t)c->r[3]; g_result[chan & 1] = CARD_RESULT_NOCARD; c->r[3] = CARD_RESULT_READY; }
HLE(CARDGetResultCode) { c->r[3] = (uint32_t)g_result[c->r[3] & 1]; }

/* s32 CARDCheckAsync(s32 chan, CARDCallback cb) / CARDCheckExAsync(s32 chan, s32 *xferBytes, CARDCallback cb) */
HLE(CARDCheckAsync) { finish(c, (int32_t)c->r[3], c->r[3] == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD, c->r[4]); }
HLE(CARDCheckExAsync) {
    if (c->r[4]) wr32(c->r[4], 0);
    finish(c, (int32_t)c->r[3], c->r[3] == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD, c->r[5]);
}

/* s32 CARDGetSerialNo(s32 chan, u64 *serialNo) */
HLE(CARDGetSerialNo) {
    if (c->r[4]) { wr32(c->r[4], 0x53464150u); wr32(c->r[4] + 4, 0x43303031u); } /* stable per install */
    c->r[3] = c->r[3] == 0 ? CARD_RESULT_READY : (uint32_t)CARD_RESULT_NOCARD;
}

/* s32 CARDFreeBlocks(s32 chan, s32 *bytesNotUsed, s32 *filesNotUsed) */
HLE(CARDFreeBlocks) {
    card_init();
    int files = 0;
    for (int i = 0; i < MAX_FILES; i++) files += g_files[i].used;
    if (c->r[4]) wr32(c->r[4], (USER_BLOCKS - used_blocks()) * SECTOR);
    if (c->r[5]) wr32(c->r[5], (uint32_t)(MAX_FILES - files));
    c->r[3] = c->r[3] == 0 ? CARD_RESULT_READY : (uint32_t)CARD_RESULT_NOCARD;
}

/* s32 CARDOpen(s32 chan, const char *fileName, CARDFileInfo *fileInfo) */
HLE(CARDOpen) {
    card_init();
    int32_t chan = (int32_t)c->r[3];
    if (chan != 0) { c->r[3] = (uint32_t)CARD_RESULT_NOCARD; return; }
    int i = find(c->r[4]);
    if (i < 0) { c->r[3] = (uint32_t)CARD_RESULT_NOFILE; return; }
    fill_file_info(c->r[5], chan, i);
    c->r[3] = CARD_RESULT_READY;
}
HLE(CARDClose) { wr32(c->r[3], 0xFFFFFFFFu); c->r[3] = CARD_RESULT_READY; }

/* s32 CARDCreateAsync(s32 chan, const char *fileName, u32 size, CARDFileInfo *fileInfo, CARDCallback cb) */
HLE(CARDCreateAsync) {
    card_init();
    int32_t chan = (int32_t)c->r[3];
    uint32_t size = c->r[5], blocks = (size + SECTOR - 1) / SECTOR;
    if (chan != 0) { finish(c, chan, CARD_RESULT_NOCARD, c->r[7]); return; }
    if (find(c->r[4]) >= 0) { finish(c, chan, CARD_RESULT_EXIST, c->r[7]); return; }
    if (used_blocks() + blocks > USER_BLOCKS) { finish(c, chan, CARD_RESULT_INSSPACE, c->r[7]); return; }
    int i = 0;
    while (i < MAX_FILES && g_files[i].used) i++;
    if (i == MAX_FILES) { finish(c, chan, CARD_RESULT_NOENT, c->r[7]); return; }
    CardFile *cf = &g_files[i];
    memset(cf, 0, sizeof *cf);
    memset(cf->dir, 0xFF, DIR_SIZE);
    for (int k = 0; k < 6; k++) cf->dir[k] = rd8(0x80000000u + (uint32_t)k);
    memset(cf->dir + 8, 0, 32);
    for (int k = 0; k < 32; k++) cf->dir[8 + k] = rd8(c->r[4] + (uint32_t)k);
    cf->dir[7] = 0;                         /* bannerFormat */
    put32(cf->dir + 0x28, (uint32_t)(time(NULL) - 946684800)); /* seconds since 2000 */
    put32(cf->dir + 0x2C, 0xFFFFFFFFu);     /* iconAddr: none */
    put16(cf->dir + 0x30, 0);
    put16(cf->dir + 0x32, 0);
    cf->dir[0x34] = 0x04;                   /* permission: public */
    cf->dir[0x35] = 0;
    put16(cf->dir + 0x36, (uint16_t)(5 + i));
    put16(cf->dir + 0x38, (uint16_t)blocks);
    put32(cf->dir + 0x3C, 0xFFFFFFFFu);     /* commentAddr */
    cf->blocks = blocks;
    cf->data = calloc(blocks, SECTOR);
    cf->used = 1;
    save_file(cf);
    printf("[card] created %.32s (%u blocks)\n", (const char *)cf->dir + 8, blocks);
    fill_file_info(c->r[6], chan, i);
    finish(c, chan, CARD_RESULT_READY, c->r[7]);
}

/* s32 CARDDeleteAsync(s32 chan, const char *fileName, CARDCallback cb) */
HLE(CARDDeleteAsync) {
    card_init();
    int32_t chan = (int32_t)c->r[3];
    int i = chan == 0 ? find(c->r[4]) : -1;
    if (i < 0) { finish(c, chan, chan == 0 ? CARD_RESULT_NOFILE : CARD_RESULT_NOCARD, c->r[5]); return; }
    remove(g_files[i].path);
    free(g_files[i].data);
    memset(&g_files[i], 0, sizeof g_files[i]);
    finish(c, chan, CARD_RESULT_READY, c->r[5]);
}

/* s32 CARDFormatAsync(s32 chan, CARDCallback cb): erase everything on the card */
HLE(CARDFormatAsync) {
    card_init();
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) continue;
        remove(g_files[i].path);
        free(g_files[i].data);
        memset(&g_files[i], 0, sizeof g_files[i]);
    }
    finish(c, (int32_t)c->r[3], CARD_RESULT_READY, c->r[4]);
}

/* CARDRead[Async](CARDFileInfo *fi, void *buf, s32 length, s32 offset[, CARDCallback cb]) */
static void card_rw(Ctx *c, int write, uint32_t callback) {
    uint32_t fi = c->r[3], buf = c->r[4], len = c->r[5], off = c->r[6];
    int32_t chan = (int32_t)rd32(fi), no = (int32_t)rd32(fi + 4);
    if (chan != 0 || no < 0 || no >= MAX_FILES || !g_files[no].used) { finish(c, chan, CARD_RESULT_NOFILE, callback); return; }
    CardFile *cf = &g_files[no];
    if (off + len > cf->blocks * SECTOR) { finish(c, chan, CARD_RESULT_LIMIT, callback); return; }
    for (uint32_t k = 0; k < len; k++) {
        if (write) cf->data[off + k] = rd8(buf + k);
        else wr8(buf + k, cf->data[off + k]);
    }
    if (write) {
        put32(cf->dir + 0x28, (uint32_t)(time(NULL) - 946684800));
        save_file(cf);
    }
    finish(c, chan, CARD_RESULT_READY, callback);
}
HLE(CARDRead) { card_rw(c, 0, 0); }
HLE(CARDReadAsync) { card_rw(c, 0, c->r[7]); }
HLE(CARDWrite) { card_rw(c, 1, 0); }
HLE(CARDWriteAsync) { card_rw(c, 1, c->r[7]); }

/* s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat *stat) */
HLE(CARDGetStatus) {
    card_init();
    int32_t chan = (int32_t)c->r[3], no = (int32_t)c->r[4];
    uint32_t st = c->r[5];
    if (chan != 0 || no < 0 || no >= MAX_FILES || !g_files[no].used) {
        c->r[3] = (uint32_t)(chan != 0 ? CARD_RESULT_NOCARD : CARD_RESULT_NOFILE);
        return;
    }
    const uint8_t *d = g_files[no].dir;
    for (uint32_t k = 0; k < 32; k++) wr8(st + k, d[8 + k]);
    wr32(st + 0x20, g_files[no].blocks * SECTOR);
    wr32(st + 0x24, be32p(d + 0x28));
    for (uint32_t k = 0; k < 4; k++) wr8(st + 0x28 + k, d[k]);
    wr8(st + 0x2C, d[4]); wr8(st + 0x2D, d[5]);
    uint8_t banner = d[7];
    uint32_t icon_addr = be32p(d + 0x2C);
    uint16_t icon_fmt = be16p(d + 0x30);
    wr8(st + 0x2E, banner);
    wr32(st + 0x30, icon_addr);
    wr16(st + 0x34, icon_fmt);
    wr16(st + 0x36, be16p(d + 0x32));
    wr32(st + 0x38, be32p(d + 0x3C));
    /* __CARDUpdateIconOffsets */
    uint32_t off = icon_addr, ob = 0xFFFFFFFFu, obt = 0xFFFFFFFFu, oi[8], oit = 0xFFFFFFFFu;
    int ci = 0;
    if (icon_addr == 0xFFFFFFFFu) { banner = 0; icon_fmt = 0; off = 0; }
    if ((banner & 3) == 1) { ob = off; obt = off + 96 * 32; off += 96 * 32 + 512; }
    else if ((banner & 3) == 2) { ob = off; off += 96 * 32 * 2; }
    for (int k = 0; k < 8; k++) {
        int f = (icon_fmt >> (2 * k)) & 3;
        if (f == 1) { oi[k] = off; off += 32 * 32; ci = 1; }
        else if (f == 2) { oi[k] = off; off += 32 * 32 * 2; }
        else oi[k] = 0xFFFFFFFFu;
    }
    if (ci) { oit = off; off += 512; }
    wr32(st + 0x3C, ob);
    wr32(st + 0x40, obt);
    for (int k = 0; k < 8; k++) wr32(st + 0x44 + 4 * (uint32_t)k, oi[k]);
    wr32(st + 0x64, oit);
    wr32(st + 0x68, off);
    c->r[3] = CARD_RESULT_READY;
}

/* s32 CARDSetStatus[Async](s32 chan, s32 fileNo, CARDStat *stat[, CARDCallback cb]) */
static void set_status(Ctx *c, uint32_t callback) {
    int32_t chan = (int32_t)c->r[3], no = (int32_t)c->r[4];
    uint32_t st = c->r[5];
    if (chan != 0 || no < 0 || no >= MAX_FILES || !g_files[no].used) { finish(c, chan, CARD_RESULT_NOFILE, callback); return; }
    uint8_t *d = g_files[no].dir;
    d[7] = rd8(st + 0x2E);
    put32(d + 0x2C, rd32(st + 0x30));
    put16(d + 0x30, rd16(st + 0x34));
    put16(d + 0x32, rd16(st + 0x36));
    put32(d + 0x3C, rd32(st + 0x38));
    put32(d + 0x28, (uint32_t)(time(NULL) - 946684800));
    save_file(&g_files[no]);
    finish(c, chan, CARD_RESULT_READY, callback);
}
HLE(CARDSetStatus) { set_status(c, 0); }
HLE(CARDSetStatusAsync) { set_status(c, c->r[6]); }

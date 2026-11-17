/* dvd.c - the disc drive (DI) for the recompiled game, served from a disc image or extracted disc.
 *
 * The disc is a disc image (.iso/.gcm, read directly) or is rebuilt virtually from an extracted
 * folder like orig/GF7E01: sys/boot.bin at offset 0, bi2.bin at 0x440, apploader.img at 0x2440,
 * main.dol and fst.bin at the offsets boot.bin records, and every file at the offset its FST entry
 * records. The SDK's DVD driver is recompiled unchanged and talks to
 * the DI registers below; commands complete immediately and raise the DI transfer interrupt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "funcs.h"
#include "runtime.h"

#define DISR 0x6000u      /* status: 0x54 interrupt bits (write 1 to clear), 0x2A masks, 0x01 break */
#define DICVR 0x6004u     /* cover: bit 0 open (always closed here), bit 2 interrupt */
#define DICMDBUF0 0x6008u
#define DICMDBUF1 0x600Cu
#define DICMDBUF2 0x6010u
#define DIMAR 0x6014u     /* DMA main-memory address */
#define DILENGTH 0x6018u
#define DICR 0x601Cu      /* control: bit 0 TSTART, bit 1 DMA */
#define DIIMMBUF 0x6020u
#define DISR_TCINT 0x10u
#define DISR_TCINTMASK 0x08u
#define OS_INTERRUPT_PI_DI 21

typedef struct { uint32_t offset, size; char *path; } DiscFile;
static DiscFile *g_files;
static int g_nfiles, g_cap;
static unsigned long g_reads, g_read_bytes;

static void add_file(uint32_t offset, uint32_t size, const char *path) {
    if (g_nfiles == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_files = realloc(g_files, sizeof *g_files * (size_t)g_cap);
    }
    g_files[g_nfiles].offset = offset;
    g_files[g_nfiles].size = size;
    g_files[g_nfiles].path = malloc(strlen(path) + 1);
    strcpy(g_files[g_nfiles].path, path);
    g_nfiles++;
}

static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* Walk the FST: 12-byte entries {type:8 nameoff:24, offset/parent, size/next}, then names. */
static void add_fst(const char *root, const uint8_t *fst, uint32_t fst_size) {
    uint32_t count = be32p(fst + 8);
    const char *names = (const char *)fst + count * 12;
    char path[1024];
    uint32_t dir_end[64], depth = 0;
    char *dir_part[64];
    size_t dir_len[64];
    snprintf(path, sizeof path, "%s/files", root);
    dir_end[0] = count;
    dir_len[0] = strlen(path);
    dir_part[0] = NULL;
    (void)fst_size;
    for (uint32_t i = 1; i < count; i++) {
        while (depth > 0 && i >= dir_end[depth]) path[dir_len[--depth]] = 0;
        const uint8_t *e = fst + i * 12;
        const char *name = names + (be32p(e) & 0xFFFFFF);
        size_t len = strlen(path);
        snprintf(path + len, sizeof path - len, "/%s", name);
        if (e[0]) { /* directory */
            depth++;
            dir_end[depth] = be32p(e + 8);
            dir_len[depth] = len;
        } else {
            add_file(be32p(e + 4), be32p(e + 8), path);
            path[len] = 0;
        }
    }
    (void)dir_part;
}

static int is_directory(const char *path) {
    char probe[1024];
    snprintf(probe, sizeof probe, "%s/sys/boot.bin", path);
    FILE *f = fopen(probe, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* A disc image (.iso/.gcm: the raw 1.4 GB disc) is one file at offset 0. */
static void dvd_init_image(const char *image) {
    long size = file_size(image);
    if (size < 0x2440) rt_fatal("cannot read the disc image %s", image);
    add_file(0, (uint32_t)size, image);
    printf("[recomp] dvd: disc image %s (%ld MB)\n", image, size >> 20);
}

static void dvd_init_folder(const char *root) {
    char path[1024];
    snprintf(path, sizeof path, "%s/sys/boot.bin", root);
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    uint8_t boot[0x440];
    if (fread(boot, 1, sizeof boot, f) != sizeof boot) rt_fatal("short boot.bin");
    fclose(f);
    add_file(0, 0x440, path);
    snprintf(path, sizeof path, "%s/sys/bi2.bin", root);
    add_file(0x440, 0x2000, path);
    snprintf(path, sizeof path, "%s/sys/apploader.img", root);
    add_file(0x2440, (uint32_t)file_size(path), path);
    snprintf(path, sizeof path, "%s/sys/main.dol", root);
    add_file(be32p(boot + 0x420), (uint32_t)file_size(path), path);

    uint32_t fst_off = be32p(boot + 0x424), fst_size = be32p(boot + 0x428);
    snprintf(path, sizeof path, "%s/sys/fst.bin", root);
    add_file(fst_off, fst_size, path);
    uint8_t *fst = malloc(fst_size);
    f = fopen(path, "rb");
    if (!f || fread(fst, 1, fst_size, f) != fst_size) rt_fatal("cannot read %s", path);
    fclose(f);
    add_fst(root, fst, fst_size);
    free(fst);
    printf("[recomp] dvd: %d files mapped from %s\n", g_nfiles, root);
}

/* The disc comes from an extracted folder (sys/ and files/, as Dolphin's "Extract Entire Disc"
 * writes them) or from a disc image file. */
void rt_dvd_init(const char *root) {
    if (is_directory(root)) dvd_init_folder(root);
    else dvd_init_image(root);
}

/* The most recently used disc file stays open (a disc image is read many times). */
static FILE *open_cached(const char *path) {
    static FILE *f;
    static const char *open_path;
    if (f && open_path == path) return f;
    if (f) fclose(f);
    f = fopen(path, "rb");
    open_path = f ? path : NULL;
    return f;
}

/* Copy disc bytes [offset, offset+len) to host memory, zero-filling unmapped gaps. */
static void disc_read_host(uint32_t offset, uint8_t *dst, uint32_t len, uint32_t guest_dest) {
    memset(dst, 0, len);
    for (int k = 0; k < g_nfiles; k++) {
        DiscFile *df = &g_files[k];
        uint32_t lo = offset > df->offset ? offset : df->offset;
        uint64_t hi64 = (uint64_t)offset + len < (uint64_t)df->offset + df->size ? (uint64_t)offset + len
                                                                                   : (uint64_t)df->offset + df->size;
        if (lo >= hi64) continue;
        if (guest_dest && g_reads <= 5000)
            printf("[dvd] read %s +0x%X (%u bytes) -> %08X\n", df->path, lo - df->offset, (unsigned)(hi64 - lo),
                   guest_dest);
        FILE *f = open_cached(df->path);
        if (!f) rt_fatal("cannot open %s", df->path);
#ifdef _WIN32
        _fseeki64(f, (int64_t)(lo - df->offset), SEEK_SET);
#else
        fseeko(f, (off_t)(lo - df->offset), SEEK_SET);
#endif
        size_t got = fread(dst + (lo - offset), 1, (size_t)(hi64 - lo), f);
        (void)got;
    }
}

/* Disc bytes into a new host buffer (for the boot loader: boot.bin, the DOL, the FST). */
uint8_t *rt_disc_alloc(uint32_t offset, uint32_t len) {
    uint8_t *p = malloc(len ? len : 1);
    if (!p) rt_fatal("out of memory reading the disc");
    disc_read_host(offset, p, len, 0);
    return p;
}

/* Copy disc bytes [offset, offset+len) to guest memory. */
static void disc_read(uint32_t offset, uint32_t dest, uint32_t len) {
    g_reads++;
    g_read_bytes += len;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) rt_fatal("out of memory reading the disc");
    disc_read_host(offset, buf, len, dest);
    for (uint32_t i = 0; i < len; i++) wr8(dest + i, buf[i]);
    free(buf);
}

static void execute(void) {
    uint32_t cmd = rt_mmio_get(DICMDBUF0, 4), dest = 0x80000000u | (rt_mmio_get(DIMAR, 4) & 0x03FFFFE0u);
    switch (cmd >> 24) {
    case 0xA8: /* read (sector data, or the disc ID when the low byte is 0x40) */
        disc_read(rt_mmio_get(DICMDBUF1, 4) << 2, dest, rt_mmio_get(DICMDBUF2, 4));
        break;
    case 0x12: { /* inquiry: DVDDriveInfo {u16 revision, u16 device code, u32 release date, pad} */
        static const uint8_t info[32] = {0x00, 0x02, 0x00, 0x06, 0x20, 0x02, 0x04, 0x02};
        for (int i = 0; i < 32; i++) wr8(dest + (uint32_t)i, info[i]);
        break;
    }
    case 0xE0: /* request error: no error */
    case 0xE2: /* request audio status */
        rt_mmio_set(DIIMMBUF, 0, 4);
        break;
    default: /* seek, stop motor, audio buffer config, ...: nothing to do */
        break;
    }
    rt_mmio_set(DILENGTH, 0, 4);
    uint32_t sr = rt_mmio_get(DISR, 4) | DISR_TCINT;
    rt_mmio_set(DISR, sr, 4);
    if (sr & DISR_TCINTMASK) rt_raise_interrupt(OS_INTERRUPT_PI_DI);
}

/* Returns 1 if the DI handled the write. */
int rt_dvd_write(uint32_t off, uint32_t v, int size) {
    if (off < 0x6000 || off >= 0x6028) return 0;
    if (size != 4) rt_fatal("dvd: %d-byte write to DI register 0x%04X", size, off);
    switch (off) {
    case DISR: { /* interrupt bits are write-1-to-clear */
        uint32_t old = rt_mmio_get(DISR, 4);
        rt_mmio_set(DISR, (v & 0x2Bu) | (old & 0x54u & ~v), 4);
        return 1;
    }
    case DICVR:
        rt_mmio_set(DICVR, v & 0x2u, 4); /* keep the mask; cover closed, no pending interrupt */
        return 1;
    case DICR:
        rt_mmio_set(DICR, v & ~1u, 4);
        if (v & 1) execute();
        return 1;
    default:
        rt_mmio_set(off, v, 4);
        return 1;
    }
}

void rt_dvd_stats(unsigned long *reads, unsigned long *bytes) { *reads = g_reads; *bytes = g_read_bytes; }

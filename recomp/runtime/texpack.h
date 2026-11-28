/* HD texture packs: replacement PNGs for the game's textures, named the way Dolphin names them
 * (tex1_<w>x<h>[_m]_<texture hash>[_<palette hash>]_<format>.png), so packs made for Dolphin work.
 * Packs go in user/GF7E01/textures/ (any subfolders). With SFA_DUMP_TEXTURES=1 (or dump_textures=1 in
 * user/launcher.cfg) every texture the game uses is saved to user/GF7E01/dump/textures/ under that name. */
#ifndef TEXPACK_H
#define TEXPACK_H
#include <stddef.h>
#include <stdint.h>

void texpack_init(void);    /* scan the pack folder; cheap to call again */
int texpack_active(void);   /* a pack has textures, or dumping is on: texture names are needed */
int texpack_dumping(void);

/* The Dolphin name (no extension) of a texture: base level `data` (`size` bytes), palette `tlut`
 * (NULL for non-palette formats) as TMEM holds it. */
void texpack_name(char *out, size_t cap, uint32_t w, uint32_t h, int mipmapped, uint32_t fmt, const uint8_t *data,
                  size_t size, const uint8_t *tlut, size_t tlut_bytes);

/* A replacement for texture `name` (RGBA8, top row first, malloc'd; the caller frees it), or NULL. */
uint8_t *texpack_find(const char *name, uint32_t *w, uint32_t *h);

/* Save a texture as user/GF7E01/dump/textures/<name>.png (once per name). */
void texpack_dump(const char *name, const uint8_t *rgba, uint32_t w, uint32_t h);

/* PNG files (also used by tests and tools) */
uint8_t *png_read(const char *path, uint32_t *w, uint32_t *h);
int png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
uint64_t xxh64(const void *data, size_t len, uint64_t seed);

/* Automatic upscaling (texscale.c, xBR): RGBA8 w x h -> factor (2 or 4) times bigger, malloc'd. */
uint8_t *texscale(const uint8_t *rgba, uint32_t w, uint32_t h, int factor, uint32_t *ow, uint32_t *oh);

#endif

/* launcher.c - the start screen: pick the game disc (and the system font), choose window or
 * fullscreen, and press PLAY. It uses SDL's 2D renderer and a built-in pixel font, so it needs no
 * files of its own, and it works with a mouse, the keyboard or a game controller (the Steam Deck).
 *
 * Choices are remembered in user/launcher.cfg. The disc is checked when it's picked: the disc ID
 * must be GF7E01 and its main.dol must be the one this program was translated from.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include "funcs.h"
#include "runtime.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* ---- pixel font: 5x7 glyphs, drawn from these patterns into a texture ------------------------- */
static const char *const k_glyphs[][8] = {
    {" ", "     ", "     ", "     ", "     ", "     ", "     ", "     "},
    {"A", " ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"},
    {"B", "#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "},
    {"C", " ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "},
    {"D", "#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "},
    {"E", "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"},
    {"F", "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "},
    {"G", " ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####"},
    {"H", "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"},
    {"I", " ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "},
    {"J", "  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "},
    {"K", "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"},
    {"L", "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"},
    {"M", "#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"},
    {"N", "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"},
    {"O", " ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "},
    {"P", "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "},
    {"Q", " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"},
    {"R", "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"},
    {"S", " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "},
    {"T", "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "},
    {"U", "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "},
    {"V", "#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "},
    {"W", "#   #", "#   #", "#   #", "# # #", "# # #", "# # #", " # # "},
    {"X", "#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"},
    {"Y", "#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "},
    {"Z", "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"},
    {"0", " ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "},
    {"1", "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "},
    {"2", " ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"},
    {"3", "#### ", "    #", "    #", " ### ", "    #", "    #", "#### "},
    {"4", "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "},
    {"5", "#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "},
    {"6", " ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "},
    {"7", "#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "},
    {"8", " ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "},
    {"9", " ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "},
    {".", "     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "},
    {",", "     ", "     ", "     ", "     ", " ##  ", "  #  ", " #   "},
    {":", "     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "},
    {"/", "    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "},
    {"\\", "#    ", "#    ", " #   ", "  #  ", "   # ", "    #", "    #"},
    {"-", "     ", "     ", "     ", "#####", "     ", "     ", "     "},
    {"_", "     ", "     ", "     ", "     ", "     ", "     ", "#####"},
    {"(", "   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "},
    {")", " #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "},
    {"!", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "},
    {"?", " ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "},
    {"'", "  #  ", "  #  ", " #   ", "     ", "     ", "     ", "     "},
    {"+", "     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "},
    {"<", "   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # "},
    {">", " #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   "},
    {"=", "     ", "     ", "#####", "     ", "#####", "     ", "     "},
    {"[", " ### ", " #   ", " #   ", " #   ", " #   ", " #   ", " ### "},
    {"]", " ### ", "   # ", "   # ", "   # ", "   # ", "   # ", " ### "},
    {"&", " ##  ", "#  # ", "# #  ", " #   ", "# # #", "#  # ", " ## #"},
    {"#", " # # ", " # # ", "#####", " # # ", "#####", " # # ", " # # "},
    {"%", "##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##"},
    {"\"", " # # ", " # # ", "     ", "     ", "     ", "     ", "     "},
    {"*", "     ", "# # #", " ### ", "#####", " ### ", "# # #", "     "},
    {"~", "     ", "     ", " #   ", "# # #", "   # ", "     ", "     "},
};
#define GLYPH_W 6 /* 5 pixels and a gap */
#define GLYPH_H 8
static SDL_Texture *g_font;
static int g_glyph_of[128];

static void font_init(SDL_Renderer *r) {
    int n = (int)(sizeof k_glyphs / sizeof k_glyphs[0]);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, n * GLYPH_W, GLYPH_H, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_FillRect(s, NULL, 0);
    for (int c = 0; c < 128; c++) g_glyph_of[c] = -1;
    for (int g = 0; g < n; g++) {
        g_glyph_of[(unsigned char)k_glyphs[g][0][0]] = g;
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 5; x++)
                if (k_glyphs[g][1 + y][x] == '#')
                    ((Uint32 *)s->pixels)[y * (s->pitch / 4) + g * GLYPH_W + x] = 0xFFFFFFFFu;
    }
    g_font = SDL_CreateTextureFromSurface(r, s);
    SDL_SetTextureBlendMode(g_font, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
}

static int glyph_index(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    int g = (unsigned char)ch < 128 ? g_glyph_of[(unsigned char)ch] : -1;
    return g >= 0 ? g : g_glyph_of['?'];
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * GLYPH_W * scale - scale; }

static void text(SDL_Renderer *r, const char *s, int x, int y, int scale, SDL_Color c) {
    SDL_SetTextureColorMod(g_font, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(g_font, c.a);
    for (; *s; s++, x += GLYPH_W * scale) {
        SDL_Rect src = {glyph_index(*s) * GLYPH_W, 0, GLYPH_W, GLYPH_H}, dst = {x, y, GLYPH_W * scale, GLYPH_H * scale};
        SDL_RenderCopy(r, g_font, &src, &dst);
    }
}

static void text_center(SDL_Renderer *r, const char *s, int cx, int y, int scale, SDL_Color c) {
    text(r, s, cx - text_w(s, scale) / 2, y, scale, c);
}

/* Fit a path into `max` characters, keeping its end (the file name). */
static void fit_left(char *out, size_t n, const char *s, int max) {
    int len = (int)strlen(s);
    if (len <= max) snprintf(out, n, "%s", s);
    else snprintf(out, n, "...%s", s + len - (max - 3));
}

/* ---- disc and font checks ---------------------------------------------------------------------- */
static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int read_at(FILE *f, uint64_t off, void *buf, size_t n) {
#ifdef _WIN32
    if (_fseeki64(f, (int64_t)off, SEEK_SET) != 0) return 0;
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return 0;
#endif
    return fread(buf, 1, n, f) == n;
}

static int path_is_dir(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* Check a disc image or extracted-disc folder; on failure, say why in msg. */
static int check_disc(const char *path, char *msg, size_t n) {
    uint8_t boot[0x440], hdr[0x100];
    uint8_t *dol = NULL;
    uint32_t size = 0;
    if (path_is_dir(path)) {
        char p[1100];
        snprintf(p, sizeof p, "%s/sys/boot.bin", path);
        FILE *f = fopen(p, "rb");
        if (!f) { snprintf(msg, n, "NOT A DISC FOLDER (NO SYS/BOOT.BIN)"); return 0; }
        int ok = fread(boot, 1, 6, f) == 6;
        fclose(f);
        if (!ok || memcmp(boot, "GF7E01", 6)) { snprintf(msg, n, "NOT STAR FOX: ASSAULT USA (GF7E01)"); return 0; }
        snprintf(p, sizeof p, "%s/sys/main.dol", path);
        f = fopen(p, "rb");
        if (!f) { snprintf(msg, n, "NO SYS/MAIN.DOL IN THAT FOLDER"); return 0; }
        fseek(f, 0, SEEK_END);
        size = (uint32_t)ftell(f);
        dol = malloc(size ? size : 1);
        ok = dol && read_at(f, 0, dol, size);
        fclose(f);
        if (!ok) { free(dol); snprintf(msg, n, "CAN'T READ SYS/MAIN.DOL"); return 0; }
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) { snprintf(msg, n, "CAN'T OPEN THAT FILE"); return 0; }
        if (!read_at(f, 0, boot, sizeof boot)) { fclose(f); snprintf(msg, n, "NOT A GAMECUBE DISC IMAGE"); return 0; }
        if (memcmp(boot, "GF7E01", 6)) {
            fclose(f);
            snprintf(msg, n, "THAT DISC IS %.6s, NOT STAR FOX: ASSAULT USA (GF7E01)", (const char *)boot);
            return 0;
        }
        uint32_t dol_off = be32p(boot + 0x420);
        if (!read_at(f, dol_off, hdr, sizeof hdr)) { fclose(f); snprintf(msg, n, "THE DISC IMAGE IS CUT SHORT"); return 0; }
        size = 0x100;
        for (int i = 0; i < 18; i++) {
            uint32_t len = be32p(hdr + 0x90 + i * 4), end = be32p(hdr + i * 4) + len;
            if (len && end > size) size = end;
        }
        dol = malloc(size);
        int ok = dol && read_at(f, dol_off, dol, size);
        fclose(f);
        if (!ok) { free(dol); snprintf(msg, n, "THE DISC IMAGE IS CUT SHORT"); return 0; }
    }
    uint32_t crc = crc32_of(dol, size);
    free(dol);
    if (size != RT_DOL_SIZE || crc != RT_DOL_CRC32) {
        snprintf(msg, n, "DIFFERENT VERSION OF THE GAME: THIS BUILD NEEDS THE DISC IT WAS MADE FROM");
        return 0;
    }
    snprintf(msg, n, "DISC OK");
    return 1;
}

static int fonts_present(void) {
    return file_exists("user/fonts/font_western.bin") || file_exists("user/GF7E01/ipl.bin");
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return 0;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return 0; }
    char buf[65536];
    size_t k;
    int ok = 1;
    while ((k = fread(buf, 1, sizeof buf, a)) > 0) ok &= fwrite(buf, 1, k, b) == k;
    fclose(a);
    fclose(b);
    return ok;
}

/* Copy Dolphin's replacement fonts (and their license) from `dir` into user/fonts/. */
static int install_fonts(const char *dir, char *msg, size_t n) {
    static const char *const names[] = {"font_western.bin", "font_japanese.bin", "font-licenses.txt"};
    char from[1100], to[256];
    MKDIR("user");
    MKDIR("user/fonts");
    int copied = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(from, sizeof from, "%s/%s", dir, names[i]);
        snprintf(to, sizeof to, "user/fonts/%s", names[i]);
        if (file_exists(from) && copy_file(from, to)) copied++;
    }
    if (!file_exists("user/fonts/font_western.bin")) {
        snprintf(msg, n, "NO FONT_WESTERN.BIN THERE (IT'S IN DOLPHIN'S SYS/GC FOLDER)");
        return 0;
    }
    snprintf(msg, n, "FONTS INSTALLED (%d FILES)", copied);
    return 1;
}

/* ---- settings ---------------------------------------------------------------------------------- */
static char g_disc[1024];
static int g_fullscreen;

static void load_settings(void) {
    FILE *f = fopen("user/launcher.cfg", "r");
    if (!f) return;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "disc=", 5)) snprintf(g_disc, sizeof g_disc, "%s", line + 5);
        else if (!strncmp(line, "fullscreen=", 11)) g_fullscreen = atoi(line + 11) != 0;
    }
    fclose(f);
}

static void save_settings(void) {
    MKDIR("user");
    FILE *f = fopen("user/launcher.cfg", "w");
    if (!f) return;
    fprintf(f, "disc=%s\nfullscreen=%d\n", g_disc, g_fullscreen);
    fclose(f);
}

/* ---- file browser ------------------------------------------------------------------------------ */
typedef struct { char name[260]; char path[1100]; int kind; } Entry; /* kind: 0 dir, 1 disc, 2 other image, 3 font, 4 place, 5 back */
#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif
static Entry *g_entries;
static int g_nentries, g_cap_entries;
static char g_cwd[1100]; /* "" = the list of places */

static void add_entry(const char *name, const char *path, int kind) {
    if (g_nentries == g_cap_entries) {
        g_cap_entries = g_cap_entries ? g_cap_entries * 2 : 256;
        g_entries = realloc(g_entries, sizeof *g_entries * (size_t)g_cap_entries);
    }
    Entry *e = &g_entries[g_nentries++];
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->path, sizeof e->path, "%s", path);
    e->kind = kind;
}

static int has_suffix(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return 0;
    for (size_t k = 0; k < e; k++) {
        char a = name[n - e + k];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ext[k]) return 0;
    }
    return 1;
}

static int entry_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if ((x->kind == 0) != (y->kind == 0)) return x->kind == 0 ? -1 : 1;
    return SDL_strcasecmp(x->name, y->name);
}

static void exe_dir(char *out, size_t n) {
    out[0] = 0;
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (k == 0 || k >= n) { out[0] = 0; return; }
#else
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    out[k > 0 ? k : 0] = 0;
#endif
    char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
}

static void list_places(void) {
    char p[1100];
    g_nentries = 0;
    g_cwd[0] = 0;
    add_entry("< BACK TO THE MENU", "", 5);
    exe_dir(p, sizeof p);
    if (p[0]) add_entry("PROGRAM FOLDER", p, 4);
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (home) {
        add_entry("HOME", home, 4);
        snprintf(p, sizeof p, "%s" SEP "Desktop", home);
        if (path_is_dir(p)) add_entry("DESKTOP", p, 4);
        snprintf(p, sizeof p, "%s" SEP "Downloads", home);
        if (path_is_dir(p)) add_entry("DOWNLOADS", p, 4);
    }
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; d++)
        if (drives & (1u << d)) {
            char name[16];
            snprintf(p, sizeof p, "%c:\\", 'A' + d);
            snprintf(name, sizeof name, "DRIVE %c:", 'A' + d);
            add_entry(name, p, 4);
        }
#else
    const char *user = getenv("USER");
    if (user) {
        snprintf(p, sizeof p, "/run/media/%s", user); /* SD cards and USB drives on the Steam Deck */
        if (path_is_dir(p)) add_entry("REMOVABLE MEDIA", p, 4);
    }
    if (path_is_dir("/media")) add_entry("/MEDIA", "/media", 4);
    add_entry("/ (ROOT)", "/", 4);
#endif
}

/* List `dir`; fonts mode shows Dolphin's font files instead of disc images. */
static void list_dir(const char *dir_in, int fonts_mode) {
    char dir[1100];
#ifdef _WIN32
    if (!GetFullPathNameA(dir_in, sizeof dir, dir, NULL)) snprintf(dir, sizeof dir, "%s", dir_in);
#else
    if (!realpath(dir_in, dir)) snprintf(dir, sizeof dir, "%s", dir_in);
#endif
    g_nentries = 0;
    snprintf(g_cwd, sizeof g_cwd, "%s", dir);
    add_entry("..", "", 0);
    char full[1400];
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char *name = fd.cFileName;
            int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            const char *name = de->d_name;
            snprintf(full, sizeof full, "%s/%s", dir, name);
            int is_dir = path_is_dir(full);
#endif
            if (name[0] == '.') continue;
            snprintf(full, sizeof full, "%s%s%s", dir, (dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\') ? "" : SEP, name);
            if (is_dir) add_entry(name, full, 0);
            else if (fonts_mode) {
                if (!strcmp(name, "font_western.bin")) add_entry(name, full, 3);
            } else if (has_suffix(name, ".iso") || has_suffix(name, ".gcm")) {
                add_entry(name, full, has_suffix(name, ".nkit.iso") ? 2 : 1);
            } else if (has_suffix(name, ".rvz") || has_suffix(name, ".wia") || has_suffix(name, ".gcz") ||
                       has_suffix(name, ".ciso") || has_suffix(name, ".wbfs")) {
                add_entry(name, full, 2);
            }
#ifdef _WIN32
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
        }
        closedir(d);
    }
#endif
    if (g_nentries > 1) qsort(g_entries + 1, (size_t)(g_nentries - 1), sizeof *g_entries, entry_cmp);
    /* an extracted disc folder can be used as it is */
    snprintf(full, sizeof full, "%s/sys/boot.bin", dir);
    if (!fonts_mode && file_exists(full)) add_entry("[ USE THIS EXTRACTED DISC FOLDER ]", dir, 1);
    if (fonts_mode) {
        snprintf(full, sizeof full, "%s/font_western.bin", dir);
        if (file_exists(full)) add_entry("[ USE THE FONTS IN THIS FOLDER ]", dir, 3);
    }
}

static void parent_of(char *dir) {
    size_t n = strlen(dir);
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[--n] = 0;
    char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    if (b > s) s = b;
    if (!s) { dir[0] = 0; return; }
    if (s == dir) { dir[1] = 0; return; } /* "/" */
#ifdef _WIN32
    if (s == dir + 2 && dir[1] == ':') { dir[3] = 0; return; } /* "C:\" */
#endif
    *s = 0;
}

/* ---- the screen -------------------------------------------------------------------------------- */
#define VW 960
#define VH 600
enum { SCREEN_MAIN, SCREEN_BROWSE_DISC, SCREEN_BROWSE_FONTS };
enum { ITEM_PLAY, ITEM_DISC, ITEM_FONT, ITEM_DISPLAY, ITEM_QUIT, ITEM_COUNT };

typedef struct { float x, y, z; } Star;
static Star g_stars[220];

static void star_reset(Star *s, int at_back) {
    s->x = (float)(rand() % 2000 - 1000);
    s->y = (float)(rand() % 2000 - 1000);
    s->z = at_back ? 1000.0f : (float)(rand() % 1000 + 1);
}

static void draw_stars(SDL_Renderer *r, float dt) {
    for (int i = 0; i < (int)(sizeof g_stars / sizeof g_stars[0]); i++) {
        Star *s = &g_stars[i];
        float pz = s->z;
        s->z -= dt * 260.0f;
        if (s->z <= 1) { star_reset(s, 1); continue; }
        int x0 = (int)(VW / 2 + s->x / pz * 300), y0 = (int)(VH / 2 + s->y / pz * 300);
        int x1 = (int)(VW / 2 + s->x / s->z * 300), y1 = (int)(VH / 2 + s->y / s->z * 300);
        if (x1 < 0 || x1 >= VW || y1 < 0 || y1 >= VH) { star_reset(s, 1); continue; }
        Uint8 b = (Uint8)(255 - (int)(s->z / 1000.0f * 200));
        SDL_SetRenderDrawColor(r, b / 2, (Uint8)(b * 3 / 4), b, 255);
        SDL_RenderDrawLine(r, x0, y0, x1, y1);
    }
}

static void panel(SDL_Renderer *r, SDL_Rect rc, int selected, int enabled) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 10, 30, 50, selected ? 230 : 170);
    SDL_RenderFillRect(r, &rc);
    if (selected) SDL_SetRenderDrawColor(r, 90, 230, 255, 255);
    else SDL_SetRenderDrawColor(r, 40, 110, 140, enabled ? 255 : 120);
    SDL_RenderDrawRect(r, &rc);
    if (selected) { /* a second line makes the selection pop */
        SDL_Rect in = {rc.x + 2, rc.y + 2, rc.w - 4, rc.h - 4};
        SDL_RenderDrawRect(r, &in);
    }
}

static const SDL_Color WHITE = {235, 245, 255, 255}, CYAN = {90, 230, 255, 255}, DIM = {120, 150, 170, 255},
                       GREEN = {120, 255, 140, 255}, RED = {255, 110, 100, 255}, GOLD = {255, 210, 90, 255};

int launcher_run(const char *found_disc, int *fullscreen_io, char *disc_out, size_t disc_n) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) return -1;
    load_settings();
    if (*fullscreen_io) g_fullscreen = 1;
    char msg[256] = "", tmp[512];
    int disc_ok = 0;
    if (g_disc[0]) disc_ok = check_disc(g_disc, msg, sizeof msg);
    if (!disc_ok && found_disc) {
        snprintf(g_disc, sizeof g_disc, "%s", found_disc);
        disc_ok = check_disc(g_disc, msg, sizeof msg);
    }
    if (!disc_ok && !g_disc[0]) snprintf(msg, sizeof msg, "CHOOSE YOUR STAR FOX: ASSAULT DISC IMAGE (.ISO)");

    SDL_Window *win = SDL_CreateWindow("Star Fox: Assault (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       VW, VH, SDL_WINDOW_RESIZABLE | (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) return -1;
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!r) { SDL_DestroyWindow(win); return -1; }
    SDL_RenderSetLogicalSize(r, VW, VH);
    font_init(r);
    for (int i = 0; i < (int)(sizeof g_stars / sizeof g_stars[0]); i++) star_reset(&g_stars[i], 0);
    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);

    int screen = SCREEN_MAIN, sel = disc_ok ? ITEM_PLAY : ITEM_DISC, bsel = 0, scroll = 0, result = 0;
    int stick_held = 0, msg_ok = disc_ok;
    Uint32 last = SDL_GetTicks();
    const int rows = 13, row_h = 30, list_y = 118;
    for (int running = 1; running;) {
        int move = 0, act = 0, back = 0, play = 0, clicked = -1, wheel = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = 0; result = 0; }
            else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_UP: move = -1; break;
                case SDLK_DOWN: move = 1; break;
                case SDLK_PAGEUP: move = -rows; break;
                case SDLK_PAGEDOWN: move = rows; break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                    if (e.key.keysym.mod & KMOD_ALT) {
                        g_fullscreen = !g_fullscreen;
                        SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    } else act = 1;
                    break;
                case SDLK_ESCAPE: case SDLK_BACKSPACE: back = 1; break;
                case SDLK_F11:
                    g_fullscreen = !g_fullscreen;
                    SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                default: break;
                }
            } else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) {
                pad = SDL_GameControllerOpen(e.cdevice.which);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: move = -1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move = 1; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: move = -rows; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: move = rows; break;
                case SDL_CONTROLLER_BUTTON_A: act = 1; break;
                case SDL_CONTROLLER_BUTTON_B: back = 1; break;
                case SDL_CONTROLLER_BUTTON_START: play = 1; break;
                default: break;
                }
            } else if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                if (!stick_held && (e.caxis.value < -20000 || e.caxis.value > 20000)) {
                    move = e.caxis.value < 0 ? -1 : 1;
                    stick_held = 1;
                } else if (e.caxis.value > -8000 && e.caxis.value < 8000) stick_held = 0;
            } else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN) {
                int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                int my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                /* hover follows the mouse only once it really moves (a window opening under the
                 * pointer, or the keyboard/controller in use, shouldn't jump the selection) */
                static int mouse_x = -1, mouse_y = -1;
                if (e.type == SDL_MOUSEMOTION) { /* the pointer's place on the screen, not in the window */
                    int gx, gy;
                    SDL_GetGlobalMouseState(&gx, &gy);
                    int first = mouse_x < 0, still = abs(gx - mouse_x) < 3 && abs(gy - mouse_y) < 3;
                    if (first || still) {
                        if (first) { mouse_x = gx; mouse_y = gy; }
                        continue;
                    }
                    mouse_x = gx;
                    mouse_y = gy;
                }
                int hit = -1;
                if (screen == SCREEN_MAIN) {
                    for (int i = 0; i < ITEM_COUNT; i++)
                        if (mx >= 180 && mx < VW - 180 && my >= 200 + i * 62 && my < 200 + i * 62 + 50) hit = i;
                    if (hit >= 0) sel = hit;
                } else if (mx >= 60 && mx < VW - 60 && my >= list_y && my < list_y + rows * row_h) {
                    hit = scroll + (my - list_y) / row_h;
                    if (hit < g_nentries) bsel = hit;
                    else hit = -1;
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && hit >= 0) clicked = hit;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) back = 1;
            } else if (e.type == SDL_MOUSEWHEEL) {
                wheel = -e.wheel.y * 3;
            }
        }
        if (clicked >= 0) act = 1;
        /* SFA_LAUNCHER_SCRIPT=down,enter,...: testing aid; every 0.6 s, save a screenshot to
         * build/GF7E01/launcher_NN.bmp, then press the next key (up down pgdn enter esc start) */
        static const char *script = (const char *)1;
        static Uint32 script_next;
        static int shot_n, take_shot;
        if (script == (const char *)1) script = getenv("SFA_LAUNCHER_SCRIPT");
        if (script && SDL_GetTicks() >= script_next) {
            script_next = SDL_GetTicks() + 600;
            take_shot = 1;
            if (!*script) { running = 0; result = 0; }
            size_t n = strcspn(script, ",");
            if (!strncmp(script, "up", n) && n == 2) move = -1;
            else if (!strncmp(script, "down", n) && n == 4) move = 1;
            else if (!strncmp(script, "pgdn", n) && n == 4) move = rows;
            else if (!strncmp(script, "enter", n) && n == 5) act = 1;
            else if (!strncmp(script, "esc", n) && n == 3) back = 1;
            else if (!strncmp(script, "start", n) && n == 5) play = 1;
            script += n + (script[n] == ',');
        }

        if (screen == SCREEN_MAIN) {
            if (move) sel = (sel + (move > 0 ? 1 : ITEM_COUNT - 1)) % ITEM_COUNT;
            if (back) { sel = ITEM_QUIT; }
            if (play && disc_ok) { result = 1; running = 0; }
            if (act) {
                switch (sel) {
                case ITEM_PLAY:
                    if (disc_ok) { result = 1; running = 0; }
                    else { snprintf(msg, sizeof msg, "CHOOSE A DISC IMAGE FIRST"); msg_ok = 0; }
                    break;
                case ITEM_DISC: case ITEM_FONT: {
                    screen = sel == ITEM_DISC ? SCREEN_BROWSE_DISC : SCREEN_BROWSE_FONTS;
                    char start[1100] = "";
                    if (sel == ITEM_DISC && g_disc[0]) {
                        snprintf(start, sizeof start, "%s", g_disc);
                        if (!path_is_dir(start)) parent_of(start);
                    }
                    if (!start[0]) exe_dir(start, sizeof start);
                    if (start[0] && path_is_dir(start)) list_dir(start, screen == SCREEN_BROWSE_FONTS);
                    else list_places();
                    bsel = scroll = 0;
                    if (sel == ITEM_DISC && g_disc[0]) { /* start on the disc chosen now */
                        const char *base = g_disc + strlen(g_disc);
                        while (base > g_disc && base[-1] != '/' && base[-1] != '\\') base--;
                        for (int i = 0; i < g_nentries; i++)
                            if (g_entries[i].kind == 1 && !strcmp(g_entries[i].name, base)) bsel = i;
                    }
                    break;
                }
                case ITEM_DISPLAY:
                    g_fullscreen = !g_fullscreen;
                    SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    save_settings();
                    break;
                case ITEM_QUIT: result = 0; running = 0; break;
                }
            }
        } else {
            int fonts_mode = screen == SCREEN_BROWSE_FONTS;
            if (move) {
                bsel += move;
                if (bsel < 0) bsel = 0;
                if (bsel >= g_nentries) bsel = g_nentries - 1;
            }
            if (wheel) {
                scroll += wheel;
                if (scroll > g_nentries - rows) scroll = g_nentries - rows;
                if (scroll < 0) scroll = 0;
            }
            if (play && disc_ok) { result = 1; running = 0; }
            if (back) {
                if (g_cwd[0]) { act = 1; bsel = 0; } /* ".." */
                else screen = SCREEN_MAIN;
            }
            if (act && g_nentries > 0) {
                Entry en = g_entries[bsel];
                if (g_cwd[0] && bsel == 0 && !strcmp(en.name, "..")) {
                    char up[1100];
                    snprintf(up, sizeof up, "%s", g_cwd);
                    char before[1100];
                    snprintf(before, sizeof before, "%s", up);
                    parent_of(up);
                    if (!up[0] || !strcmp(up, before)) list_places();
                    else list_dir(up, fonts_mode);
                    bsel = scroll = 0;
                } else if (en.kind == 5) {
                    screen = SCREEN_MAIN;
                } else if (en.kind == 0 || en.kind == 4) {
                    list_dir(en.path, fonts_mode);
                    bsel = scroll = 0;
                } else if (en.kind == 1) {
                    char m[256];
                    if (check_disc(en.path, m, sizeof m)) {
                        snprintf(g_disc, sizeof g_disc, "%s", en.path);
                        disc_ok = 1;
                        save_settings();
                        snprintf(msg, sizeof msg, "DISC OK - PRESS PLAY");
                        msg_ok = 1;
                        screen = SCREEN_MAIN;
                        sel = ITEM_PLAY;
                    } else {
                        snprintf(msg, sizeof msg, "%s", m);
                        msg_ok = 0;
                    }
                } else if (en.kind == 2) {
                    snprintf(msg, sizeof msg, "COMPRESSED IMAGE: CONVERT IT TO ISO IN DOLPHIN (RIGHT-CLICK, CONVERT FILE)");
                    msg_ok = 0;
                } else if (en.kind == 3) {
                    char dir[1100];
                    snprintf(dir, sizeof dir, "%s", en.path);
                    if (!path_is_dir(dir)) parent_of(dir);
                    msg_ok = install_fonts(dir, msg, sizeof msg);
                    if (msg_ok) { screen = SCREEN_MAIN; sel = disc_ok ? ITEM_PLAY : ITEM_DISC; }
                }
            }
            if (bsel < scroll) scroll = bsel;
            if (bsel >= scroll + rows) scroll = bsel - rows + 1;
        }

        /* draw */
        Uint32 now = SDL_GetTicks();
        float dt = (float)(now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;
        SDL_SetRenderDrawColor(r, 2, 4, 14, 255);
        SDL_RenderClear(r);
        draw_stars(r, dt);
        if (screen == SCREEN_MAIN) {
            float pulse = 0.75f + 0.25f * sinf((float)now / 300.0f);
            text_center(r, "STAR FOX: ASSAULT", VW / 2 + 4, 54 + 4, 7, (SDL_Color){0, 60, 120, 200});
            text_center(r, "STAR FOX: ASSAULT", VW / 2, 54, 7, WHITE);
            text_center(r, "STATIC RECOMPILATION FOR PC", VW / 2, 124, 2, CYAN);
            static const char *const labels[ITEM_COUNT] = {"PLAY", "GAME DISC", "SYSTEM FONT", "DISPLAY", "QUIT"};
            for (int i = 0; i < ITEM_COUNT; i++) {
                SDL_Rect rc = {180, 200 + i * 62, VW - 360, 50};
                int enabled = i != ITEM_PLAY || disc_ok;
                panel(r, rc, sel == i, enabled);
                SDL_Color c = enabled ? (sel == i ? CYAN : WHITE) : DIM;
                if (i == ITEM_PLAY) {
                    if (sel == i && enabled) c = (SDL_Color){(Uint8)(90 + 165 * (1 - pulse)), 230, 255, 255};
                    text_center(r, "PLAY", VW / 2, rc.y + 11, 4, c);
                    continue;
                }
                text(r, labels[i], rc.x + 20, rc.y + 17, 2, c);
                const char *value = "";
                SDL_Color vc = DIM;
                if (i == ITEM_DISC) {
                    if (g_disc[0]) {
                        const char *base = g_disc + strlen(g_disc);
                        while (base > g_disc && base[-1] != '/' && base[-1] != '\\') base--;
                        fit_left(tmp, sizeof tmp, *base ? base : g_disc, 30);
                        value = tmp;
                        vc = disc_ok ? GREEN : RED;
                    } else { value = "NOT CHOSEN"; vc = RED; }
                } else if (i == ITEM_FONT) {
                    int ok = fonts_present();
                    value = ok ? "FOUND" : "MISSING (TEXT WILL BE BLANK)";
                    vc = ok ? GREEN : GOLD;
                } else if (i == ITEM_DISPLAY) {
                    value = g_fullscreen ? "FULLSCREEN" : "WINDOW";
                    vc = WHITE;
                }
                text(r, value, rc.x + rc.w - 20 - text_w(value, 2), rc.y + 17, 2, vc);
            }
            if (msg[0]) text_center(r, msg, VW / 2, 520, 2, msg_ok ? GREEN : GOLD);
            text_center(r, "ENTER / A: SELECT    ESC / B: BACK    START: PLAY    F11: FULLSCREEN", VW / 2, 566, 1, DIM);
        } else {
            int fonts_mode = screen == SCREEN_BROWSE_FONTS;
            text_center(r, fonts_mode ? "FIND DOLPHIN'S SYS/GC FOLDER (FONT_WESTERN.BIN)" : "CHOOSE YOUR GAME DISC (.ISO / .GCM)",
                        VW / 2, 30, 2, WHITE);
            fit_left(tmp, sizeof tmp, g_cwd[0] ? g_cwd : "PLACES", 70);
            text_center(r, tmp, VW / 2, 70, 2, CYAN);
            SDL_Rect frame = {50, list_y - 8, VW - 100, rows * row_h + 16};
            panel(r, frame, 0, 1);
            for (int k = 0; k < rows && scroll + k < g_nentries; k++) {
                int i = scroll + k;
                Entry *en = &g_entries[i];
                SDL_Rect rc = {60, list_y + k * row_h, VW - 120, row_h - 4};
                if (i == bsel) {
                    SDL_SetRenderDrawColor(r, 30, 90, 120, 220);
                    SDL_RenderFillRect(r, &rc);
                }
                static const char *const tags[] = {"DIR ", "DISC", "    ", "FONT", "    ", "    "};
                SDL_Color c = en->kind == 1 ? GREEN : en->kind == 3 ? GREEN : en->kind == 2 ? DIM : WHITE;
                text(r, tags[en->kind], rc.x + 8, rc.y + 6, 2, en->kind == 0 ? CYAN : c);
                fit_left(tmp, sizeof tmp, en->name, 58);
                text(r, tmp, rc.x + 80, rc.y + 6, 2, c);
            }
            if (g_nentries > rows) { /* scroll bar */
                SDL_Rect bar = {VW - 58, list_y + scroll * rows * row_h / g_nentries, 4, rows * rows * row_h / g_nentries};
                SDL_SetRenderDrawColor(r, 90, 230, 255, 255);
                SDL_RenderFillRect(r, &bar);
            }
            if (msg[0]) text_center(r, msg, VW / 2, 534, 2, msg_ok ? GREEN : GOLD);
            text_center(r, "ENTER / A: OPEN    ESC / B: UP A FOLDER    LB / RB: PAGE", VW / 2, 570, 1, DIM);
        }
        if (take_shot) { /* SFA_LAUNCHER_SCRIPT: the frame as drawn, before it's presented */
            take_shot = 0;
            int w = 0, h = 0;
            SDL_GetRendererOutputSize(r, &w, &h);
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            char path[64];
            snprintf(path, sizeof path, "build/GF7E01/launcher_%02d.bmp", shot_n++);
            if (s && SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, path);
            if (s) SDL_FreeSurface(s);
        }
        SDL_RenderPresent(r);
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) SDL_Delay(30);
    }
    save_settings();
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyTexture(g_font);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    free(g_entries);
    g_entries = NULL;
    g_nentries = g_cap_entries = 0;
    if (result == 1) {
        snprintf(disc_out, disc_n, "%s", g_disc);
        *fullscreen_io = g_fullscreen;
    }
    return result;
}

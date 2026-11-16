/* gx.h - interface between the GX command decoder (gx.c, runs on guest threads) and the
 * renderer (video.c, runs on its own thread with the window and OpenGL context). */
#ifndef GX_H
#define GX_H

#include <stdint.h>

#define EFB_WIDTH 640
#define EFB_HEIGHT 528

/* One vertex after CPU transform: clip-space position, both color channels, eight texcoords. */
typedef struct {
    float x, y, z, w;
    float c0[4], c1[4];
    float uv[8][2];
} GxVertex;

typedef enum { GX_PRIM_TRIANGLES, GX_PRIM_LINES, GX_PRIM_POINTS } GxPrimKind;

/* The TEV configuration: everything that decides the generated fragment shader. The renderer
 * caches one GL program per distinct value (compared bytewise, so keep unused fields zeroed). */
typedef struct {
    uint32_t color_env[16]; /* BP 0xC0 + 2i: d, c, b, a, bias, sub, clamp, scale, dest */
    uint32_t alpha_env[16]; /* BP 0xC1 + 2i: swaps, d, c, b, a, bias, sub, clamp, scale, dest */
    uint32_t tref[8];       /* BP 0x28-0x2F: per stage pair texmap, texcoord, enable, channel */
    uint32_t ksel[8];       /* BP 0xF6-0xFD: swap tables and konst selectors */
    uint32_t alpha_cmp;     /* BP 0xF3 compare functions and logic (refs are uniforms) */
    uint8_t nstages;
    uint8_t pad[3];
} GxTevConfig;

/* Render state for one batch. */
typedef struct {
    GxTevConfig tev;
    float konst[4][4];       /* konst colors K0-K3 */
    float reg[4][4];         /* TEV registers PREV, C0, C1, C2 (initial values) */
    float alpha_ref[2];
    uint32_t texture[8];     /* GxTexture id per texture map, 0 = none */
    uint8_t wrap_s[8], wrap_t[8], linear[8];
    uint8_t blend_enable, blend_src, blend_dst, blend_sub, logic_op;
    uint8_t color_update, alpha_update;
    uint8_t z_enable, z_func, z_update;
    uint8_t cull;            /* 0 none, 1 front (cw), 2 back (ccw), 3 all */
    uint16_t scissor[4];     /* x0, y0, x1, y1 in EFB pixels (inclusive) */
} GxState;

typedef struct {
    GxState state;
    GxPrimKind kind;
    uint32_t serial;        /* render-state serial (gx.c): equal serial means equal state */
    uint32_t first, count;  /* into the frame's vertex array */
} GxBatch;

typedef struct {
    uint32_t id, width, height;
    uint8_t *rgba;           /* width*height*4, owned by the frame until uploaded */
} GxTextureUpload;

/* EFB clears requested by copies: applied before batches with index >= at_batch. */
typedef struct {
    uint32_t at_batch;
    float color[4];
    float depth;
    uint8_t clear_color, clear_depth;
} GxClear;

typedef struct GxFrame {
    GxVertex *verts;
    uint32_t nverts, cap_verts;
    GxBatch *batches;
    uint32_t nbatches, cap_batches;
    GxTextureUpload *uploads;
    uint32_t nuploads, cap_uploads;
    GxClear *clears;
    uint32_t nclears, cap_clears;
    uint16_t copy_rect[4];   /* EFB source rectangle of the XFB copy: x, y, w, h */
    uint32_t number;
    struct GxCopyRequest *copy; /* non-NULL: a mid-frame segment ending in an EFB-to-texture copy */
} GxFrame;

/* An EFB copy to texture: the renderer draws the segment, reads back the rectangle (RGBA8, top
 * row first) into rgba and sets done; the guest thread then encodes it into guest memory. */
typedef struct GxCopyRequest {
    uint32_t x, y, w, h;
    uint8_t *rgba;
    volatile int done;
} GxCopyRequest;
void video_copy_segment(GxFrame *frame); /* takes ownership of frame; returns when copy->done */

/* video.c */
/* Controller state in PADStatus layout (button bits as in the SDK: A 0x100, B 0x200, X 0x400,
 * Y 0x800, START 0x1000, Z 0x10, R 0x20, L 0x40, D-pad left/right/down/up 0x1/0x2/0x4/0x8). */
typedef struct {
    uint16_t buttons;
    int8_t stick_x, stick_y, substick_x, substick_y;
    uint8_t trigger_l, trigger_r;
    int connected;
} PadState;
void video_get_pad(int port, PadState *out);
void video_set_auto_press(int on);
void video_start(int dump_frames);
void video_submit(GxFrame *frame);  /* takes ownership */
int video_quit_requested(void);

/* gx.c */
void gx_fifo_write(uint32_t value, int size);  /* a write to the 0xCC008000 pipe */
void gx_init(void);
void gx_pi_wptr_write(uint32_t v);   /* PI FIFO write pointer (0xCC003014) */
uint32_t gx_pi_wptr_read(void);
void gx_profile_report(void);

#endif

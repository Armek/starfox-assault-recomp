/* gx.h - interface between the GX command decoder (gx.c, runs on guest threads) and the
 * renderer (video.c, runs on its own thread with the window and OpenGL context). */
#ifndef GX_H
#define GX_H

#include <stdint.h>

#define EFB_WIDTH 640
#define EFB_HEIGHT 528

/* One vertex after CPU transform: clip-space position, color, texture coordinate. */
typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float s, t;
} GxVertex;

typedef enum { GX_PRIM_TRIANGLES, GX_PRIM_LINES, GX_PRIM_POINTS } GxPrimKind;

/* Render state for one batch of triangles. */
typedef struct {
    uint32_t texture;       /* GxTexture id, 0 = none */
    uint8_t tex_wrap_s, tex_wrap_t, tex_linear;
    uint8_t blend_enable, blend_src, blend_dst, blend_sub, logic_op;
    uint8_t color_update, alpha_update;
    uint8_t z_enable, z_func, z_update;
    uint8_t cull;            /* 0 none, 1 front (cw), 2 back (ccw), 3 all */
    uint8_t tev_mode;        /* 0 = color only, 1 = texture * color, 2 = texture only */
    uint16_t scissor[4];     /* x0, y0, x1, y1 in EFB pixels (inclusive) */
    uint8_t alpha_test;      /* 1 = discard alpha < alpha_ref */
    float alpha_ref;
} GxState;

typedef struct {
    GxState state;
    GxPrimKind kind;
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
} GxFrame;

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

#endif

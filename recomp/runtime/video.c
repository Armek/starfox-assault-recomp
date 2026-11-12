/* video.c - the window and the OpenGL renderer, on a dedicated render thread.
 *
 * Guest threads hand over complete frames (vertices already in clip space, batches with their
 * render state, textures to upload). The render thread draws them into a 640x528 EFB-sized
 * framebuffer, then scales the XFB copy rectangle into the window. With --dump-frames it also
 * saves every presented frame to build/GF7E01/frames/frame_NNNNN.bmp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "gx.h"
#include "runtime.h"

/* ---- minimal OpenGL 3.3 core loader ------------------------------------------------------------ */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
#ifdef _WIN32
#define GLAPI __stdcall
#else
#define GLAPI
#endif
#define GL_FUNCS(X) \
    X(void, glClear, GLbitfield) X(void, glClearColor, GLfloat, GLfloat, GLfloat, GLfloat) \
    X(void, glClearDepth, GLdouble) X(void, glViewport, GLint, GLint, GLsizei, GLsizei) \
    X(void, glEnable, GLenum) X(void, glDisable, GLenum) X(void, glDepthFunc, GLenum) \
    X(void, glDepthMask, GLboolean) X(void, glColorMask, GLboolean, GLboolean, GLboolean, GLboolean) \
    X(void, glBlendFunc, GLenum, GLenum) X(void, glBlendEquation, GLenum) X(void, glCullFace, GLenum) \
    X(void, glScissor, GLint, GLint, GLsizei, GLsizei) X(void, glDrawArrays, GLenum, GLint, GLsizei) \
    X(void, glGenTextures, GLsizei, GLuint *) X(void, glBindTexture, GLenum, GLuint) \
    X(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) \
    X(void, glTexParameteri, GLenum, GLenum, GLint) X(void, glPixelStorei, GLenum, GLint) \
    X(void, glReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) \
    X(void, glActiveTexture, GLenum) X(GLuint, glCreateShader, GLenum) \
    X(void, glShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *) \
    X(void, glCompileShader, GLuint) X(void, glGetShaderiv, GLuint, GLenum, GLint *) \
    X(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei *, GLchar *) X(GLuint, glCreateProgram, void) \
    X(void, glAttachShader, GLuint, GLuint) X(void, glLinkProgram, GLuint) X(void, glUseProgram, GLuint) \
    X(GLint, glGetUniformLocation, GLuint, const GLchar *) X(void, glUniform1i, GLint, GLint) \
    X(void, glUniform1f, GLint, GLfloat) X(void, glGenBuffers, GLsizei, GLuint *) \
    X(void, glBindBuffer, GLenum, GLuint) X(void, glBufferData, GLenum, GLsizeiptr, const void *, GLenum) \
    X(void, glGenVertexArrays, GLsizei, GLuint *) X(void, glBindVertexArray, GLuint) \
    X(void, glEnableVertexAttribArray, GLuint) \
    X(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) \
    X(void, glGenFramebuffers, GLsizei, GLuint *) X(void, glBindFramebuffer, GLenum, GLuint) \
    X(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint) \
    X(void, glGenRenderbuffers, GLsizei, GLuint *) X(void, glBindRenderbuffer, GLenum, GLuint) \
    X(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei) \
    X(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint) \
    X(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
#define GL_DECLARE(ret, name, ...) static ret(GLAPI *name)(__VA_ARGS__);
GL_FUNCS(GL_DECLARE)
#define GL_LOAD(ret, name, ...) \
    if (!(*(void **)&name = SDL_GL_GetProcAddress(#name))) rt_fatal("OpenGL function %s missing", #name);

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_TRIANGLES 0x0004
#define GL_LINES 0x0001
#define GL_POINTS 0x0000
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B

/* GX enums -> GL */
static const GLenum k_depth_func[8] = {0x0200, 0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206, 0x0207};
static const GLenum k_blend_factor[8] = {0 /*ZERO*/, 1 /*ONE*/, 0x0306 /*DST_COLOR / SRC_COLOR*/,
                                         0x0307 /*1-DST_COLOR*/, 0x0302 /*SRC_ALPHA*/, 0x0303 /*1-SRC_ALPHA*/,
                                         0x0304 /*DST_ALPHA*/, 0x0305 /*1-DST_ALPHA*/};

/* ---- input ----------------------------------------------------------------------------------------
 * Port 1 = keyboard + the first game controller (Steam Deck controls included), merged.
 * Keyboard: arrows/WASD = stick, Enter = START, Space/J = A, K/Backspace = B, L = X, I = Y,
 * Q/E = L/R, Z = Z, IJKL-style C-stick on the numpad (8/4/6/2). */
static SDL_mutex *g_pad_lock;
static PadState g_kbd, g_ctl;
static SDL_GameController *g_controller;
static int g_auto_press;

void video_set_auto_press(int on) { g_auto_press = on; }

static int8_t axis8(Sint16 v) {
    int x = v / 256;
    if (x > -20 && x < 20) x = 0; /* dead zone */
    return (int8_t)(x > 127 ? 127 : x < -127 ? -127 : x);
}

static void poll_controller(void) {
    if (!g_controller) return;
    PadState p = {0};
    static const struct { SDL_GameControllerButton b; uint16_t bit; } map[] = {
        {SDL_CONTROLLER_BUTTON_A, 0x100}, {SDL_CONTROLLER_BUTTON_B, 0x200}, {SDL_CONTROLLER_BUTTON_X, 0x400},
        {SDL_CONTROLLER_BUTTON_Y, 0x800}, {SDL_CONTROLLER_BUTTON_START, 0x1000},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x10}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x1},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x2}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x4}, {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x8},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GameControllerGetButton(g_controller, map[i].b)) p.buttons |= map[i].bit;
    p.stick_x = axis8(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX));
    p.stick_y = (int8_t)-axis8(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY));
    p.substick_x = axis8(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX));
    p.substick_y = (int8_t)-axis8(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY));
    p.trigger_l = (uint8_t)(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 128);
    p.trigger_r = (uint8_t)(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 128);
    if (p.trigger_l > 200) p.buttons |= 0x40; /* digital L/R click at the end of the travel */
    if (p.trigger_r > 200) p.buttons |= 0x20;
    p.connected = 1;
    SDL_LockMutex(g_pad_lock);
    g_ctl = p;
    SDL_UnlockMutex(g_pad_lock);
}

static void poll_keyboard(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    PadState p = {0};
    p.connected = 1;
    if (k[SDL_SCANCODE_RETURN]) p.buttons |= 0x1000;
    if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_J]) p.buttons |= 0x100;
    if (k[SDL_SCANCODE_K] || k[SDL_SCANCODE_BACKSPACE]) p.buttons |= 0x200;
    if (k[SDL_SCANCODE_L]) p.buttons |= 0x400;
    if (k[SDL_SCANCODE_I]) p.buttons |= 0x800;
    if (k[SDL_SCANCODE_Z]) p.buttons |= 0x10;
    if (k[SDL_SCANCODE_Q]) { p.buttons |= 0x40; p.trigger_l = 255; }
    if (k[SDL_SCANCODE_E]) { p.buttons |= 0x20; p.trigger_r = 255; }
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) p.stick_x = -100;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) p.stick_x = 100;
    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) p.stick_y = 100;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) p.stick_y = -100;
    if (k[SDL_SCANCODE_KP_4]) p.substick_x = -100;
    if (k[SDL_SCANCODE_KP_6]) p.substick_x = 100;
    if (k[SDL_SCANCODE_KP_8]) p.substick_y = 100;
    if (k[SDL_SCANCODE_KP_2]) p.substick_y = -100;
    SDL_LockMutex(g_pad_lock);
    g_kbd = p;
    SDL_UnlockMutex(g_pad_lock);
}

void video_get_pad(int port, PadState *out) {
    memset(out, 0, sizeof *out);
    if (port != 0) return;
    out->connected = 1;
    if (g_pad_lock) {
        SDL_LockMutex(g_pad_lock);
        PadState a = g_kbd, b = g_ctl;
        SDL_UnlockMutex(g_pad_lock);
        out->buttons = a.buttons | b.buttons;
        out->stick_x = b.stick_x ? b.stick_x : a.stick_x;
        out->stick_y = b.stick_y ? b.stick_y : a.stick_y;
        out->substick_x = b.substick_x ? b.substick_x : a.substick_x;
        out->substick_y = b.substick_y ? b.substick_y : a.substick_y;
        out->trigger_l = b.trigger_l > a.trigger_l ? b.trigger_l : a.trigger_l;
        out->trigger_r = b.trigger_r > a.trigger_r ? b.trigger_r : a.trigger_r;
    }
    if (g_auto_press) { /* testing aid: tap START, then A, every 4 seconds */
        Uint32 t = SDL_GetTicks() % 4000;
        if (t < 150) out->buttons |= 0x1000;
        else if (t >= 2000 && t < 2150) out->buttons |= 0x100;
    }
}

/* ---- frame queue --------------------------------------------------------------------------------- */
static SDL_mutex *g_qlock;
static SDL_cond *g_qcond;
static GxFrame *g_pending_frame;
static volatile int g_quit;
static int g_dump;

int video_quit_requested(void) { return g_quit; }

static void free_frame(GxFrame *f) {
    for (uint32_t i = 0; i < f->nuploads; i++) free(f->uploads[i].rgba);
    free(f->uploads);
    free(f->verts);
    free(f->batches);
    free(f->clears);
    free(f);
}

void video_submit(GxFrame *frame) {
    SDL_LockMutex(g_qlock);
    if (g_pending_frame) { /* renderer is behind: keep its uploads, drop the old frame's draws */
        GxFrame *old = g_pending_frame;
        for (uint32_t i = 0; i < old->nuploads; i++) {
            if (frame->nuploads == frame->cap_uploads) {
                frame->cap_uploads = frame->cap_uploads ? frame->cap_uploads * 2 : 64;
                frame->uploads = realloc(frame->uploads, sizeof *frame->uploads * frame->cap_uploads);
            }
            frame->uploads[frame->nuploads++] = old->uploads[i];
        }
        old->nuploads = 0;
        free_frame(old);
    }
    g_pending_frame = frame;
    SDL_CondSignal(g_qcond);
    SDL_UnlockMutex(g_qlock);
}

/* ---- renderer --------------------------------------------------------------------------------- */
static SDL_Window *g_window;
static GLuint g_prog, g_vao, g_vbo, g_efb_fbo, g_efb_color, g_efb_depth;
static GLint g_u_mode, g_u_alpha_test, g_u_alpha_ref;
static GLuint *g_textures; /* GxTexture id -> GL name */
static uint32_t g_ntextures;

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 pos; layout(location=1) in vec4 col; layout(location=2) in vec2 uv;\n"
    "out vec4 v_col; out vec2 v_uv;\n"
    "void main() { gl_Position = pos; v_col = col; v_uv = uv; }\n";
static const char *k_fs =
    "#version 330 core\n"
    "in vec4 v_col; in vec2 v_uv; out vec4 o;\n"
    "uniform sampler2D tex; uniform int mode; uniform int alpha_test; uniform float alpha_ref;\n"
    "void main() {\n"
    "  vec4 c = v_col;\n"
    "  if (mode == 1) c = texture(tex, v_uv) * v_col;\n"
    "  else if (mode == 2) c = texture(tex, v_uv);\n"
    "  if (alpha_test == 1 && c.a < alpha_ref) discard;\n"
    "  o = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_fatal("shader compile failed: %s", log);
    }
    return s;
}

static void gl_setup(void) {
    GL_FUNCS(GL_LOAD)
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, k_vs));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, k_fs));
    glLinkProgram(g_prog);
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "tex"), 0);
    g_u_mode = glGetUniformLocation(g_prog, "mode");
    g_u_alpha_test = glGetUniformLocation(g_prog, "alpha_test");
    g_u_alpha_ref = glGetUniformLocation(g_prog, "alpha_ref");

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(GxVertex), (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(GxVertex), (void *)(4 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(GxVertex), (void *)(8 * sizeof(float)));

    /* the EFB */
    glGenTextures(1, &g_efb_color);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_WIDTH, EFB_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &g_efb_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_efb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, EFB_WIDTH, EFB_HEIGHT);
    glGenFramebuffers(1, &g_efb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_efb_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_efb_depth);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

static void upload(GxTextureUpload *u) {
    if (u->id >= g_ntextures) {
        uint32_t n = g_ntextures ? g_ntextures : 256;
        while (n <= u->id) n *= 2;
        g_textures = realloc(g_textures, sizeof *g_textures * n);
        memset(g_textures + g_ntextures, 0, sizeof *g_textures * (n - g_ntextures));
        g_ntextures = n;
    }
    if (!g_textures[u->id]) glGenTextures(1, &g_textures[u->id]);
    glBindTexture(GL_TEXTURE_2D, g_textures[u->id]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)u->width, (GLsizei)u->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, u->rgba);
}

static void apply_state(const GxState *s) {
    /* EFB y grows downwards; the GL framebuffer's y grows upwards. */
    glScissor(s->scissor[0], EFB_HEIGHT - 1 - s->scissor[3], s->scissor[2] - s->scissor[0] + 1,
              s->scissor[3] - s->scissor[1] + 1);
    if (s->z_enable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(k_depth_func[s->z_func & 7]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(s->z_update);
    glColorMask(s->color_update, s->color_update, s->color_update, s->alpha_update);
    if (s->blend_enable) {
        glEnable(GL_BLEND);
        GLenum src = k_blend_factor[s->blend_src & 7], dst = k_blend_factor[s->blend_dst & 7];
        if ((s->blend_src & 7) == 2) src = 0x0306; /* DST_COLOR */
        if ((s->blend_src & 7) == 3) src = 0x0307;
        if ((s->blend_dst & 7) == 2) dst = 0x0300; /* SRC_COLOR */
        if ((s->blend_dst & 7) == 3) dst = 0x0301;
        glBlendFunc(src, dst);
        glBlendEquation(s->blend_sub ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    } else {
        glDisable(GL_BLEND);
    }
    if (s->cull == 0) {
        glDisable(GL_CULL_FACE);
    } else {
        /* GX "front" faces are clockwise in EFB space, which is counter-clockwise after the y flip */
        glEnable(GL_CULL_FACE);
        glCullFace(s->cull == 1 ? GL_FRONT : s->cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
    }
    glUniform1i(g_u_mode, s->texture ? s->tev_mode : 0);
    glUniform1i(g_u_alpha_test, s->alpha_test);
    glUniform1f(g_u_alpha_ref, s->alpha_ref);
    if (s->texture && s->texture < g_ntextures && g_textures[s->texture]) {
        static const GLint wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
        glBindTexture(GL_TEXTURE_2D, g_textures[s->texture]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[s->tex_wrap_s & 3]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[s->tex_wrap_t & 3]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->tex_linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->tex_linear ? GL_LINEAR : GL_NEAREST);
    }
}

static void write_bmp(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = (uint32_t)w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * (uint32_t)h;
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t v;
    v = 54 + size; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &size, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = 0; y < h; y++) { /* rgba rows are bottom-up already (glReadPixels) */
        for (int x = 0; x < w; x++) {
            const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            uint8_t bgr[3] = {p[2], p[1], p[0]};
            fwrite(bgr, 1, 3, f);
        }
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

static void render(GxFrame *f) {
    for (uint32_t i = 0; i < f->nuploads; i++) upload(&f->uploads[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, EFB_WIDTH, EFB_HEIGHT);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GxVertex) * f->nverts), f->verts, GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_SCISSOR_TEST);

    uint32_t next_clear = 0;
    for (uint32_t b = 0; b <= f->nbatches; b++) {
        while (next_clear < f->nclears && f->clears[next_clear].at_batch == b) {
            GxClear *cl = &f->clears[next_clear++];
            glDisable(GL_SCISSOR_TEST);
            glColorMask(1, 1, 1, 1);
            glDepthMask(1);
            glClearColor(cl->color[0], cl->color[1], cl->color[2], cl->color[3]);
            glClearDepth(cl->depth);
            glClear((cl->clear_color ? GL_COLOR_BUFFER_BIT : 0) | (cl->clear_depth ? GL_DEPTH_BUFFER_BIT : 0));
            glEnable(GL_SCISSOR_TEST);
        }
        if (b == f->nbatches) break;
        GxBatch *bt = &f->batches[b];
        apply_state(&bt->state);
        GLenum mode = bt->kind == GX_PRIM_LINES ? GL_LINES : bt->kind == GX_PRIM_POINTS ? GL_POINTS : GL_TRIANGLES;
        glDrawArrays(mode, (GLint)bt->first, (GLsizei)bt->count);
    }
    glDisable(GL_SCISSOR_TEST);

    /* present: the XFB copy rectangle, scaled to the window */
    int ww, wh;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    int sx = f->copy_rect[0], sy = f->copy_rect[1], sw = f->copy_rect[2], sh = f->copy_rect[3];
    if (!sw || !sh) { sx = 0; sy = 0; sw = 640; sh = 480; }
    int gy0 = EFB_HEIGHT - (sy + sh), gy1 = EFB_HEIGHT - sy;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    double scale = (double)ww / sw < (double)wh / sh ? (double)ww / sw : (double)wh / sh;
    int dw = (int)(sw * scale), dh = (int)(sh * scale), dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBlitFramebuffer(sx, gy0, sx + sw, gy1, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    if (g_dump) {
        static uint8_t *pix;
        static int n;
        if (!pix) pix = malloc((size_t)640 * 528 * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
        glReadPixels(sx, gy0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        char path[256];
        snprintf(path, sizeof path, "build/GF7E01/frames/frame_%05u.bmp", f->number);
        if (n++ % 15 == 0) write_bmp(path, pix, sw, sh); /* every 15th frame */
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    SDL_GL_SwapWindow(g_window);
}

static int render_thread(void *arg) {
    (void)arg;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("Star Fox: Assault (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                1280, 960, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_GLContext gl = SDL_GL_CreateContext(g_window);
    if (!gl) rt_fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(0);
    gl_setup();
    printf("[video] OpenGL renderer ready\n");
    if (g_dump) {
#ifdef _WIN32
        system("if not exist build\\GF7E01\\frames mkdir build\\GF7E01\\frames");
#else
        system("mkdir -p build/GF7E01/frames");
#endif
    }

    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_quit = 1;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !g_controller) {
                g_controller = SDL_GameControllerOpen(e.cdevice.which);
                if (g_controller) printf("[video] controller: %s\n", SDL_GameControllerName(g_controller));
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_controller &&
                       e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controller))) {
                SDL_GameControllerClose(g_controller);
                g_controller = NULL;
                SDL_LockMutex(g_pad_lock);
                memset(&g_ctl, 0, sizeof g_ctl);
                SDL_UnlockMutex(g_pad_lock);
            }
        }
        poll_keyboard();
        poll_controller();
        SDL_LockMutex(g_qlock);
        if (!g_pending_frame) SDL_CondWaitTimeout(g_qcond, g_qlock, 16);
        GxFrame *f = g_pending_frame;
        g_pending_frame = NULL;
        SDL_UnlockMutex(g_qlock);
        if (f) {
            render(f);
            free_frame(f);
        }
    }
    return 0;
}

void video_start(int dump_frames) {
    g_dump = dump_frames;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) rt_fatal("SDL_Init: %s", SDL_GetError());
    g_pad_lock = SDL_CreateMutex();
    g_qlock = SDL_CreateMutex();
    g_qcond = SDL_CreateCond();
    SDL_CreateThread(render_thread, "render", NULL);
}

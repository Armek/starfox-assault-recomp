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
#include <stdarg.h>
#include <stddef.h>
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
    X(void, glUniform4fv, GLint, GLsizei, const GLfloat *) X(void, glUniform2f, GLint, GLfloat, GLfloat) \
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

/* ---- audio output -------------------------------------------------------------------------------- */
static SDL_AudioDeviceID g_audio;

/* 32 kHz interleaved stereo from the AI DMA (host-endian samples). */
void video_audio_push(const int16_t *pcm, int frames) {
    static FILE *wav;
    static uint32_t wav_frames;
    if (getenv("SFA_DUMP_AUDIO") && frames > 0) { /* debugging: everything played, as a WAV file */
        if (!wav) {
            wav = fopen("build/GF7E01/audio.wav", "wb");
            static const uint8_t hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                0x00,0x7D,0,0, 0x00,0xF4,0x01,0, 4,0,16,0,'d','a','t','a',0,0,0,0};
            if (wav) fwrite(hdr, 1, 44, wav);
        }
        if (wav) {
            fwrite(pcm, 4, (size_t)frames, wav);
            wav_frames += (uint32_t)frames;
            uint32_t data = wav_frames * 4, riff = 36 + data;
            long pos = ftell(wav);
            fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
            fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav);
            fseek(wav, pos, SEEK_SET);
            fflush(wav);
        }
    }
    if (!g_audio || frames <= 0) return;
    if (SDL_GetQueuedAudioSize(g_audio) > 32000 * 4 / 6) return; /* > ~150 ms queued: drop, don't lag */
    SDL_QueueAudio(g_audio, pcm, (Uint32)frames * 4);
}

/* ---- frame queue --------------------------------------------------------------------------------- */
static SDL_mutex *g_qlock;
static SDL_cond *g_qcond;
static GxFrame *g_pending_frame;
static volatile int g_quit;
static int g_dump;

int video_quit_requested(void) { return g_quit; }

/* Finished frames go back to a small pool with their arrays, so building the next frame reuses
 * memory that is already allocated and paged in (a Mission 1 frame holds ~150k vertices, ~17 MB). */
#define FRAME_POOL 4
static GxFrame *g_frame_pool[FRAME_POOL];
static int g_frame_pool_n;
static SDL_SpinLock g_frame_pool_lock;

static void free_frame(GxFrame *f) {
    for (uint32_t i = 0; i < f->nuploads; i++) free(f->uploads[i].rgba);
    f->nuploads = 0;
    SDL_AtomicLock(&g_frame_pool_lock);
    if (g_frame_pool_n < FRAME_POOL) {
        g_frame_pool[g_frame_pool_n++] = f;
        f = NULL;
    }
    SDL_AtomicUnlock(&g_frame_pool_lock);
    if (!f) return;
    free(f->uploads);
    free(f->verts);
    free(f->batches);
    free(f->clears);
    free(f);
}

GxFrame *video_new_frame(uint32_t number) {
    GxFrame *f = NULL;
    SDL_AtomicLock(&g_frame_pool_lock);
    if (g_frame_pool_n) f = g_frame_pool[--g_frame_pool_n];
    SDL_AtomicUnlock(&g_frame_pool_lock);
    if (!f) f = calloc(1, sizeof *f);
    f->nverts = f->nbatches = f->nuploads = f->nclears = 0;
    memset(f->copy_rect, 0, sizeof f->copy_rect);
    f->copy = NULL;
    f->number = number;
    return f;
}

void video_submit(GxFrame *frame) {
    if (!g_qlock) { /* --no-video: nothing renders, drop the frame */
        free_frame(frame);
        return;
    }
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

/* A mid-frame segment ending in an EFB copy to texture. Unlike whole frames it is never dropped
 * (later draws build on this EFB content), and the caller waits for the read-back pixels. */
static SDL_cond *g_done_cond;

void video_copy_segment(GxFrame *frame) {
    GxCopyRequest *req = frame->copy;
    if (!g_qlock) { /* --no-video: no EFB to read */
        memset(req->rgba, 0, (size_t)req->w * req->h * 4);
        req->done = 1;
        free_frame(frame);
        return;
    }
    SDL_LockMutex(g_qlock);
    while (g_pending_frame) SDL_CondWait(g_done_cond, g_qlock); /* let the renderer take what is queued */
    g_pending_frame = frame;
    SDL_CondSignal(g_qcond);
    while (!req->done && !g_quit) SDL_CondWaitTimeout(g_done_cond, g_qlock, 100);
    SDL_UnlockMutex(g_qlock);
}

/* ---- renderer --------------------------------------------------------------------------------- */
static SDL_Window *g_window;
static GLuint g_vao, g_vbo, g_efb_fbo, g_efb_color, g_efb_depth, g_white;
static GLuint *g_textures; /* GxTexture id -> GL name */
static uint32_t g_ntextures;

/* ---- TEV: one generated fragment shader per distinct GxTevConfig ------------------------------- */
static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 pos; layout(location=1) in vec4 c0; layout(location=2) in vec4 c1;\n"
    "layout(location=3) in vec2 t0; layout(location=4) in vec2 t1; layout(location=5) in vec2 t2;\n"
    "layout(location=6) in vec2 t3; layout(location=7) in vec2 t4; layout(location=8) in vec2 t5;\n"
    "layout(location=9) in vec2 t6; layout(location=10) in vec2 t7;\n"
    "out vec4 v_c0, v_c1; out vec2 v_t0, v_t1, v_t2, v_t3, v_t4, v_t5, v_t6, v_t7;\n"
    "void main() { gl_Position = pos; v_c0 = c0; v_c1 = c1; v_t0 = t0; v_t1 = t1; v_t2 = t2; v_t3 = t3;\n"
    "  v_t4 = t4; v_t5 = t5; v_t6 = t6; v_t7 = t7; }\n";

typedef struct {
    GxTevConfig cfg;
    GLuint prog;
    GLint u_konst, u_reg, u_aref;
} TevProgram;
static TevProgram *g_progs;
static unsigned g_nprogs, g_cap_progs;

typedef struct { char *p; size_t n, cap; } Str;
static void sadd(Str *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < s->cap - s->n) { s->n += (size_t)k; return; }
        s->cap = s->cap ? s->cap * 2 : 8192;
        s->p = realloc(s->p, s->cap);
    }
}

static const char *k_color_in[16] = {"prev.rgb", "prev.aaa", "r0.rgb", "r0.aaa", "r1.rgb", "r1.aaa", "r2.rgb", "r2.aaa",
                                     "tx.rgb", "tx.aaa", "ras.rgb", "ras.aaa", "vec3(1.0)", "vec3(0.5)", "kc", "vec3(0.0)"};
static const char *k_alpha_in[8] = {"prev.a", "r0.a", "r1.a", "r2.a", "tx.a", "ras.a", "ka", "0.0"};
static const char *k_dest[4] = {"prev", "r0", "r1", "r2"};
static const char *k_konst_frac[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};

static void konst_color(Str *s, int sel) {
    if (sel < 8) sadd(s, "vec3(%s)", k_konst_frac[sel]);
    else if (sel < 12) sadd(s, "vec3(0.0)");
    else if (sel < 16) sadd(s, "u_konst[%d].rgb", sel - 12);
    else sadd(s, "u_konst[%d].%c%c%c", sel & 3, "rgba"[(sel - 16) / 4], "rgba"[(sel - 16) / 4], "rgba"[(sel - 16) / 4]);
}
static void konst_alpha(Str *s, int sel) {
    if (sel < 8) sadd(s, "%s", k_konst_frac[sel]);
    else if (sel < 16) sadd(s, "0.0");
    else sadd(s, "u_konst[%d].%c", sel & 3, "rgba"[(sel - 16) / 4]);
}
static void swizzle(char out[5], const GxTevConfig *t, int table) {
    uint32_t a = t->ksel[table * 2], b = t->ksel[table * 2 + 1];
    out[0] = "rgba"[a & 3]; out[1] = "rgba"[(a >> 2) & 3]; out[2] = "rgba"[b & 3]; out[3] = "rgba"[(b >> 2) & 3]; out[4] = 0;
}
static const char *alpha_cmp(int f) {
    static const char *k[8] = {"false", "(A < R)", "(A == R)", "(A <= R)", "(A > R)", "(A != R)", "(A >= R)", "true"};
    return k[f & 7];
}

static char *tev_fragment_source(const GxTevConfig *t) {
    Str s = {0};
    sadd(&s, "#version 330 core\n"
             "in vec4 v_c0, v_c1; in vec2 v_t0, v_t1, v_t2, v_t3, v_t4, v_t5, v_t6, v_t7; out vec4 o;\n"
             "uniform sampler2D tex0, tex1, tex2, tex3, tex4, tex5, tex6, tex7;\n"
             "uniform vec4 u_konst[4]; uniform vec4 u_reg[4]; uniform vec2 u_aref;\n"
             "float q8(float x) { return floor(clamp(x, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }\n"
             "vec3 q8(vec3 x) { return floor(clamp(x, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }\n"
             "void main() {\n"
             "  vec4 prev = u_reg[0], r0 = u_reg[1], r1 = u_reg[2], r2 = u_reg[3];\n"
             "  vec4 tx = vec4(1.0), ras = vec4(0.0); vec3 kc; float ka;\n");
    static const float scale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
    static const char *bias[3] = {"0.0", "0.5", "-0.5"};
    int last_cdest = 0, last_adest = 0;
    for (int i = 0; i < t->nstages && i < 16; i++) {
        uint32_t tr = (t->tref[i / 2] >> ((i & 1) * 12)) & 0xFFF, ce = t->color_env[i], ae = t->alpha_env[i];
        uint32_t ks = t->ksel[i / 2];
        int map = tr & 7, tc = (tr >> 3) & 7, ten = (tr >> 6) & 1, chan = (tr >> 7) & 7;
        int kcs = (i & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31, kas = (i & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
        char rsw[5], tsw[5];
        swizzle(rsw, t, ae & 3);
        swizzle(tsw, t, (ae >> 2) & 3);
        sadd(&s, "  // stage %d\n", i);
        if (ten) sadd(&s, "  tx = texture(tex%d, v_t%d).%s;\n", map, tc, tsw);
        else sadd(&s, "  tx = vec4(1.0);\n");
        sadd(&s, "  ras = %s;\n", chan == 0 ? "v_c0" : chan == 1 ? "v_c1" : "vec4(0.0)");
        if (chan <= 1) sadd(&s, "  ras = ras.%s;\n", rsw);
        sadd(&s, "  kc = "); konst_color(&s, kcs); sadd(&s, "; ka = "); konst_alpha(&s, kas); sadd(&s, ";\n");

        /* color */
        int cd = ce & 15, cc = (ce >> 4) & 15, cb = (ce >> 8) & 15, ca = (ce >> 12) & 15;
        int cbias = (ce >> 16) & 3, csub = (ce >> 18) & 1, cclamp = (ce >> 19) & 1, cscale = (ce >> 20) & 3, cdest = (ce >> 22) & 3;
        if (cbias != 3) {
            sadd(&s, "  vec3 cr%d = (%s %c mix(q8(%s), q8(%s), q8(%s)) + vec3(%s)) * %.3f;\n", i, k_color_in[cd],
                 csub ? '-' : '+', k_color_in[ca], k_color_in[cb], k_color_in[cc], bias[cbias], scale[cscale]);
        } else { /* compare mode: d + (a OP b ? c : 0) */
            int op = (cscale << 1) | csub;
            const char *A = k_color_in[ca], *B = k_color_in[cb];
            switch (op) {
            case 0: case 1: sadd(&s, "  bool cm%d = q8(%s).r %s q8(%s).r;\n", i, A, op & 1 ? "==" : ">", B); break;
            case 2: case 3: sadd(&s, "  bool cm%d = dot(q8(%s).rg, vec2(1.0, 256.0)) %s dot(q8(%s).rg, vec2(1.0, 256.0));\n", i, A, op & 1 ? "==" : ">", B); break;
            case 4: case 5: sadd(&s, "  bool cm%d = dot(q8(%s), vec3(1.0, 256.0, 65536.0)) %s dot(q8(%s), vec3(1.0, 256.0, 65536.0));\n", i, A, op & 1 ? "==" : ">", B); break;
            default: break;
            }
            if (op >= 6)
                sadd(&s, "  vec3 cr%d = %s + mix(vec3(0.0), %s, vec3(%s(q8(%s), q8(%s))));\n", i, k_color_in[cd],
                     k_color_in[cc], op & 1 ? "equal" : "greaterThan", A, B);
            else
                sadd(&s, "  vec3 cr%d = %s + (cm%d ? %s : vec3(0.0));\n", i, k_color_in[cd], i, k_color_in[cc]);
        }
        sadd(&s, "  cr%d = clamp(cr%d, %s);\n", i, i, cclamp ? "0.0, 1.0" : "-4.0, 4.0");

        /* alpha */
        int ad = (ae >> 4) & 7, ac = (ae >> 7) & 7, ab = (ae >> 10) & 7, aa = (ae >> 13) & 7;
        int abias = (ae >> 16) & 3, asub = (ae >> 18) & 1, aclamp = (ae >> 19) & 1, ascale = (ae >> 20) & 3, adest = (ae >> 22) & 3;
        if (abias != 3) {
            sadd(&s, "  float ar%d = (%s %c mix(q8(%s), q8(%s), q8(%s)) + %s) * %.3f;\n", i, k_alpha_in[ad], asub ? '-' : '+',
                 k_alpha_in[aa], k_alpha_in[ab], k_alpha_in[ac], bias[abias], scale[ascale]);
        } else {
            int op = (ascale << 1) | asub;
            sadd(&s, "  float ar%d = %s + ((q8(%s) %s q8(%s)) ? %s : 0.0);\n", i, k_alpha_in[ad], k_alpha_in[aa],
                 op & 1 ? "==" : ">", k_alpha_in[ab], k_alpha_in[ac]);
        }
        sadd(&s, "  ar%d = clamp(ar%d, %s);\n", i, i, aclamp ? "0.0, 1.0" : "-4.0, 4.0");
        sadd(&s, "  %s.rgb = cr%d; %s.a = ar%d;\n", k_dest[cdest], i, k_dest[adest], i);
        last_cdest = cdest;
        last_adest = adest;
    }
    sadd(&s, "  vec4 outc = vec4(%s.rgb, %s.a);\n", k_dest[last_cdest], k_dest[last_adest]);
    int comp0 = (t->alpha_cmp >> 16) & 7, comp1 = (t->alpha_cmp >> 19) & 7, logic = (t->alpha_cmp >> 22) & 3;
    static const char *logic_op[4] = {"&&", "||", "!=", "=="};
    char c0[64], c1[64];
    snprintf(c0, sizeof c0, "%s", alpha_cmp(comp0));
    snprintf(c1, sizeof c1, "%s", alpha_cmp(comp1));
    sadd(&s, "  float A = floor(clamp(outc.a, 0.0, 1.0) * 255.0 + 0.5);\n");
    sadd(&s, "  bool p0, p1; { float R = floor(u_aref.x * 255.0 + 0.5); p0 = %s; }\n", c0);
    sadd(&s, "  { float R = floor(u_aref.y * 255.0 + 0.5); p1 = %s; }\n", c1);
    sadd(&s, "  if (!(p0 %s p1)) discard;\n", logic_op[logic]);
    sadd(&s, "  o = clamp(outc, 0.0, 1.0);\n}\n");
    return s.p;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "%s\n", src);
        rt_fatal("shader compile failed: %s", log);
    }
    return s;
}

static GLuint g_vs_shader;

static TevProgram *tev_program(const GxTevConfig *cfg) {
    for (unsigned i = 0; i < g_nprogs; i++)
        if (!memcmp(&g_progs[i].cfg, cfg, sizeof *cfg)) return &g_progs[i];
    if (g_nprogs == g_cap_progs) {
        g_cap_progs = g_cap_progs ? g_cap_progs * 2 : 64;
        g_progs = realloc(g_progs, sizeof *g_progs * g_cap_progs);
    }
    TevProgram *p = &g_progs[g_nprogs++];
    p->cfg = *cfg;
    char *fs = tev_fragment_source(cfg);
    p->prog = glCreateProgram();
    glAttachShader(p->prog, g_vs_shader);
    glAttachShader(p->prog, compile(GL_FRAGMENT_SHADER, fs));
    free(fs);
    glLinkProgram(p->prog);
    glUseProgram(p->prog);
    for (int i = 0; i < 8; i++) {
        char name[8];
        snprintf(name, sizeof name, "tex%d", i);
        glUniform1i(glGetUniformLocation(p->prog, name), i);
    }
    p->u_konst = glGetUniformLocation(p->prog, "u_konst");
    p->u_reg = glGetUniformLocation(p->prog, "u_reg");
    p->u_aref = glGetUniformLocation(p->prog, "u_aref");
    return p;
}

static void gl_setup(void) {
    GL_FUNCS(GL_LOAD)
    g_vs_shader = compile(GL_VERTEX_SHADER, k_vs);

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(GxVertex), (void *)offsetof(GxVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(GxVertex), (void *)offsetof(GxVertex, c0));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(GxVertex), (void *)offsetof(GxVertex, c1));
    for (int i = 0; i < 8; i++) {
        glEnableVertexAttribArray(3 + i);
        glVertexAttribPointer(3 + i, 2, GL_FLOAT, 0, sizeof(GxVertex), (void *)(offsetof(GxVertex, uv) + i * 2 * sizeof(float)));
    }

    /* 1x1 white texture for texture maps that have nothing bound */
    static const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

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
    TevProgram *p = tev_program(&s->tev);
    glUseProgram(p->prog);
    glUniform4fv(p->u_konst, 4, &s->konst[0][0]);
    glUniform4fv(p->u_reg, 4, &s->reg[0][0]);
    glUniform2f(p->u_aref, s->alpha_ref[0], s->alpha_ref[1]);
    static const GLint wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    for (int m = 0; m < 8; m++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)m);
        uint32_t id = s->texture[m];
        if (id && id < g_ntextures && g_textures[id]) {
            glBindTexture(GL_TEXTURE_2D, g_textures[id]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[s->wrap_s[m] & 3]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[s->wrap_t[m] & 3]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->linear[m] ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->linear[m] ? GL_LINEAR : GL_NEAREST);
        } else {
            glBindTexture(GL_TEXTURE_2D, g_white);
        }
    }
    glActiveTexture(GL_TEXTURE0);
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

    if (f->copy) { /* EFB copy to texture: read the rectangle back for the guest, don't present */
        GxCopyRequest *r = f->copy;
        static uint8_t *tmp;
        static size_t tmp_size;
        size_t bytes = (size_t)r->w * r->h * 4;
        if (bytes > tmp_size) { tmp = realloc(tmp, bytes); tmp_size = bytes; }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
        glReadPixels((GLint)r->x, (GLint)(EFB_HEIGHT - (r->y + r->h)), (GLsizei)r->w, (GLsizei)r->h, GL_RGBA,
                     GL_UNSIGNED_BYTE, tmp);
        for (uint32_t y = 0; y < r->h; y++) /* GL rows are bottom-up */
            memcpy(r->rgba + (size_t)y * r->w * 4, tmp + (size_t)(r->h - 1 - y) * r->w * 4, (size_t)r->w * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_LockMutex(g_qlock);
        r->done = 1;
        SDL_CondBroadcast(g_done_cond);
        SDL_UnlockMutex(g_qlock);
        return;
    }

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
        SDL_CondBroadcast(g_done_cond); /* a waiting copy segment may queue now */
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
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = {0}, have;
        want.freq = 32000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audio) {
            SDL_PauseAudioDevice(g_audio, 0);
            printf("[audio] %d Hz stereo\n", have.freq);
        } else {
            printf("[audio] no audio device: %s\n", SDL_GetError());
        }
    }
    g_pad_lock = SDL_CreateMutex();
    g_qlock = SDL_CreateMutex();
    g_qcond = SDL_CreateCond();
    g_done_cond = SDL_CreateCond();
    SDL_CreateThread(render_thread, "render", NULL);
}

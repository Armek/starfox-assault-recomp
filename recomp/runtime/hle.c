/* hle.c - host reimplementations of guest functions listed in hle_overrides.txt.
 *
 * Calling convention (PowerPC EABI): arguments in r3-r10 / f1-f8, return value in r3 (or f1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "funcs.h"
#include "runtime.h"
#include "gx.h"
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h> /* _mm_getcsr / _mm_setcsr */
#endif

/* int __write_console(__file_handle handle, unsigned char *buf, size_t *count, __idle_proc idle) */
static void write_console(Ctx *c) {
    uint32_t buf = c->r[4], count = rd32(c->r[5]);
    for (uint32_t i = 0; i < count; i++) rt_console_putc((char)rd8(buf + i));
    c->r[3] = 0; /* __no_io_error */
}
HLE(__write_console) { write_console(c); }
HLE(__TRK_write_console) { write_console(c); }

/* void __OSInitAudioSystem(void): DSP boot handshake. Nothing to do until audio has a host backend. */
HLE(__OSInitAudioSystem) { (void)c; }

/* void __OSInitMemoryProtection(void): MMU/BAT and MI setup through RealMode()/rfi. */
HLE(__OSInitMemoryProtection) { (void)c; }

/* void PPCSetFpNonIEEEMode(void): OSInit calls it, so the game always runs with FPSCR NI set: a float
 * result too small for a normal number is flushed to zero instead of becoming a denormal (and denormal
 * operands count as zero). Code relies on it. Mission 1's last boss steers toward a point by normalizing
 * (target - position); as it arrives, the squared distance (~1e-39) must become exactly 0, so the
 * distance is 0 and the step is skipped. Kept as a denormal, PSVECNormalize's Newton step overflowed to
 * infinity, the boss's position went to -inf and then NaN, it vanished, and the debris it sheds piled
 * up by the thousand (with NaN positions they never landed, so never expired).
 *
 * The host FPU gets the same mode: on x86, MXCSR flush-to-zero and denormals-are-zero, which also cover
 * the float<->double conversions the generated code rounds through (the generated code computes in
 * double and rounds singles with a conversion). The mode is per host thread: it is applied here and
 * when each guest thread starts. */
static volatile int g_fp_ni;

void rt_fp_apply_mode(void) {
    if (!g_fp_ni) return;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_setcsr(_mm_getcsr() | 0x8040u); /* FTZ (bit 15) | DAZ (bit 6) */
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24))); /* FZ */
#endif
}

HLE(PPCSetFpNonIEEEMode) {
    c->fpscr |= 0x4u; /* NI */
    g_fp_ni = 1;
    rt_fp_apply_mode();
}

/* void GXDrawDone(void): flush and wait for the GPU (here, the GX thread). */
HLE(GXDrawDone) {
    F_GXFlush(c);
    gx_sync(); /* the commands run on the GX thread: wait for them, like waiting for the GPU */
}

/* u32 PADRead(PADStatus status[4]). PADStatus (12 bytes): u16 button; s8 stickX, stickY,
 * substickX, substickY; u8 triggerL, triggerR, analogA, analogB; s8 err. Input comes from the
 * host (keyboard and game controllers, see video.c). */
#define PAD_ERR_NONE 0
#define PAD_ERR_NO_CONTROLLER (-1)
HLE(PADRead) {
    uint32_t status = c->r[3];
    for (int port = 0; port < 4; port++) {
        uint32_t p = status + (uint32_t)port * 12;
        PadState in;
        video_get_pad(port, &in);
        for (uint32_t i = 0; i < 12; i++) wr8(p + i, 0);
        wr16(p, in.buttons);
        wr8(p + 2, (uint8_t)in.stick_x);
        wr8(p + 3, (uint8_t)in.stick_y);
        wr8(p + 4, (uint8_t)in.substick_x);
        wr8(p + 5, (uint8_t)in.substick_y);
        wr8(p + 6, in.trigger_l);
        wr8(p + 7, in.trigger_r);
        wr8(p + 10, (uint8_t)(in.connected ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER));
    }
    c->r[3] = 0; /* no motor/reset bits */
}

/* BOOL __OSReadROM(void *buffer, s32 length, s32 offset): read the boot ROM (IPL) over EXI.
 * Games only read the system fonts from it: the SJIS font at 0x1AFF00 and the ANSI font at
 * 0x1FCF00, Yay0-compressed. They come from user/GF7E01/ipl.bin (a dump of your own console's
 * boot ROM), else from font files in user/fonts/, else from the replacement fonts compiled into
 * the program (recomp/fonts: Dolphin's font_western.bin and font_japanese.bin, generated from
 * Droid Sans, Apache 2.0). */
#define IPL_SIZE 0x200000u
static uint8_t *g_ipl;

static long load_into(const char *path, uint8_t *dst, uint32_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(dst, 1, max, f);
    fclose(f);
    return n;
}

static void ipl_init(void) {
    if (g_ipl) return;
    g_ipl = calloc(1, IPL_SIZE);
    if (load_into("user/GF7E01/ipl.bin", g_ipl, IPL_SIZE) > 0) {
        printf("[rom] using the boot ROM dump user/GF7E01/ipl.bin\n");
        return;
    }
    long a = load_into("user/fonts/font_western.bin", g_ipl + 0x1FCF00, IPL_SIZE - 0x1FCF00);
    long s = load_into("user/fonts/font_japanese.bin", g_ipl + 0x1AFF00, 0x1FCF00 - 0x1AFF00);
    if (a > 0 || s > 0) {
        printf("[rom] system fonts from user/fonts (western %ld bytes, japanese %ld bytes)\n", a, s);
        return;
    }
    extern const unsigned char g_font_western[], g_font_japanese[];
    extern const size_t g_font_western_size, g_font_japanese_size;
    memcpy(g_ipl + 0x1FCF00, g_font_western, g_font_western_size);
    memcpy(g_ipl + 0x1AFF00, g_font_japanese, g_font_japanese_size);
    printf("[rom] built-in system fonts (Droid Sans, Apache 2.0)\n");
}

HLE(__OSReadROM) {
    uint32_t buf = c->r[3], len = c->r[4], off = c->r[5];
    ipl_init();
    for (uint32_t i = 0; i < len; i++) wr8(buf + i, off + i < IPL_SIZE ? g_ipl[off + i] : 0);
    c->r[3] = 1; /* TRUE */
}

/* --stage=N starts stage N of the stage table instead of Mission 1. The table at 0x80223560 has 20-byte
 * records (byte 0 = mission - 1, byte 5 = play type: 0 ground, 1 flight); the first stages of missions
 * 1-10 are 0, 5, 9, 11, 15, 19, 20, 24, 27 and 29.
 *
 * fn_8001D5B4(stage) sets the current stage: it copies the record into the game info at 0x802A2B60
 * (+0x13C stage, +0x140 mission, +0x13F play type, which picks the gameplay module m8 or m9). Replacing
 * the first request for stage 0 there makes the game set up stage N the way its own menus would: the
 * briefing, the gameplay module and the stage start all follow from it. */
int g_start_stage = -1;

void rt_hook_8001D5B4(Ctx *c) {
    static int replaced;
    if (g_start_stage >= 0 && !replaced && (c->r[3] & 0xFF) == 0 && g_start_stage < 0x3B) {
        replaced = 1;
        c->r[3] = (uint32_t)g_start_stage;
        printf("[stage] --stage=%d: current stage set to %d (play type %u)\n", g_start_stage, g_start_stage,
               rd8(0x80223565u + (uint32_t)g_start_stage * 0x14));
        /* later parts of a mission take their resource list from RAM the earlier parts fill in
         * (the table's pointer is outside the DOL's read-only data), so they can't start cold */
        if (rd32(0x80229748u + (uint32_t)g_start_stage * 12) >= 0x80380000u)
            printf("[stage] warning: stage %d is a later part of its mission and needs the earlier parts' state; "
                   "start from a mission's first stage (0, 5, 9, 11, 15, 19, 20, 24, 27, 29)\n", g_start_stage);
        fflush(stdout);
    }
}

/* Stage start, fn_8005F470(state, stage, a, b): log each stage the game starts; from here on
 * --auto-press plays (fires and moves) instead of tapping START. */
void rt_hook_8005F470(Ctx *c) {
    video_set_auto_play(1);
    uint32_t stage = c->r[4];
    printf("[stage] start stage %d (mission %d, part %02X)\n", (int)stage,
           stage < 0x3B ? rd8(0x80223560u + stage * 0x14) + 1 : -1, stage < 0x3B ? rd8(0x80223561u + stage * 0x14) : 0);
    fflush(stdout);
}

/* fn_8000ECC8(ctl): the game's main state machine, called every frame with its control block; ctl+0x34
 * is the state (16 = a stage part is being played). SFA_GAME_STATES=1 logs each change with the
 * current stage.
 *
 * In state 16 the game watches the part's outcome, game info +0x142 (0x802A2CA2), which the gameplay
 * code sets: 7 when a part is finished. The state machine then plays the part's closing scene and moves
 * to the next part through the table at 0x8021CF38, as in normal play. SFA_CLEAR_PART=<seconds>
 * (testing; "seconds:stop_stage" leaves stage stop_stage and later alone) sets it once a part has been
 * played that long, to reach later parts and bosses with the
 * state the earlier parts build up. */
uint32_t g_game_state; /* the main state machine's state (16 = a part is being played), set below */

void rt_hook_8000ECC8(Ctx *c) {
    static int log = -1;
    static double clear_after = -1;
    static int stop_stage;
    static uint32_t last = 0xFFFFFFFFu;
    static uint64_t entered;
    if (log < 0) {
        log = getenv("SFA_GAME_STATES") != NULL;
        const char *s = getenv("SFA_CLEAR_PART"); /* "seconds" or "seconds:stop_stage" */
        clear_after = s ? atof(s) : 0;
        const char *colon = s ? strchr(s, ':') : NULL;
        stop_stage = colon ? atoi(colon + 1) : 0x3B;
    }
    uint32_t st = rd32(c->r[3] + 0x34);
    g_game_state = st;
    if (st != last) {
        if (log) {
            printf("[state] %d -> %u (stage %u, ctl %08X)\n", (int)last, st, rd8(0x802A2B60u + 0x13C), c->r[3]);
            fflush(stdout);
        }
        last = st;
        entered = rt_timebase();
        /* --auto-press plays only while a part runs (state 16); results screens and story scenes between
         * parts and missions get the menu taps again */
        video_set_auto_play(st == 16);
    }
    if (st == 16 && clear_after > 0 && rd8(0x802A2B60u + 0x13C) < stop_stage &&
        (double)(rt_timebase() - entered) / 40500000.0 >= clear_after) {
        wr8(0x802A2B60u + 0x142, 7); /* the part's outcome: finished */
        printf("[stage] SFA_CLEAR_PART: part of stage %u marked cleared\n", rd8(0x802A2B60u + 0x13C));
        fflush(stdout);
        entered = rt_timebase(); /* once per part */
    }
    /* SFA_COUNT_CALLS (see runtime.c): report every 10 s from the SFA_CLEAR_PART stop stage on */
    static uint64_t next_count;
    if (g_count_hi && rd8(0x802A2B60u + 0x13C) >= stop_stage && rt_timebase() >= next_count) {
        if (next_count) rt_count_report();
        next_count = rt_timebase() + 10ull * 40500000ull;
    }
    /* SFA_RAM_SNAPSHOTS=<seconds> (testing): while a part is played from the SFA_CLEAR_PART stop stage on,
     * save main RAM to build/GF7E01/ram_NN.bin that often, to compare the game's state over time */
    static double snap_every = -1;
    static uint64_t next_snap;
    static int snap_n;
    if (snap_every < 0) { const char *s = getenv("SFA_RAM_SNAPSHOTS"); snap_every = s ? atof(s) : 0; }
    if (snap_every > 0 && st == 16 && rd8(0x802A2B60u + 0x13C) >= stop_stage && rt_timebase() >= next_snap) {
        char path[64];
        snprintf(path, sizeof path, "build/GF7E01/ram_%02d.bin", snap_n++);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(g_mem, 1, PPC_MEM_SIZE, f); fclose(f); }
        printf("[stage] RAM snapshot %s (stage %u)\n", path, rd8(0x802A2B60u + 0x13C));
        fflush(stdout);
        next_snap = rt_timebase() + (uint64_t)(snap_every * 40500000.0);
    }
}

/* 60 fps option (launcher "Frame rate: 60 fps", or SFA_60FPS=1; SFA_60FPS=0 turns it off).
 *
 * The game keeps its target frame rate at 0x80385AF4: 60 in flight, 30 for on-foot parts (set as the stage
 * loads) and in-engine cutscenes. fn_8003AEB0(), once a frame, derives from it the frame period (0x80385B04,
 * in VI units of 0x444) and the time-step constants the game code uses, and each frame then advances game
 * time by one step: 1/60 or 1/30 s. So the game is correct at either rate as long as frames keep up (measured:
 * forced to 60, Mission 2's game clock advanced exactly frames / 60), but a PC that can't hold 60 would run the
 * game slower than real time. While a part is played (after its first 5 s, which have loading hitches), the
 * hook therefore checks the frame rate: below about 54
 * for 2 s, it gives the game its own rate back for 20 s, then tries 60 again (waiting twice as long after each
 * failure, up to 160 s; a new stage starts over at 20 s). SFA_FPS_LOG=1 logs it all. */
int g_force_60fps = -1; /* set by runtime.c from the launcher setting; the environment wins */

void rt_hook_8003AEB0(Ctx *c) {
    (void)c;
    static int log = -1, force;
    static uint32_t last, native = 30;
    if (log < 0) {
        log = getenv("SFA_FPS_LOG") != NULL;
        const char *e = getenv("SFA_60FPS");
        force = e ? atoi(e) != 0 : g_force_60fps > 0;
        if (force) printf("[fps-rate] 60 fps option on\n");
    }
    uint32_t rate = rd32(0x80385AF4u);
    if (rate != last) {
        if (log) {
            printf("[fps-rate] game frame rate %u -> %u (stage %u, state %u)\n", last, rate, rd8(0x802A2B60u + 0x13C),
                   g_game_state);
            fflush(stdout);
        }
        last = rate;
    }
    if (log) { /* every 10 s: frames, how far the game clock (0x802A2D3C, seconds) moved, and the rate in use */
        static uint64_t rep_start;
        static uint32_t rep_frames, rep_rate60;
        static float rep_clock;
        uint64_t t = rt_timebase();
        uint32_t cw = rd32(0x802A2D3Cu);
        float clock;
        memcpy(&clock, &cw, 4);
        if (!rep_start) { rep_start = t; rep_clock = clock; rep_frames = 0; rep_rate60 = 0; }
        rep_frames++;
        rep_rate60 += rate == 60; /* as the frame began: 60 unless the game (or a fallback) set 30 since */
        if (t - rep_start >= 10ull * 40500000ull) {
            double secs = (double)(t - rep_start) / 40500000.0;
            printf("[fps-rate] 10 s: stage %u state %u: %.1f fps, game clock +%.2f s (%.2fx real time), "
                   "%u%% of frames at the 60 rate\n", rd8(0x802A2B60u + 0x13C), g_game_state, rep_frames / secs,
                   clock - rep_clock, (clock - rep_clock) / secs, 100 * rep_rate60 / rep_frames);
            fflush(stdout);
            rep_start = t;
            rep_clock = clock;
            rep_frames = rep_rate60 = 0;
        }
    }
    if (!force) return;
    if (rate != 60) native = rate; /* the game asked for another rate since the last frame */
    /* keeping up? frames counted per second of wall time while a part is played */
    static uint64_t window_start, retry_at;
    static uint32_t frames, slow_secs, wait_secs = 20, stage = 0xFFFFFFFFu;
    static int backed_off;
    uint64_t now = rt_timebase();
    uint32_t cur_stage = rd8(0x802A2B60u + 0x13C);
    if (cur_stage != stage) { stage = cur_stage; wait_secs = 20; } /* a new stage: start with a short wait */
    if (backed_off) {
        if (now < retry_at) return; /* the game's own rate */
        backed_off = 0;
        slow_secs = 0;
        window_start = 0;
        printf("[fps-rate] trying 60 fps again\n");
    }
    static uint64_t playing_since;
    if (g_game_state != 16) { window_start = 0; slow_secs = 0; playing_since = 0; }
    else if (!playing_since) playing_since = now;
    else if (now - playing_since < 5ull * 40500000ull) { /* the first seconds of play: loading hitches, shaders */ }
    else if (!window_start) { window_start = now; frames = 0; }
    else if (++frames, now - window_start >= 40500000ull) {
        double fps = frames * 40500000.0 / (double)(now - window_start);
        slow_secs = fps < 54.0 ? slow_secs + 1 : 0;
        window_start = now;
        frames = 0;
        if (slow_secs >= 2) {
            backed_off = 1;
            retry_at = now + (uint64_t)wait_secs * 40500000ull;
            if (rd32(0x80385AF4u) == 60) wr32(0x80385AF4u, native);
            last = native;
            printf("[fps-rate] can't hold 60 fps here (%.0f): back to the game's own %u fps for %u s\n", fps, native,
                   wait_secs);
            if (wait_secs < 160) wait_secs *= 2; /* failing again soon: wait longer, so it doesn't keep switching */
            fflush(stdout);
            return;
        }
    }
    if (rate != 60) {
        wr32(0x80385AF4u, 60);
        last = 60;
    }
}

/* PSVECNormalize(src, dst): with SFA_VEC_DEBUG=1, log inputs that aren't zero but whose squared length
 * is zero in single precision (normalising them gives infinities), with the caller. */
void rt_hook_8013B2EC(Ctx *c) {
    static int dbg = -1, shown;
    if (dbg < 0) dbg = getenv("SFA_VEC_DEBUG") != NULL;
    if (!dbg || shown >= 40) return;
    float v[3];
    uint32_t w[3];
    for (int k = 0; k < 3; k++) { w[k] = rd32(c->r[3] + 4 * (uint32_t)k); memcpy(&v[k], &w[k], 4); }
    float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (len2 == 0.0f && (v[0] != 0 || v[1] != 0 || v[2] != 0)) {
        shown++;
        printf("[vec] PSVECNormalize of a tiny vector (%g, %g, %g) [%08X %08X %08X] from %s\n", v[0], v[1], v[2], w[0],
               w[1], w[2], rt_func_containing(c->lr));
        fflush(stdout);
    }
}

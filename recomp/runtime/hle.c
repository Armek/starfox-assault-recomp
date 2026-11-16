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

/* void GXDrawDone(void): flush and wait for the GPU. With no GPU, the work is done immediately. */
HLE(GXDrawDone) { F_GXFlush(c); }

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
 * boot ROM) or, failing that, from free replacement fonts in user/fonts/ (Dolphin ships
 * font_western.bin and font_japanese.bin, Apache 2.0, based on Droid Sans). */
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
    if (a > 0 || s > 0)
        printf("[rom] system fonts from user/fonts (western %ld bytes, japanese %ld bytes)\n", a, s);
    else
        printf("[rom] no system font found: text drawn with OSGetFontTexture will be blank "
               "(put font_western.bin in user/fonts or an IPL dump at user/GF7E01/ipl.bin)\n");
}

HLE(__OSReadROM) {
    uint32_t buf = c->r[3], len = c->r[4], off = c->r[5];
    ipl_init();
    for (uint32_t i = 0; i < len; i++) wr8(buf + i, off + i < IPL_SIZE ? g_ipl[off + i] : 0);
    c->r[3] = 1; /* TRUE */
}

/* Stage start, fn_8005F470(state, stage, a, b): log every stage, and with --stage=N replace the
 * first start of stage 0 (Mission 1, part 1) with stage N. The stage table at 0x80223560 has 20-byte
 * records whose first byte is the mission number - 1; first stages of missions 1-10 are
 * 0, 5, 9, 11, 15, 19, 20, 24, 27 and 29. The callers keep the stage in state+0x5A too. */
int g_start_stage = -1;

void rt_hook_8005F470(Ctx *c) {
    static int replaced;
    if (g_start_stage >= 0 && !replaced && c->r[4] == 0 && g_start_stage < 0x3B) {
        replaced = 1;
        printf("[stage] --stage=%d: the game still asked for stage 0; starting stage %d instead\n", g_start_stage,
               g_start_stage);
        c->r[4] = (uint32_t)g_start_stage;
        if (ppc_is_ram(c->r[3] + 0x5A) && rd8(c->r[3] + 0x5A) == 0) wr8(c->r[3] + 0x5A, (uint8_t)g_start_stage);
    }
    uint32_t stage = c->r[4];
    printf("[stage] start stage %d (mission %d, part %02X)\n", (int)stage,
           stage < 0x3B ? rd8(0x80223560u + stage * 0x14) + 1 : -1, stage < 0x3B ? rd8(0x80223561u + stage * 0x14) : 0);
    fflush(stdout);
}

/* The briefing module m5 reads its mission from its REL argument block (0x8029E440 + 0xC) as
 * (mission - 1) * 2 and loads that mission's packs. The loader fn_8001AB7C fills that block right
 * after OSLink from the module's pending-load record (a table of pointers at 0x8029E2C0; the record
 * holds the module at +0x38 and the first argument at +0x84). With --stage=N, when m5 links, find its
 * record and set the argument to stage N's mission, so the mission is set up the way the menus would. */
void rt_on_rel_linked(const char *name, uint32_t module) {
    static int done;
    if (g_start_stage < 0 || done || strcmp(name, "m5") != 0) return;
    done = 1;
    uint32_t mission0 = rd8(0x80223560u + (uint32_t)g_start_stage * 0x14);
    if (mission0 >= 9) { /* the disc has briefings b0101-b0901 only */
        printf("[stage] --stage=%d: mission %u has no briefing; keeping Mission 1's\n", g_start_stage, mission0 + 1);
        return;
    }
    for (uint32_t k = 0; k < 96; k++) {
        uint32_t rec = rd32(0x8029E2C0u + 4 * k);
        if (!ppc_is_ram(rec) || rd32(rec + 0x38) != module) continue;
        uint32_t old = rd32(rec + 0x84), v = mission0 * 2 + (old & 1);
        wr32(rec + 0x84, v);
        printf("[stage] --stage=%d: briefing argument %u -> %u (mission %u)\n", g_start_stage, old, v, mission0 + 1);
        return;
    }
    printf("[stage] --stage=%d: m5's load record not found; the briefing stays on Mission 1\n", g_start_stage);
}

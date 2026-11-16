/* hle.c - host reimplementations of guest functions listed in hle_overrides.txt.
 *
 * Calling convention (PowerPC EABI): arguments in r3-r10 / f1-f8, return value in r3 (or f1).
 */
#include <stdio.h>
#include <stdlib.h>
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

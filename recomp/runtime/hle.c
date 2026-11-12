/* hle.c - host reimplementations of guest functions listed in hle_overrides.txt.
 *
 * Calling convention (PowerPC EABI): arguments in r3-r10 / f1-f8, return value in r3 (or f1).
 */
#include <stdio.h>
#include "funcs.h"
#include "runtime.h"

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

/* void VIWaitForRetrace(void). Until the VI interrupt is emulated, advance the SDK's retrace
 * counter (read by VIGetRetraceCount, lbl_80386664) and sleep one 60 Hz frame. */
#define VI_RETRACE_COUNT 0x80386664u
HLE(VIWaitForRetrace) {
    (void)c;
    wr32(VI_RETRACE_COUNT, rd32(VI_RETRACE_COUNT) + 1);
    rt_sleep_us(16683);
}

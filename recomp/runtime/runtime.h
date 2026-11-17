/* runtime.h - services shared by the host runtime's source files. */
#ifndef RUNTIME_H
#define RUNTIME_H

#include "ppc.h"

/* Define a host implementation of a guest function listed in hle_overrides.txt. */
#define HLE(name) void F_##name(Ctx *c)

void rt_fatal(const char *fmt, ...);
void rt_console_putc(char ch);
const char *rt_func_name(uint32_t addr);
void rt_dump_trace(void);
void rt_sleep_us(unsigned us);

/* Boot */
void rt_boot_setup(const char *root);

/* Threads and interrupts (threads.c) */
void rt_threads_init(Ctx *main_ctx);
Ctx *rt_current_ctx(void);
void rt_raise_interrupt(int n);

/* Devices (runtime.c) */
void rt_vi_retrace(void);
uint32_t rt_mmio_get(uint32_t off, int size);
void rt_mmio_set(uint32_t off, uint32_t v, int size);

/* ARAM, audio interface and AX (runtime.c, ax.c) */
#define RT_ARAM_SIZE 0x01000000u
const uint8_t *rt_aram(void);
void rt_ai_tick(void);
uint64_t rt_ai_next_deadline(void);  /* timebase tick when the current AI buffer ends, 0 if idle */
void rt_ax_command_list(uint32_t cl);
void video_audio_push(const int16_t *interleaved_be_ok, int frames);

/* Disc drive (dvd.c) */
void rt_dvd_init(const char *root);
int rt_dvd_write(uint32_t off, uint32_t v, int size);
void rt_dvd_stats(unsigned long *reads, unsigned long *bytes);

/* hle.c: --stage=N starts stage N of the game's stage table in place of Mission 1's first stage */
extern int g_start_stage;

#endif

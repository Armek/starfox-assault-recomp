/* runtime.h - services shared by the host runtime's source files. */
#ifndef RUNTIME_H
#define RUNTIME_H

#include "ppc.h"

/* Define a host implementation of a guest function listed in hle_overrides.txt. */
#define HLE(name) void F_##name(Ctx *c)

void rt_fatal(const char *fmt, ...);
void rt_console_putc(char ch);
const char *rt_func_name(uint32_t addr);
const char *rt_func_containing(uint32_t addr); /* name of the function holding a code address */
void rt_count_report(void); /* SFA_COUNT_CALLS: print and reset the call counts */
void rt_fp_apply_mode(void); /* the guest's FPU mode on this host thread (see hle.c) */
void rt_prof_stall(uint32_t gap_ms); /* SFA_PROFILE: report what the guest ran during a frame hitch */
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
void rt_dvd_init(const char *root);        /* root: an extracted disc folder or a disc image */
uint8_t *rt_disc_alloc(uint32_t offset, uint32_t len); /* disc bytes into a new host buffer */
int rt_dvd_write(uint32_t off, uint32_t v, int size);
void rt_dvd_stats(unsigned long *reads, unsigned long *bytes);

/* hle.c: --stage=N starts stage N of the game's stage table in place of Mission 1's first stage */
extern int g_start_stage;
/* launcher.c: the start screen. found: a disc already found, or NULL. Returns 1 to play (with
 * disc_out and *fullscreen set), 0 to quit, -1 if it couldn't open a window. */
int launcher_run(const char *found, int *fullscreen, char *disc_out, size_t disc_n);
int launcher_fps60(void); /* the start screen's saved frame-rate choice */
extern int g_force_60fps; /* hle.c: 60 fps option from the launcher (SFA_60FPS overrides) */

#endif

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

/* Disc drive (dvd.c) */
void rt_dvd_init(const char *root);
int rt_dvd_write(uint32_t off, uint32_t v, int size);
void rt_dvd_stats(unsigned long *reads, unsigned long *bytes);

#endif

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

#endif

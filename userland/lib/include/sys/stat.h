#ifndef NAUMI_SHIM_SYS_STAT_H
#define NAUMI_SHIM_SYS_STAT_H

#include <sys/types.h>

/* No writable filesystem yet (see kernel/fs/fat16.c — read-only) — always
   fails. Only used by third_party/doomgeneric's M_MakeDirectory(), whose
   own callers already tolerate it silently not working (config/savegame
   directories that just never get created). */
int mkdir(const char *path, mode_t mode);

#endif

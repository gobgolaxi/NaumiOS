#ifndef SPAWN_H
#define SPAWN_H

#include <stdint.h>
#include <stddef.h>

/* Creates a brand-new, isolated process from an ELF64 image already fully
   in memory: fresh address space (vmm_new_addrspace), ELF segments loaded
   into it, a stack mapped, and a task registered with the scheduler.
   `data`/`size` are only read during the call — the caller keeps ownership
   and can free them right after. Returns the new task's pid, or -1 on
   failure (bad ELF, allocation failure). */
int spawn_elf_bytes(const char *name, const void *data, size_t size);

#endif

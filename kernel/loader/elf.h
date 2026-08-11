#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>
#include "../mm/vmm.h"

/* Parses a statically-linked, non-relocatable ELF64 AArch64 executable out
   of `data` (already fully in memory — `size` bytes), maps each PT_LOAD
   segment into address space `as` at its p_vaddr (must be page-aligned;
   see the userland demo's user.ld), and writes the entry point to *entry.
   Returns 0 on success, -1 on any validation failure. */
int elf_load(vmm_addrspace_t as, const void *data, size_t size, uint64_t *entry);

#endif

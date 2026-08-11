#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

/* Must run after pmm_init(). Doesn't touch vmm at all — the heap lives
   directly in HHDM-mapped pages from pmm, since HHDM already covers every
   page pmm can hand out. */
void heap_init(void);

void *kmalloc(size_t size);
void kfree(void *ptr);

#endif

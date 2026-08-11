#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <limine.h>

#define PAGE_SIZE 4096ULL

/* Builds the free-page bitmap from Limine's memory map. hhdm_offset is
   needed to write into the bitmap itself, which is stashed inside one of
   the usable regions it describes. */
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

/* Returns the physical address of a freshly zeroed 4KiB page, or 0 if
   physical memory is exhausted. */
uint64_t pmm_alloc_page(void);

void pmm_free_page(uint64_t phys_addr);

uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);

/* HHDM offset stashed by pmm_init(), for callers that need to turn a
   physical page they just allocated into a writable pointer themselves. */
uint64_t pmm_hhdm_offset(void);

#endif

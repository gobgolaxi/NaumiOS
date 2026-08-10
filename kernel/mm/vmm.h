#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* Allocates the TTBR0_EL1 root table (via pmm) and points the CPU at it.
   Must run after pmm_init(). TTBR0_EL1 is unspecified at Limine handoff
   (base revision 1+), so it's ours to use for whatever mappings the
   kernel needs beyond what Limine's own TTBR1 mapping already covers. */
void vmm_init(uint64_t hhdm_offset);

/* Normal cacheable RAM mapping, kernel-only RW, never executable. */
void vmm_map_normal(uint64_t virt, uint64_t phys);

/* Device-nGnRnE identity mapping (virt == phys) for MMIO, kernel-only RW,
   never executable. Limine's HHDM does not cover device memory. */
void vmm_map_device(uint64_t phys_identity);

#endif

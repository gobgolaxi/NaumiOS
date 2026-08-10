#ifndef MMU_H
#define MMU_H

#include <stdint.h>

/* Must be called once with the kernel's physical/virtual load addresses
   (from Limine's executable address request) before mmu_map_device_identity,
   so it can translate its own table pointers to physical addresses. */
void mmu_init(uint64_t phys_base, uint64_t virt_base);

/* Identity-maps the 2MiB block containing phys_addr as Device-nGnRnE
   memory via a small TTBR0_EL1 table. Limine's HHDM (base revision 3+)
   only covers RAM-backed memmap regions, not MMIO, so device registers
   need their own mapping before they can be touched. */
void mmu_map_device_identity(uint64_t phys_addr);

#endif

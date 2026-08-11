#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* Physical address of a TTBR0 root table — an opaque handle for a whole
   process's address space. */
typedef uint64_t vmm_addrspace_t;

/* Allocates the kernel's own TTBR0_EL1 root table (via pmm) and points the
   CPU at it. Must run after pmm_init(). TTBR0_EL1 is unspecified at Limine
   handoff (base revision 1+), so it's ours to use for whatever mappings the
   kernel needs beyond what Limine's own TTBR1 mapping already covers. */
void vmm_init(uint64_t hhdm_offset);

/* The address space kernel-mode tasks run under (device mappings + whatever
   vmm_map_normal()/vmm_map_device() add to it). */
vmm_addrspace_t vmm_kernel_addrspace(void);

/* Allocates a brand-new, otherwise-empty address space and immediately
   replicates the kernel's device mappings (UART/GIC) into it — device
   mappings live in TTBR0, not the always-resident TTBR1/HHDM range, so
   without this, kernel exception/syscall handling would fault on its own
   UART/GIC access the moment TTBR0 pointed at a process's table instead of
   the kernel's. */
vmm_addrspace_t vmm_new_addrspace(void);

/* Loads `as` into TTBR0_EL1 and invalidates the TLB. */
void vmm_switch(vmm_addrspace_t as);

/* Below: convenience wrappers that operate on the kernel's own address
   space (vmm_kernel_addrspace()) — what every kernel-mode driver has used
   all along. The _in variants take an explicit address space, for mapping
   pages into a specific process. */

/* Normal cacheable RAM mapping, kernel-only RW, never executable. */
void vmm_map_normal(uint64_t virt, uint64_t phys);

/* Device-nGnRnE identity mapping (virt == phys) for MMIO, kernel-only RW,
   never executable. Limine's HHDM does not cover device memory. */
void vmm_map_device(uint64_t phys_identity);

/* Normal RAM, EL0+EL1 RW, executable at EL0 (PXN set so EL1 itself never
   runs it). For user-mode code pages. */
void vmm_map_user_code(uint64_t virt, uint64_t phys);

/* Normal RAM, EL0+EL1 RW, never executable anywhere. For user-mode stacks
   and data. */
void vmm_map_user_data(uint64_t virt, uint64_t phys);

void vmm_map_normal_in(vmm_addrspace_t as, uint64_t virt, uint64_t phys);
void vmm_map_device_in(vmm_addrspace_t as, uint64_t phys_identity);
void vmm_map_user_code_in(vmm_addrspace_t as, uint64_t virt, uint64_t phys);
void vmm_map_user_data_in(vmm_addrspace_t as, uint64_t virt, uint64_t phys);

#endif

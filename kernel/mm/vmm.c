#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "pmm.h"
#include "../arch/aarch64/mmio_map.h"

#define PT_ENTRIES 512
#define ADDR_MASK 0x0000FFFFFFFFF000ULL /* bits [47:12] */

#define DESC_VALID (1ULL << 0)
#define DESC_TABLE (1ULL << 1) /* also doubles as the L3 "page" bit */

#define ATTR_IDX(n) ((uint64_t)(n) << 2)
#define AP_RW_EL1   (0ULL << 6)
#define AP_RW_EL0   (1ULL << 6) /* AP[2:1] = 01: EL1 RW, EL0 RW */
#define SH_INNER    (3ULL << 8)
#define SH_OUTER    (2ULL << 8)
#define AF          (1ULL << 10)
#define UXN         (1ULL << 54)
#define PXN         (1ULL << 53)

/* Attr0 is guaranteed Normal Write-Back by the Limine protocol; Attr1 is
   reserved for the framebuffer's caching type, so device memory takes the
   next free slot. */
#define MAIR_NORMAL_IDX 0
#define MAIR_DEVICE_IDX 2
#define MAIR_DEVICE_NGNRNE 0x00ULL

static uint64_t hhdm_off;
static vmm_addrspace_t kernel_as;

static inline uint64_t *phys_to_virt(uint64_t phys) {
    return (uint64_t *)(hhdm_off + phys);
}

static inline void write_ttbr0_el1(uint64_t val) {
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"(val));
}

static inline uint64_t read_mair_el1(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, mair_el1" : "=r"(v));
    return v;
}

static inline void write_mair_el1(uint64_t v) {
    __asm__ volatile ("msr mair_el1, %0" :: "r"(v));
}

static inline uint64_t read_tcr_el1(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, tcr_el1" : "=r"(v));
    return v;
}

static inline void write_tcr_el1(uint64_t v) {
    __asm__ volatile ("msr tcr_el1, %0" :: "r"(v));
}

static inline void flush_tlb(void) {
    __asm__ volatile ("dsb ishst");
    __asm__ volatile ("tlbi vmalle1");
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");
}

/* Walks the 4-level, 4KiB-granule table rooted at `as` for `virt`, creating
   intermediate tables via pmm as needed, and returns a writable pointer to
   the L3 (page) entry. */
static uint64_t *walk_create(vmm_addrspace_t as, uint64_t virt) {
    uint64_t *table = phys_to_virt(as);

    for (int level = 0; level < 3; level++) {
        size_t idx = (virt >> (39 - level * 9)) & 0x1FF;
        if (!(table[idx] & DESC_VALID)) {
            uint64_t new_table_phys = pmm_alloc_page();
            table[idx] = new_table_phys | DESC_TABLE | DESC_VALID;
        }
        uint64_t next_phys = table[idx] & ADDR_MASK;
        table = phys_to_virt(next_phys);
    }

    size_t l3_idx = (virt >> 12) & 0x1FF;
    return &table[l3_idx];
}

static void map_page(vmm_addrspace_t as, uint64_t virt, uint64_t phys,
                      uint64_t attr_idx, uint64_t extra_flags) {
    uint64_t *pte = walk_create(as, virt);
    *pte = (phys & ADDR_MASK) | ATTR_IDX(attr_idx) | AF | extra_flags |
           DESC_TABLE | DESC_VALID;
    flush_tlb();
}

void vmm_init(uint64_t hhdm_offset) {
    hhdm_off = hhdm_offset;
    kernel_as = pmm_alloc_page();

    uint64_t mair = read_mair_el1();
    mair &= ~(0xFFULL << (MAIR_DEVICE_IDX * 8));
    mair |= MAIR_DEVICE_NGNRNE << (MAIR_DEVICE_IDX * 8);
    write_mair_el1(mair);

    write_ttbr0_el1(kernel_as);

    uint64_t tcr = read_tcr_el1();
    tcr &= ~0x3FULL;        /* T0SZ = 16 -> 48-bit TTBR0 input address size */
    tcr |= 16ULL;
    tcr &= ~(0x3ULL << 14); /* TG0 = 0b00 -> 4KiB granule */
    tcr &= ~(1ULL << 7);    /* EPD0 = 0 -> enable TTBR0 walks */
    write_tcr_el1(tcr);

    flush_tlb();
}

vmm_addrspace_t vmm_kernel_addrspace(void) {
    return kernel_as;
}

vmm_addrspace_t vmm_new_addrspace(void) {
    vmm_addrspace_t as = pmm_alloc_page();
    vmm_map_device_in(as, MMIO_UART0_BASE);
    vmm_map_device_in(as, MMIO_GICD_BASE);
    vmm_map_device_in(as, MMIO_GICC_BASE);
    for (int p = 0; p < MMIO_VIRTIO_PAGE_COUNT; p++) {
        vmm_map_device_in(as, MMIO_VIRTIO_BASE + (uint64_t)p * PAGE_SIZE);
    }
    return as;
}

void vmm_switch(vmm_addrspace_t as) {
    write_ttbr0_el1(as);
    flush_tlb();
}

void vmm_map_normal_in(vmm_addrspace_t as, uint64_t virt, uint64_t phys) {
    map_page(as, virt, phys, MAIR_NORMAL_IDX, AP_RW_EL1 | SH_INNER | UXN | PXN);
}

void vmm_map_device_in(vmm_addrspace_t as, uint64_t phys_identity) {
    map_page(as, phys_identity, phys_identity, MAIR_DEVICE_IDX, AP_RW_EL1 | SH_OUTER | UXN | PXN);
}

void vmm_map_user_code_in(vmm_addrspace_t as, uint64_t virt, uint64_t phys) {
    /* UXN clear: EL0 may execute this page. PXN set: EL1 never will. */
    map_page(as, virt, phys, MAIR_NORMAL_IDX, AP_RW_EL0 | SH_INNER | PXN);
}

void vmm_map_user_data_in(vmm_addrspace_t as, uint64_t virt, uint64_t phys) {
    map_page(as, virt, phys, MAIR_NORMAL_IDX, AP_RW_EL0 | SH_INNER | UXN | PXN);
}

void vmm_map_normal(uint64_t virt, uint64_t phys) {
    vmm_map_normal_in(kernel_as, virt, phys);
}

void vmm_map_device(uint64_t phys_identity) {
    vmm_map_device_in(kernel_as, phys_identity);
}

void vmm_map_user_code(uint64_t virt, uint64_t phys) {
    vmm_map_user_code_in(kernel_as, virt, phys);
}

void vmm_map_user_data(uint64_t virt, uint64_t phys) {
    vmm_map_user_data_in(kernel_as, virt, phys);
}

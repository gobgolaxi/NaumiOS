#include <stdint.h>
#include <stddef.h>
#include "mmu.h"

#define PAGE_SIZE 4096ULL
#define BLOCK_2M_MASK (~(0x1FFFFFULL))

__attribute__((aligned(PAGE_SIZE))) static uint64_t l0_table[512];
__attribute__((aligned(PAGE_SIZE))) static uint64_t l1_table[512];
__attribute__((aligned(PAGE_SIZE))) static uint64_t l2_table[512];

#define DESC_VALID (1ULL << 0)
#define DESC_TABLE (1ULL << 1)
#define DESC_BLOCK (0ULL << 1)

#define ATTR_IDX(n) ((uint64_t)(n) << 2)
#define AP_RW_EL1   (0ULL << 6)
#define SH_OUTER    (2ULL << 8)
#define AF          (1ULL << 10)
#define UXN         (1ULL << 54)
#define PXN         (1ULL << 53)

/* Attr0 is guaranteed Normal Write-Back by the Limine protocol (base
   revision 4+) and Attr1 is reserved for the framebuffer's caching type,
   so device memory goes in the first attribute slot the spec leaves free. */
#define MAIR_DEVICE_IDX     2
#define MAIR_DEVICE_NGNRNE  0x00ULL

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

/* physical_base - virtual_base, per Limine's executable address request;
   the loaded image uses a single uniform offset for all its symbols. */
static int64_t kernel_slide;

void mmu_init(uint64_t phys_base, uint64_t virt_base) {
    kernel_slide = (int64_t)(phys_base - virt_base);
}

static inline uint64_t v2p(void *ptr) {
    return (uint64_t)ptr + (uint64_t)kernel_slide;
}

void mmu_map_device_identity(uint64_t phys_addr) {
    uint64_t block_base = phys_addr & BLOCK_2M_MASK;

    size_t l0_idx = (block_base >> 39) & 0x1FF;
    size_t l1_idx = (block_base >> 30) & 0x1FF;
    size_t l2_idx = (block_base >> 21) & 0x1FF;

    l0_table[l0_idx] = v2p(l1_table) | DESC_TABLE | DESC_VALID;
    l1_table[l1_idx] = v2p(l2_table) | DESC_TABLE | DESC_VALID;
    l2_table[l2_idx] = block_base | ATTR_IDX(MAIR_DEVICE_IDX) | AP_RW_EL1 |
                        SH_OUTER | AF | UXN | PXN | DESC_BLOCK | DESC_VALID;

    uint64_t mair = read_mair_el1();
    mair &= ~(0xFFULL << (MAIR_DEVICE_IDX * 8));
    mair |= MAIR_DEVICE_NGNRNE << (MAIR_DEVICE_IDX * 8);
    write_mair_el1(mair);

    write_ttbr0_el1(v2p(l0_table));

    uint64_t tcr = read_tcr_el1();
    tcr &= ~0x3FULL;          /* T0SZ = 16 -> 48-bit TTBR0 input address size */
    tcr |= 16ULL;
    tcr &= ~(0x3ULL << 14);   /* TG0 = 0b00 -> 4KiB granule */
    tcr &= ~(1ULL << 7);      /* EPD0 = 0 -> enable TTBR0 walks */
    write_tcr_el1(tcr);

    __asm__ volatile ("dsb ishst");
    __asm__ volatile ("tlbi vmalle1");
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");
}

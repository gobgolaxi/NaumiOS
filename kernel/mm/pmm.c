#include <stdint.h>
#include <stddef.h>
#include "pmm.h"

static uint8_t *bitmap;
static uint64_t bitmap_page_count;
static uint64_t hhdm_off;
static uint64_t free_count;
static uint64_t search_cursor;

static inline int bit_test(uint64_t page_idx) {
    return bitmap[page_idx / 8] & (1U << (page_idx % 8));
}

static inline void bit_set(uint64_t page_idx) {
    bitmap[page_idx / 8] |= (uint8_t)(1U << (page_idx % 8));
}

static inline void bit_clear(uint64_t page_idx) {
    bitmap[page_idx / 8] &= (uint8_t)~(1U << (page_idx % 8));
}

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(hhdm_off + phys);
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    hhdm_off = hhdm_offset;

    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE ||
            e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            uint64_t end = e->base + e->length;
            if (end > highest_addr) {
                highest_addr = end;
            }
        }
    }

    bitmap_page_count = highest_addr / PAGE_SIZE;
    uint64_t bitmap_bytes = (bitmap_page_count + 7) / 8;

    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes) {
            bitmap_phys = e->base;
            break;
        }
    }

    bitmap = (uint8_t *)phys_to_virt(bitmap_phys);
    for (uint64_t i = 0; i < bitmap_bytes; i++) {
        bitmap[i] = 0xFF;
    }

    free_count = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        uint64_t start_page = e->base / PAGE_SIZE;
        uint64_t page_count = e->length / PAGE_SIZE;
        for (uint64_t p = 0; p < page_count; p++) {
            bit_clear(start_page + p);
            free_count++;
        }
    }

    /* The bitmap itself sits inside a usable region and was just marked
       free above — reclaim those pages before anything can hand them out. */
    uint64_t bitmap_start_page = bitmap_phys / PAGE_SIZE;
    uint64_t bitmap_page_span = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = 0; p < bitmap_page_span; p++) {
        if (!bit_test(bitmap_start_page + p)) {
            bit_set(bitmap_start_page + p);
            free_count--;
        }
    }

    search_cursor = 0;
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t i = 0; i < bitmap_page_count; i++) {
        uint64_t idx = (search_cursor + i) % bitmap_page_count;
        if (!bit_test(idx)) {
            bit_set(idx);
            free_count--;
            search_cursor = idx + 1;

            uint64_t phys = idx * PAGE_SIZE;
            uint64_t *page = (uint64_t *)phys_to_virt(phys);
            for (size_t w = 0; w < PAGE_SIZE / sizeof(uint64_t); w++) {
                page[w] = 0;
            }
            return phys;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys_addr) {
    uint64_t idx = phys_addr / PAGE_SIZE;
    if (bit_test(idx)) {
        bit_clear(idx);
        free_count++;
    }
}

uint64_t pmm_free_pages(void) {
    return free_count;
}

uint64_t pmm_total_pages(void) {
    return bitmap_page_count;
}

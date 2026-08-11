#include <stdint.h>
#include "virtio_mmio.h"
#include "uart.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"

#define VIRTIO_MMIO_BASE 0x0a000000UL
#define VIRTIO_MMIO_SLOT_STRIDE 0x200UL
#define VIRTIO_MMIO_SLOT_COUNT 32
#define VIRTIO_MMIO_PAGE_COUNT 4 /* 32 slots * 0x200 = 0x4000 = 4 pages */

#define REG_MAGIC    0x00
#define REG_VERSION  0x04
#define REG_DEVICEID 0x08
#define REG_VENDORID 0x0C

static inline uint32_t slot_read(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void map_slots(void) {
    for (int p = 0; p < VIRTIO_MMIO_PAGE_COUNT; p++) {
        vmm_map_device(VIRTIO_MMIO_BASE + (uint64_t)p * PAGE_SIZE);
    }
}

uint64_t virtio_mmio_find_device(uint32_t device_id) {
    map_slots();

    for (int i = 0; i < VIRTIO_MMIO_SLOT_COUNT; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_SLOT_STRIDE;
        if (slot_read(base, REG_MAGIC) != 0x74726976UL) {
            continue;
        }
        if (slot_read(base, REG_DEVICEID) == device_id) {
            return base;
        }
    }
    return 0;
}

void virtio_mmio_for_each_device(uint32_t device_id, void (*visit)(uint64_t base)) {
    map_slots();

    for (int i = 0; i < VIRTIO_MMIO_SLOT_COUNT; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_SLOT_STRIDE;
        if (slot_read(base, REG_MAGIC) != 0x74726976UL) {
            continue;
        }
        if (slot_read(base, REG_DEVICEID) == device_id) {
            visit(base);
        }
    }
}

void virtio_mmio_scan(void) {
    map_slots();

    for (int i = 0; i < VIRTIO_MMIO_SLOT_COUNT; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_SLOT_STRIDE;
        uint32_t magic = slot_read(base, REG_MAGIC);
        if (magic != 0x74726976UL) {
            continue;
        }

        uint32_t version = slot_read(base, REG_VERSION);
        uint32_t device_id = slot_read(base, REG_DEVICEID);
        uint32_t vendor_id = slot_read(base, REG_VENDORID);

        uart_puts("virtio-mmio slot "); uart_puthex((uint64_t)i);
        uart_puts(" base "); uart_puthex(base);
        uart_puts(" version "); uart_puthex(version);
        uart_puts(" device_id "); uart_puthex(device_id);
        uart_puts(" vendor_id "); uart_puthex(vendor_id);
        uart_puts("\n");
    }
}

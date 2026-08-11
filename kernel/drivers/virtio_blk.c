#include <stdint.h>
#include <stddef.h>
#include "virtio_blk.h"
#include "virtio_mmio.h"
#include "uart.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"

/* Legacy (version 1) virtio-mmio register offsets — confirmed via a live
   scan (kernel/drivers/virtio_mmio.c) that QEMU hands us version 1 on this
   machine, which uses this register set (not the "modern"/v2 one with
   QueueDescLow/QueueDriverLow/QueueDeviceLow — those don't exist here). */
#define REG_DEVICEID          0x08
#define REG_HOST_FEATURES     0x10
#define REG_HOST_FEATURES_SEL 0x14
#define REG_GUEST_FEATURES    0x20
#define REG_GUEST_FEATURES_SEL 0x24
#define REG_GUEST_PAGE_SIZE   0x28
#define REG_QUEUE_SEL         0x30
#define REG_QUEUE_NUM_MAX     0x34
#define REG_QUEUE_NUM         0x38
#define REG_QUEUE_ALIGN       0x3C
#define REG_QUEUE_PFN         0x40
#define REG_QUEUE_NOTIFY      0x50
#define REG_INTERRUPT_STATUS  0x60
#define REG_INTERRUPT_ACK     0x64
#define REG_STATUS            0x70
#define REG_CONFIG            0x100

#define STATUS_ACKNOWLEDGE 1U
#define STATUS_DRIVER      2U
#define STATUS_DRIVER_OK   4U

#define VIRTQ_DESC_F_NEXT  1U
#define VIRTQ_DESC_F_WRITE 2U

#define QSIZE 8U /* small, fixed, well under any QueueNumMax QEMU offers */

#define VIRTIO_BLK_T_IN 0U /* read */

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSIZE];
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[QSIZE];
} virtq_used_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_req_hdr_t;

static uint64_t mmio_base;
static uint64_t hhdm;

/* volatile: this memory is written/read by the device asynchronously (from
   the CPU's point of view), so the compiler must never cache a stale value
   or reorder/elide accesses to it — without this, -O2 hoisted the
   used->idx read out of the polling loop in virtio_blk_read() entirely,
   turning it into an infinite loop the very first time it ran. */
static volatile virtq_desc_t *desc;
static volatile virtq_avail_t *avail;
static volatile virtq_used_t *used;
static uint16_t last_used_idx;

/* Fixed, reused for every request — this driver only ever has one request
   in flight at a time. Header/status must be HHDM-backed like everything
   else the device DMAs into/out of. */
static virtio_blk_req_hdr_t *req_hdr;
static uint8_t *req_status;

static inline uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(mmio_base + off);
}

static inline void mmio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(mmio_base + off) = val;
}

static inline uint64_t phys_of(const void *ptr) {
    return (uint64_t)ptr - hhdm;
}

int virtio_blk_init(void) {
    hhdm = pmm_hhdm_offset();

    mmio_base = virtio_mmio_find_device(2 /* block device */);
    if (mmio_base == 0) {
        uart_puts("virtio_blk: no block device found\n");
        return -1;
    }

    mmio_write32(REG_STATUS, 0); /* reset */
    mmio_write32(REG_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write32(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    /* No optional features (e.g. VIRTIO_BLK_F_SIZE_MAX) needed for plain
       sector reads — accept nothing. */
    mmio_write32(REG_GUEST_FEATURES_SEL, 0);
    mmio_write32(REG_GUEST_FEATURES, 0);

    mmio_write32(REG_GUEST_PAGE_SIZE, (uint32_t)PAGE_SIZE);

    mmio_write32(REG_QUEUE_SEL, 0);
    uint32_t max = mmio_read32(REG_QUEUE_NUM_MAX);
    if (max == 0) {
        uart_puts("virtio_blk: queue 0 unavailable\n");
        return -1;
    }
    if (QSIZE > max) {
        uart_puts("virtio_blk: QSIZE exceeds QueueNumMax\n");
        return -1;
    }
    mmio_write32(REG_QUEUE_NUM, QSIZE);
    mmio_write32(REG_QUEUE_ALIGN, (uint32_t)PAGE_SIZE);

    /* Legacy layout: desc table + avail ring share page 0 (comfortably —
       QSIZE=8 means 8*16 + (4+8*2) = 148 bytes), used ring must start at
       the next QueueAlign-aligned boundary, i.e. page 1. Two pages need to
       be physically contiguous for this to be one valid queue region;
       pmm's bump allocator gives back-to-back calls sequential pages as
       long as nothing else raced in between, which nothing does this
       early at boot. */
    uint64_t page0 = pmm_alloc_page();
    uint64_t page1 = pmm_alloc_page();
    if (page0 == 0 || page1 == 0 || page1 != page0 + PAGE_SIZE) {
        uart_puts("virtio_blk: could not get 2 contiguous pages for virtqueue\n");
        return -1;
    }

    desc = (virtq_desc_t *)(hhdm + page0);
    avail = (virtq_avail_t *)((uint8_t *)desc + sizeof(virtq_desc_t) * QSIZE);
    used = (virtq_used_t *)(hhdm + page1);

    mmio_write32(REG_QUEUE_PFN, (uint32_t)(page0 / PAGE_SIZE));

    /* Reused for every request (only one ever in flight); kmalloc() is
       HHDM-backed like everything else the device DMAs into/out of. */
    req_hdr = (virtio_blk_req_hdr_t *)kmalloc(sizeof(virtio_blk_req_hdr_t));
    req_status = (uint8_t *)kmalloc(1);
    if (!req_hdr || !req_status) {
        uart_puts("virtio_blk: kmalloc failed for request buffers\n");
        return -1;
    }

    mmio_write32(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK);

    last_used_idx = used->idx;

    uart_puts("virtio_blk: ready, base "); uart_puthex(mmio_base); uart_puts("\n");
    return 0;
}

int virtio_blk_read(uint64_t start_sector, void *buf, uint32_t sector_count) {
    if (mmio_base == 0) {
        return -1;
    }

    req_hdr->type = VIRTIO_BLK_T_IN;
    req_hdr->reserved = 0;
    req_hdr->sector = start_sector;
    *req_status = 0xFF; /* sentinel; device overwrites with VIRTIO_BLK_S_OK (0) on success */

    desc[0].addr = phys_of(req_hdr);
    desc[0].len = sizeof(*req_hdr);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = phys_of(buf);
    desc[1].len = sector_count * VIRTIO_BLK_SECTOR_SIZE;
    desc[1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    desc[1].next = 2;

    desc[2].addr = phys_of(req_status);
    desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next = 0;

    /* Descriptor/avail writes must land before the device sees avail->idx
       advance, and before it sees the notify — plain compiler-reordering
       protection matters even under QEMU's emulation, hardware DMA
       ordering would need this for real. */
    __asm__ volatile ("dsb sy" ::: "memory");

    uint16_t avail_idx = avail->idx;
    avail->ring[avail_idx % QSIZE] = 0; /* head descriptor index */
    __asm__ volatile ("dsb sy" ::: "memory");
    avail->idx = avail_idx + 1;
    __asm__ volatile ("dsb sy" ::: "memory");

    mmio_write32(REG_QUEUE_NOTIFY, 0);

    /* No interrupt wired up yet — plain poll. Bounded so a device that
       never completes can't hang the caller forever. */
    uint32_t spins = 0;
    while (used->idx == last_used_idx) {
        spins++;
        if (spins > 100000000U) {
            uart_puts("virtio_blk: read timed out\n");
            return -1;
        }
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    last_used_idx = used->idx;

    if (*req_status != 0) {
        uart_puts("virtio_blk: device reported error, status=");
        uart_puthex(*req_status);
        uart_puts("\n");
        return -1;
    }

    return 0;
}

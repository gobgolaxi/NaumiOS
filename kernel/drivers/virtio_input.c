#include <stdint.h>
#include <stddef.h>
#include "virtio_input.h"
#include "virtio_mmio.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"

/* Legacy (version 1) virtio-mmio register offsets — same layout confirmed
   for virtio-blk (see virtio_blk.c); QEMU hands out legacy uniformly. */
#define REG_GUEST_FEATURES_SEL 0x24
#define REG_GUEST_FEATURES     0x20
#define REG_GUEST_PAGE_SIZE    0x28
#define REG_QUEUE_SEL          0x30
#define REG_QUEUE_NUM_MAX      0x34
#define REG_QUEUE_NUM          0x38
#define REG_QUEUE_ALIGN        0x3C
#define REG_QUEUE_PFN          0x40
#define REG_QUEUE_NOTIFY       0x50
#define REG_STATUS             0x70
#define REG_CONFIG             0x100

#define STATUS_ACKNOWLEDGE 1U
#define STATUS_DRIVER      2U
#define STATUS_DRIVER_OK   4U

#define VIRTQ_DESC_F_WRITE 2U

#define QSIZE 16U /* pre-posted rx buffers, one virtio_input_event_t each */

#define VIRTIO_ID_INPUT 18U
#define VIRTIO_INPUT_CFG_ID_NAME 0x01U

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

struct virtio_input_dev {
    uint64_t mmio_base;
    volatile virtq_desc_t *desc;
    volatile virtq_avail_t *avail;
    volatile virtq_used_t *used;
    uint16_t last_used_idx;
    virtio_input_event_t *events; /* QSIZE of them, kmalloc'd (HHDM-backed) */
};

static uint64_t hhdm;

static inline uint32_t mmio_read32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write32(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static inline uint8_t mmio_read8(uint64_t base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

static inline void mmio_write8(uint64_t base, uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(base + off) = val;
}

/* select+subsel take effect synchronously — the device updates size/data
   in the config struct as soon as they're written, no separate commit. */
static void read_config_name(uint64_t base, char *out, size_t out_size) {
    mmio_write8(base, REG_CONFIG + 0, VIRTIO_INPUT_CFG_ID_NAME);
    mmio_write8(base, REG_CONFIG + 1, 0);
    uint8_t size = mmio_read8(base, REG_CONFIG + 2);

    size_t n = size < out_size - 1 ? size : out_size - 1;
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)mmio_read8(base, REG_CONFIG + 8 + i);
    }
    out[n] = '\0';
}

static int str_contains(const char *hay, const char *needle) {
    if (*needle == '\0') {
        return 1;
    }
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0') {
            return 1;
        }
    }
    return 0;
}

/* virtio_mmio_for_each_device()'s callback takes no context pointer, so
   this is filled in by a single static during virtio_input_open() — fine
   for driver init, which only ever runs single-threaded at boot. */
static const char *g_want_name;
static uint64_t g_found_base;

static void find_visit(uint64_t base) {
    if (g_found_base != 0) {
        return;
    }
    char name[64];
    read_config_name(base, name, sizeof(name));
    if (str_contains(name, g_want_name)) {
        g_found_base = base;
    }
}

virtio_input_dev_t *virtio_input_open(const char *name_substr) {
    hhdm = pmm_hhdm_offset();

    g_want_name = name_substr;
    g_found_base = 0;
    virtio_mmio_for_each_device(VIRTIO_ID_INPUT, find_visit);
    if (g_found_base == 0) {
        return NULL;
    }
    uint64_t base = g_found_base;

    mmio_write32(base, REG_STATUS, 0); /* reset */
    mmio_write32(base, REG_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write32(base, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    mmio_write32(base, REG_GUEST_FEATURES_SEL, 0);
    mmio_write32(base, REG_GUEST_FEATURES, 0);
    mmio_write32(base, REG_GUEST_PAGE_SIZE, (uint32_t)PAGE_SIZE);

    mmio_write32(base, REG_QUEUE_SEL, 0);
    uint32_t max = mmio_read32(base, REG_QUEUE_NUM_MAX);
    if (max == 0 || QSIZE > max) {
        return NULL;
    }
    mmio_write32(base, REG_QUEUE_NUM, QSIZE);
    mmio_write32(base, REG_QUEUE_ALIGN, (uint32_t)PAGE_SIZE);

    /* Same 2-contiguous-pages layout as virtio_blk.c: desc+avail in page 0,
       used ring page-aligned in page 1. */
    uint64_t page0 = pmm_alloc_page();
    uint64_t page1 = pmm_alloc_page();
    if (page0 == 0 || page1 == 0 || page1 != page0 + PAGE_SIZE) {
        return NULL;
    }

    virtio_input_dev_t *dev = (virtio_input_dev_t *)kmalloc(sizeof(virtio_input_dev_t));
    if (!dev) {
        return NULL;
    }
    dev->events = (virtio_input_event_t *)kmalloc(sizeof(virtio_input_event_t) * QSIZE);
    if (!dev->events) {
        kfree(dev);
        return NULL;
    }

    dev->mmio_base = base;
    dev->desc = (volatile virtq_desc_t *)(hhdm + page0);
    dev->avail = (volatile virtq_avail_t *)((uint8_t *)dev->desc + sizeof(virtq_desc_t) * QSIZE);
    dev->used = (volatile virtq_used_t *)(hhdm + page1);
    dev->last_used_idx = 0;

    mmio_write32(base, REG_QUEUE_PFN, (uint32_t)(page0 / PAGE_SIZE));

    /* Input devices are a pure "rx" queue: every descriptor starts
       pre-posted as a device-writable buffer, refilled and reposted as
       fast as events are consumed (see virtio_input_poll()). */
    for (uint32_t i = 0; i < QSIZE; i++) {
        dev->desc[i].addr = (uint64_t)&dev->events[i] - hhdm;
        dev->desc[i].len = sizeof(virtio_input_event_t);
        dev->desc[i].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[i].next = 0;
        dev->avail->ring[i] = (uint16_t)i;
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    dev->avail->idx = QSIZE;
    __asm__ volatile ("dsb sy" ::: "memory");

    mmio_write32(base, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK);
    mmio_write32(base, REG_QUEUE_NOTIFY, 0);

    return dev;
}

int virtio_input_poll(virtio_input_dev_t *dev, virtio_input_event_t *out_ev) {
    if (dev->used->idx == dev->last_used_idx) {
        return 0;
    }
    __asm__ volatile ("dsb sy" ::: "memory");

    uint16_t ring_idx = dev->last_used_idx % QSIZE;
    uint32_t desc_id = dev->used->ring[ring_idx].id;
    *out_ev = dev->events[desc_id];
    dev->last_used_idx++;

    uint16_t avail_idx = dev->avail->idx;
    dev->avail->ring[avail_idx % QSIZE] = (uint16_t)desc_id;
    __asm__ volatile ("dsb sy" ::: "memory");
    dev->avail->idx = avail_idx + 1;
    __asm__ volatile ("dsb sy" ::: "memory");
    mmio_write32(dev->mmio_base, REG_QUEUE_NOTIFY, 0);

    return 1;
}

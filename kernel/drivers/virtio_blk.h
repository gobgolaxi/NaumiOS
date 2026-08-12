#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>

#define VIRTIO_BLK_SECTOR_SIZE 512

/* Finds the virtio-blk device (via virtio_mmio_find_device) and brings it
   up: legacy (version 1) virtio-mmio init sequence, one small polling
   virtqueue. Returns 0 on success, -1 if no block device is present or
   initialization failed. */
int virtio_blk_init(void);

/* Reads `sector_count` consecutive 512-byte sectors starting at
   `start_sector` into `buf`. `buf` MUST be a kernel pointer backed by HHDM
   (kmalloc(), or a pointer straight from pmm) — this driver hands the
   device a physical address computed as `buf - hhdm_offset`, which is only
   correct for HHDM-mapped memory. A buffer on kmain's own boot-time stack
   (TTBR1 higher-half, not HHDM) would silently compute the wrong physical
   address. `buf` must be at least `sector_count * 512` bytes.
   Returns 0 on success, -1 on failure/timeout. */
int virtio_blk_read(uint64_t start_sector, void *buf, uint32_t sector_count);

/* Writes `sector_count` consecutive 512-byte sectors starting at
   `start_sector` from `buf` (same HHDM-backing requirement as
   virtio_blk_read()). Returns 0 on success, -1 on failure/timeout. */
int virtio_blk_write(uint64_t start_sector, const void *buf, uint32_t sector_count);

#endif

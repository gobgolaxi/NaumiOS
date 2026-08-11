#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

#include <stdint.h>

/* Maps all 32 virtio-mmio slots the QEMU `virt` machine always provides
   (0x0a000000..0x0a003e00, confirmed via device-tree dump) and prints
   MagicValue/Version/DeviceID/VendorID for every slot that responds.
   Debug/discovery tool, not needed for normal boot. */
void virtio_mmio_scan(void);

/* Maps the slots (idempotent) and returns the MMIO base of the first slot
   whose DeviceID matches, or 0 if none does. DeviceID 2 = block device. */
uint64_t virtio_mmio_find_device(uint32_t device_id);

/* Maps the slots (idempotent) and calls `visit(base)` once for every slot
   whose DeviceID matches — for devices like virtio-input (DeviceID 18)
   where more than one instance (keyboard, mouse, ...) can be present and
   the caller needs to distinguish them (e.g. by reading each one's config
   space name). */
void virtio_mmio_for_each_device(uint32_t device_id, void (*visit)(uint64_t base));

#endif

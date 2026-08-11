#ifndef MMIO_MAP_H
#define MMIO_MAP_H

/* Fixed physical MMIO addresses on the QEMU `virt` machine (confirmed via
   device-tree dump — see the GIC/timer bring-up). Shared between the
   drivers that own each device (uart.c, gic.c) and vmm.c, which needs to
   replicate these exact mappings into every process's own address space:
   device mappings live in TTBR0 rather than the always-resident
   TTBR1/HHDM range, so they don't come along for free the way kernel code
   and the HHDM do. */
#define MMIO_UART0_BASE 0x09000000UL
#define MMIO_GICD_BASE  0x08000000UL
#define MMIO_GICC_BASE  0x08010000UL

/* 32 virtio-mmio transport slots, 0x200 bytes each, spanning 4 pages —
   always present on `virt` regardless of which slot (if any) actually has
   a device plugged in (see virtio_mmio.c). Syscalls like SYS_OPEN run
   kernel code (fat16 -> virtio_blk) under the *caller's* still-live TTBR0
   (see syscall.c), so every process needs this mapped too, not just the
   kernel's own address space — a process without it faults the instant a
   syscall it makes touches the disk. */
#define MMIO_VIRTIO_BASE 0x0a000000UL
#define MMIO_VIRTIO_PAGE_COUNT 4

#endif

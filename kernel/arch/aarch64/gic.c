#include <stdint.h>
#include "gic.h"
#include "mmio_map.h"
#include "../../mm/vmm.h"

/* QEMU `virt` machine, confirmed via device-tree dump: GICv2-compatible
   ("arm,cortex-a15-gic"). Each region is 0x10000 but every register used
   here lives in the first page. */
#define GICD_BASE MMIO_GICD_BASE
#define GICC_BASE MMIO_GICC_BASE

#define GICD_CTLR         (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_IPRIORITYR(n) (*(volatile uint8_t *)(GICD_BASE + 0x400 + (n)))

#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR  (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR  (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))

#define GICC_IAR_INTID_MASK 0x3FFU

void gic_init(void) {
    vmm_map_device(GICD_BASE);
    vmm_map_device(GICC_BASE);

    GICD_CTLR = 1; /* enable distributor forwarding to CPU interfaces */

    GICC_PMR = 0xFF;  /* mask no priority level */
    GICC_CTLR = 1;    /* enable this CPU's interface */
}

void gic_enable_irq(uint32_t intid) {
    GICD_IPRIORITYR(intid) = 0x80;
    GICD_ISENABLER(intid / 32) = 1U << (intid % 32);
}

uint32_t gic_ack_irq(void) {
    return GICC_IAR & GICC_IAR_INTID_MASK;
}

void gic_eoi_irq(uint32_t intid) {
    GICC_EOIR = intid;
}

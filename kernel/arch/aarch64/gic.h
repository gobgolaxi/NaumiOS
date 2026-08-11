#ifndef GIC_H
#define GIC_H

#include <stdint.h>

/* Maps the GICv2 distributor + CPU interface (QEMU `virt` machine: GICD at
   0x08000000, GICC at 0x08010000) and enables both. Must run after vmm_init(). */
void gic_init(void);

/* Enables forwarding for a shared/private INTID and gives it a default
   priority. For PPIs (16-31) this only affects the calling CPU's banked
   registers. */
void gic_enable_irq(uint32_t intid);

/* Reads GICC_IAR, both acknowledging the highest-priority pending IRQ and
   returning its INTID (spurious reads back as 1023). */
uint32_t gic_ack_irq(void);

/* Writes GICC_EOIR to signal completion of the given INTID. */
void gic_eoi_irq(uint32_t intid);

#endif

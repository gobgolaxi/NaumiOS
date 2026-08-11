#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Arms the non-secure EL1 physical generic timer (PPI 14, GIC INTID 30) to
   fire `hz` times per second and enables its GIC interrupt. Must run after
   gic_init(). IRQs still need to be unmasked separately (PSTATE.I). */
void timer_init(uint32_t hz);

/* Called from the IRQ dispatch path when INTID 30 fires: reloads the
   down-counter and bumps the tick count. */
void timer_handle_irq(void);

uint64_t timer_ticks(void);

#endif

#include <stdint.h>
#include "timer.h"
#include "gic.h"

/* Non-secure EL1 physical timer: PPI 14 on the QEMU `virt` machine's GIC
   (confirmed via device-tree dump), i.e. GIC INTID 16 + 14 = 30. */
#define TIMER_IRQ 30U

static volatile uint64_t ticks;
static uint32_t reload_ticks;

static inline uint64_t read_cntfrq_el0(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval_el0(uint32_t v) {
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"((uint64_t)v));
}

static inline void write_cntp_ctl_el0(uint64_t v) {
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(v));
}

void timer_init(uint32_t hz) {
    uint64_t freq = read_cntfrq_el0();
    reload_ticks = (uint32_t)(freq / hz);

    gic_enable_irq(TIMER_IRQ);

    write_cntp_tval_el0(reload_ticks);
    write_cntp_ctl_el0(1); /* ENABLE=1, IMASK=0 */
}

void timer_handle_irq(void) {
    write_cntp_tval_el0(reload_ticks);
    ticks++;
}

uint64_t timer_ticks(void) {
    return ticks;
}

#include <stdint.h>
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "../../drivers/uart.h"
#include "../../sched/sched.h"
#include "../../sched/syscall.h"

#define ESR_EC_SVC64 0x15UL

extern char vector_table[];

static const char *const vector_names[16] = {
    "Synchronous (EL1, SP_EL0)", "IRQ (EL1, SP_EL0)",
    "FIQ (EL1, SP_EL0)",         "SError (EL1, SP_EL0)",
    "Synchronous (EL1, SP_EL1)", "IRQ (EL1, SP_EL1)",
    "FIQ (EL1, SP_EL1)",         "SError (EL1, SP_EL1)",
    "Synchronous (EL0, AArch64)", "IRQ (EL0, AArch64)",
    "FIQ (EL0, AArch64)",         "SError (EL0, AArch64)",
    "Synchronous (EL0, AArch32)", "IRQ (EL0, AArch32)",
    "FIQ (EL0, AArch32)",         "SError (EL0, AArch32)",
};

void exceptions_init(void) {
    __asm__ volatile ("msr vbar_el1, %0" :: "r"(vector_table));
    __asm__ volatile ("isb");
}

void irq_enable(void) {
    __asm__ volatile ("msr daifclr, #2");
}

static uint64_t irq_handler(uint64_t *regs) {
    uint32_t intid = gic_ack_irq();
    uint64_t next_sp = (uint64_t)regs;

    if (intid == 30) {
        timer_handle_irq();
        next_sp = sched_tick(regs);
    }

    if (intid < 1020) {
        gic_eoi_irq(intid);
    }

    return next_sp;
}

/* regs[n] holds xN for n=0..29, x30 at index 30, per save_context's layout
   (16 stp/str pairs = 32 slots of 8 bytes). SVC's ABI here: syscall number
   in x8, up to 3 args in x0-x2, result written back into x0. */
static uint64_t svc_handler(uint64_t *regs) {
    uint64_t num = regs[8];

    if (num == SYS_EXIT) {
        return sched_exit_current();
    }

    regs[0] = syscall_dispatch(num, regs[0], regs[1], regs[2]);
    return (uint64_t)regs;
}

uint64_t exc_handler(uint64_t vector, uint64_t *regs) {
    /* Vector 5 = IRQ taken from EL1 (SP_ELx, how the kernel runs — see
       boot.S). Vector 9 = IRQ taken from EL0 (AArch64), which is always
       SP_EL0 since EL0 has no SPSel choice — the user demo task hits this
       one whenever the timer fires while it's running. Same handler either
       way: GIC ack/EOI and scheduling don't care which EL got interrupted.
       May return a different task's frame pointer than the one it was
       handed — that's how sched_tick() drives a context switch. */
    if (vector == 5 || vector == 9) {
        return irq_handler(regs);
    }

    uint64_t esr, elr, far;
    __asm__ volatile ("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile ("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile ("mrs %0, far_el1" : "=r"(far));

    /* Vector 8 = Synchronous, lower EL (EL0), AArch64 — where `svc` from
       our EL0 demo task lands. ELR_EL1 already points past the svc
       instruction, so a plain eret resumes the caller correctly. */
    if (vector == 8 && (esr >> 26) == ESR_EC_SVC64) {
        return svc_handler(regs);
    }

    /* Vector 4 = Synchronous, EL1, SP_ELx — where `svc` from the kernel's
       own sched_block()/sched_sleep_ticks() lands (they run at EL1, not
       EL0). Kernel-internal only: never issued by anything untrusted. */
    if (vector == 4 && (esr >> 26) == ESR_EC_SVC64) {
        return sched_handle_el1_svc(regs);
    }

    uart_puts("\n--- Unhandled exception: ");
    uart_puts(vector_names[vector]);
    uart_puts(" ---\n");
    uart_puts("ESR_EL1: "); uart_puthex(esr); uart_puts("\n");
    uart_puts("ELR_EL1: "); uart_puthex(elr); uart_puts("\n");
    uart_puts("FAR_EL1: "); uart_puthex(far); uart_puts("\n");

    for (;;) {
        __asm__ volatile ("wfi");
    }
}

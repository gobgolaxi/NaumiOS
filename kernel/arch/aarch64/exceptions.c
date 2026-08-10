#include <stdint.h>
#include "exceptions.h"
#include "../../drivers/uart.h"

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

void exc_handler(uint64_t vector, uint64_t *regs) {
    (void)regs;

    uint64_t esr, elr, far;
    __asm__ volatile ("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile ("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile ("mrs %0, far_el1" : "=r"(far));

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

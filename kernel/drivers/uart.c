#include <stdint.h>
#include "uart.h"
#include "../mm/vmm.h"
#include "../arch/aarch64/mmio_map.h"

/* Not covered by Limine's HHDM (base revision 3+ only maps RAM-backed
   memmap regions), so it needs its own identity mapping first. */
#define UARTDR (*(volatile uint32_t *)(MMIO_UART0_BASE + 0x00))
#define UARTFR (*(volatile uint32_t *)(MMIO_UART0_BASE + 0x18))
#define UARTFR_TXFF (1 << 5)
#define UARTFR_RXFE (1 << 4)

void uart_init(void) {
    vmm_map_device(MMIO_UART0_BASE);
}

void uart_putc(char c) {
    while (UARTFR & UARTFR_TXFF) { }
    UARTDR = (uint32_t)(uint8_t)c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

int uart_try_getc(char *out) {
    if (UARTFR & UARTFR_RXFE) {
        return 0;
    }
    *out = (char)(UARTDR & 0xFF);
    return 1;
}

void uart_puthex(uint64_t val) {
    static const char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc(digits[(val >> shift) & 0xF]);
    }
}

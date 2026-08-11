#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint64_t val);

/* Non-blocking: writes the received byte to *out and returns 1 if one was
   waiting in the RX FIFO, or returns 0 immediately if not. */
int uart_try_getc(char *out);

#endif

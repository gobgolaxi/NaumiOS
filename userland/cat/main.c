#include "../lib/naumi.h"

/* Demonstrates real userland file I/O: open a file from the FAT16 volume,
   read it in chunks, write each chunk to stdout (routed to UART by the
   kernel's SYS_WRITE handler), then exit. Proves SYS_OPEN/READ/WRITE/CLOSE
   work end-to-end from EL0, not just SYS_PRINT_VAL. */
void _start(void) {
    long fd = sys_open("hello.txt");
    if (fd < 0) {
        sys_exit();
    }

    char buf[64];
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        sys_write(1, buf, (unsigned long)n);
    }

    sys_close(fd);
    sys_exit();
}

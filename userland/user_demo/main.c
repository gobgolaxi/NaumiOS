#include "syscall.h"

/* Kernel jumps straight here via eret — no libc, no argc/argv, SP_EL0 is
   already valid (set up by task_create_user() before this task was ever
   scheduled), so a normal function prologue just works. */
void _start(void) {
    unsigned long n = 0;

    for (;;) {
        sys_print_val(n++);
        for (volatile long i = 0; i < 150000000; i++) { }
    }
}

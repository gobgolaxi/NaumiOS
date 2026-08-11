#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#define SYS_PRINT_VAL 0UL
#define SYS_EXIT      1UL

static inline unsigned long sys_print_val(unsigned long val) {
    register unsigned long x0 __asm__("x0") = val;
    register unsigned long x8 __asm__("x8") = SYS_PRINT_VAL;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline void sys_exit(void) {
    register unsigned long x8 __asm__("x8") = SYS_EXIT;
    __asm__ volatile ("svc #0" :: "r"(x8) : "memory");
    __builtin_unreachable();
}

#endif

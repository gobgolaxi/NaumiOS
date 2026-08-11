/* Real kernel entry point (see linker.ld: ENTRY(_start)).
   Limine guarantees EL1 with *a* valid stack already loaded, but doesn't
   document which SP bank (SP_EL0 vs SP_EL1) ends up selected — and getting
   this wrong is invisible until the first IRQ, which is dispatched based on
   PSTATE.SP at the moment it lands: an exception taken with SPSel=0 goes to
   the "current EL, SP_EL0" vector group, using SP_EL0, which nothing has
   ever initialized. So before doing anything else: force SPSel=1 (EL1h)
   and point SP_EL1 at a stack we control. */
.section .bss.stack, "aw", %nobits
.balign 16
stack_bottom:
.skip 64 * 1024
stack_top:

.section .text.boot
.global _start
_start:
    msr daifset, #0xf
    msr spsel, #1
    isb
    ldr x0, =stack_top
    mov sp, x0
    bl kmain
1:
    wfi
    b 1b

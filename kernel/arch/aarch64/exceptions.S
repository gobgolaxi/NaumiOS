.section .text.exceptions

.macro save_context
    sub sp, sp, #256
    stp x0, x1,   [sp, #16 * 0]
    stp x2, x3,   [sp, #16 * 1]
    stp x4, x5,   [sp, #16 * 2]
    stp x6, x7,   [sp, #16 * 3]
    stp x8, x9,   [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x19, [sp, #16 * 9]
    stp x20, x21, [sp, #16 * 10]
    stp x22, x23, [sp, #16 * 11]
    stp x24, x25, [sp, #16 * 12]
    stp x26, x27, [sp, #16 * 13]
    stp x28, x29, [sp, #16 * 14]
    str x30,      [sp, #16 * 15]
.endm

.macro restore_context
    ldp x0, x1,   [sp, #16 * 0]
    ldp x2, x3,   [sp, #16 * 1]
    ldp x4, x5,   [sp, #16 * 2]
    ldp x6, x7,   [sp, #16 * 3]
    ldp x8, x9,   [sp, #16 * 4]
    ldp x10, x11, [sp, #16 * 5]
    ldp x12, x13, [sp, #16 * 6]
    ldp x14, x15, [sp, #16 * 7]
    ldp x16, x17, [sp, #16 * 8]
    ldp x18, x19, [sp, #16 * 9]
    ldp x20, x21, [sp, #16 * 10]
    ldp x22, x23, [sp, #16 * 11]
    ldp x24, x25, [sp, #16 * 12]
    ldp x26, x27, [sp, #16 * 13]
    ldp x28, x29, [sp, #16 * 14]
    ldr x30,      [sp, #16 * 15]
    add sp, sp, #256
.endm

/* Each vector gets exactly 0x80 bytes (32 instructions) of space — far too
   little for save_context/restore_context/bl (~38 instructions), so each
   table entry is just a branch to its own named handler below, which is
   unrestricted in size. The vector number is only loaded into x0 *after*
   save_context, since the real x0 must be preserved untouched until then. */
.macro vector_entry vector_num
.balign 0x80
    b vector_handler_\vector_num
.endm

.balign 0x800
.global vector_table
vector_table:
    vector_entry 0   /* Synchronous, current EL, SP_EL0 */
    vector_entry 1   /* IRQ,         current EL, SP_EL0 */
    vector_entry 2   /* FIQ,         current EL, SP_EL0 */
    vector_entry 3   /* SError,      current EL, SP_EL0 */

    vector_entry 4   /* Synchronous, current EL, SP_ELx */
    vector_entry 5   /* IRQ,         current EL, SP_ELx */
    vector_entry 6   /* FIQ,         current EL, SP_ELx */
    vector_entry 7   /* SError,      current EL, SP_ELx */

    vector_entry 8   /* Synchronous, lower EL, AArch64 */
    vector_entry 9   /* IRQ,         lower EL, AArch64 */
    vector_entry 10  /* FIQ,         lower EL, AArch64 */
    vector_entry 11  /* SError,      lower EL, AArch64 */

    vector_entry 12  /* Synchronous, lower EL, AArch32 */
    vector_entry 13  /* IRQ,         lower EL, AArch32 */
    vector_entry 14  /* FIQ,         lower EL, AArch32 */
    vector_entry 15  /* SError,      lower EL, AArch32 */

.macro vector_handler vector_num
vector_handler_\vector_num:
    save_context
    mov x0, #\vector_num
    mov x1, sp
    bl exc_handler
    restore_context
    eret
.endm

vector_handler 0
vector_handler 1
vector_handler 2
vector_handler 3
vector_handler 4
vector_handler 5
vector_handler 6
vector_handler 7
vector_handler 8
vector_handler 9
vector_handler 10
vector_handler 11
vector_handler 12
vector_handler 13
vector_handler 14
vector_handler 15

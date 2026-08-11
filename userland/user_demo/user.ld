/* Standalone EL0 demo program, loaded by the kernel's ELF loader (not part
   of the kernel image itself — see kernel/loader/elf.c). One combined
   PT_LOAD segment keeps the loader simple: it doesn't need to reconcile
   per-segment RWX flags, just map every page RW+executable at EL0. */
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

PHDRS
{
    all PT_LOAD;
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
    } :all

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :all

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
    }
}

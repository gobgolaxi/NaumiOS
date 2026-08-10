/* AArch64 ELF64, higher-half kernel loaded by Limine. */
OUTPUT_FORMAT(elf64-littleaarch64)

ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD;
    text PT_LOAD;
    rodata PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    /* Limine spec mandates the kernel be placed in the topmost 2GiB
       of the address space; 0xffffffff80000000 is the conventional base. */
    . = 0xffffffff80000000;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        KEEP(*(.data .data.*))
    } :data

    /* .bss must be last in :data — otherwise the binary gets padded with zeros. */
    .bss : {
        KEEP(*(.bss .bss.*))
        KEEP(*(COMMON))
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}

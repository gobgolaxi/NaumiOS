#include <stdint.h>
#include <stddef.h>
#include "elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

/* Hand-rolled ELF64 structures — freestanding cross-toolchain has no
   <elf.h>, and this project only ever needs to read its own binaries. */
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

#define PT_LOAD 1UL
#define PF_X 1U

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_AARCH64 183

int elf_load(vmm_addrspace_t as, const void *data, size_t size, uint64_t *entry) {
    if (size < sizeof(elf64_ehdr_t)) {
        return -1;
    }

    const uint8_t *base = (const uint8_t *)data;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        return -1;
    }
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_AARCH64) {
        return -1;
    }
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        return -1;
    }

    uint64_t hhdm = pmm_hhdm_offset();

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (base + eh->e_phoff + (uint64_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD) {
            continue;
        }
        /* Keeps the loader simple: no partial-page relocation logic. The
           userland demo's linker script page-aligns every segment for
           exactly this reason. */
        if (ph->p_vaddr % PAGE_SIZE != 0) {
            return -1;
        }
        if (ph->p_offset + ph->p_filesz > size || ph->p_filesz > ph->p_memsz) {
            return -1;
        }

        uint64_t page_count = (ph->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;
        int executable = (ph->p_flags & PF_X) != 0;

        for (uint64_t p = 0; p < page_count; p++) {
            uint64_t phys = pmm_alloc_page();
            if (phys == 0) {
                return -1;
            }

            uint8_t *dst = (uint8_t *)(hhdm + phys);
            uint64_t page_off = p * PAGE_SIZE;
            for (uint64_t b = 0; b < PAGE_SIZE; b++) {
                uint64_t file_off = page_off + b;
                dst[b] = (file_off < ph->p_filesz) ? base[ph->p_offset + file_off] : 0;
            }

            uint64_t virt = ph->p_vaddr + page_off;
            if (executable) {
                vmm_map_user_code_in(as, virt, phys);
            } else {
                vmm_map_user_data_in(as, virt, phys);
            }
        }
    }

    *entry = eh->e_entry;
    return 0;
}

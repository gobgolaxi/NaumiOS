#include <stdint.h>
#include <stddef.h>
#include "spawn.h"
#include "elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"

/* Every spawned process gets the same stack VA — safe because each one
   lives in its own address space (see vmm_new_addrspace()); proven by the
   two concurrent user_demo instances that already run at identical VAs
   without colliding. 128MiB above the 0x400000 ELF load address (every
   userland program's own user.ld linker script): comfortable headroom for
   any single-PT_LOAD-segment program's combined .text/.rodata/.data/.bss
   to grow into without ever reaching the stack page — needed once
   userland/doom's ~80-file engine image is far bigger than this project's
   earlier, tiny userland programs. TASK_HEAP_BASE (kernel/sched/sched.h)
   starts at 256MiB, past this with its own margin. */
#define USER_STACK_VA 0x8000000UL

int spawn_elf_bytes(const char *name, const void *data, size_t size) {
    vmm_addrspace_t as = vmm_new_addrspace();

    uint64_t entry;
    if (elf_load(as, data, size, &entry) != 0) {
        return -1;
    }

    uint64_t stack_phys = pmm_alloc_page();
    if (stack_phys == 0) {
        return -1;
    }
    vmm_map_user_data_in(as, USER_STACK_VA, stack_phys);

    int pid = task_create_user(name, as, entry, USER_STACK_VA + PAGE_SIZE);
    return pid;
}

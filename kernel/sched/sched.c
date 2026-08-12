#include <stdint.h>
#include <stddef.h>
#include "sched.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../arch/aarch64/timer.h"

#define KSVC_BLOCK 0UL /* x0 = wait_queue_t* to link onto */
#define KSVC_SLEEP 1UL /* x0 = absolute tick to wake at */

#define KSTACK_SIZE (32 * 1024)
#define FRAME_WORDS 32 /* matches exceptions.S save_context: 256 bytes */
#define NAME_MAX 16
#define TASK_MAX_FDS 4

/* EL1h, D=1 A=1 I=0 F=1 — the PSTATE the kernel itself runs under once
   boot.S's `daifset #0xf` has been narrowed back down to just IRQs unmasked
   by irq_enable(). New kernel-mode tasks start in the same state. */
#define SPSR_TASK_INIT 0x345UL

/* EL0t (M[3:0] = 0000), same D=1 A=1 I=0 F=1. User-mode tasks start here. */
#define SPSR_USER_INIT 0x340UL

/* Per-task open file table. Whole-file-in-memory only (backed by
   fat16_read_file()'s own kmalloc'd buffer) — no seeking beyond a linear
   read cursor, no writes. Good enough for userland to read files off the
   FAT16 volume via SYS_OPEN/READ/CLOSE; a real VFS with proper file
   objects can replace this later without changing the syscall ABI. */
typedef struct {
    int used;
    uint8_t *data;
    uint32_t size;
    uint32_t pos;
} fd_slot_t;

/* Process control block. A circular, singly-linked list in scheduling
   order — no fixed task-count ceiling, since each PCB and its kernel-side
   stack come from kmalloc() rather than a static array. */
typedef struct task {
    int pid;
    int parent_pid; /* whoever was `current` when this task was created; -1 for task0 */
    char name[NAME_MAX];
    task_state_t state;

    uint64_t sp;     /* EL1-side (trap frame) stack pointer */
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0; /* only meaningful for EL0 tasks */
    vmm_addrspace_t as; /* fixed for the task's whole lifetime */

    uint64_t wake_tick;      /* nonzero while asleep: tick to wake at */
    struct task *wait_next;  /* wait_queue_t linkage; unused otherwise */

    uint64_t heap_brk; /* current SYS_SBRK break; 0 for kernel-mode tasks (unused) */

    fd_slot_t fds[TASK_MAX_FDS];

    struct task *next; /* round-robin scheduling order */
} task_t;

static task_t *task0;    /* pid 0, kmain — the permanent fallback */
static task_t *current;
static int next_pid;
static int sched_enabled;

static void set_name(task_t *t, const char *name) {
    size_t i = 0;
    for (; name != NULL && name[i] != '\0' && i < NAME_MAX - 1; i++) {
        t->name[i] = name[i];
    }
    t->name[i] = '\0';
}

void sched_init(void) {
    task0 = (task_t *)kmalloc(sizeof(task_t));

    task0->pid = 0;
    task0->parent_pid = -1;
    set_name(task0, "kmain");
    task0->state = TASK_STATE_READY;
    /* sp/elr/spsr/sp_el0 get filled in on task0's first preemption, which
       always happens before it's ever picked as "next" — it's whatever's
       currently running. Its address space is fixed and known up front,
       though: kmain runs in the kernel's own. */
    task0->as = vmm_kernel_addrspace();
    task0->wake_tick = 0;
    task0->wait_next = NULL;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        task0->fds[i].used = 0;
    }
    task0->next = task0;

    current = task0;
    next_pid = 1;
    sched_enabled = 0;
}

/* Allocates a PCB + its own kernel-side stack, lays down a synthetic frame
   matching exceptions.S's save_context layout (so the first "resume" looks
   identical to resuming one that actually took a trap), and links it in
   right after the currently-running task. Leaves elr/spsr/sp_el0/as for the
   caller to fill in — those differ between kernel- and user-mode tasks. */
static task_t *alloc_task(const char *name) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) {
        return NULL;
    }
    uint8_t *stack = (uint8_t *)kmalloc(KSTACK_SIZE);
    if (!stack) {
        kfree(t);
        return NULL;
    }

    uint64_t *stack_top = (uint64_t *)(stack + KSTACK_SIZE);
    uint64_t *frame = stack_top - FRAME_WORDS;
    for (int i = 0; i < FRAME_WORDS; i++) {
        frame[i] = 0;
    }

    t->pid = next_pid++;
    t->parent_pid = current->pid;
    set_name(t, name);
    t->state = TASK_STATE_READY;
    t->sp = (uint64_t)frame;
    t->wake_tick = 0;
    t->wait_next = NULL;
    t->heap_brk = 0;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        t->fds[i].used = 0;
    }

    t->next = current->next;
    current->next = t;

    sched_enabled = 1;
    return t;
}

int task_create(const char *name, void (*entry)(void)) {
    task_t *t = alloc_task(name);
    if (!t) {
        return -1;
    }

    t->elr = (uint64_t)entry;
    t->spsr = SPSR_TASK_INIT;
    t->sp_el0 = 0;
    t->as = vmm_kernel_addrspace();
    return t->pid;
}

int task_create_user(const char *name, vmm_addrspace_t as, uint64_t entry_va, uint64_t user_sp_top) {
    task_t *t = alloc_task(name);
    if (!t) {
        return -1;
    }

    t->elr = entry_va;
    t->spsr = SPSR_USER_INIT;
    t->sp_el0 = user_sp_top;
    t->as = as;
    t->heap_brk = TASK_HEAP_BASE;
    return t->pid;
}

/* task0 never exits or blocks, so this always terminates. */
static task_t *pick_next(void) {
    task_t *t = current->next;
    while (t->state != TASK_STATE_READY) {
        t = t->next;
    }
    return t;
}

/* Every task's own trap frame (its kmalloc'd kernel stack) lives in kernel
   .bss/heap, reachable via TTBR1 regardless of TTBR0, so it's safe to
   switch address spaces (see vmm_switch() below) even mid-exception:
   nothing between that point and eret touches TTBR0-mapped memory except
   the code the task itself resumes into. */
static void restore_next(void) {
    __asm__ volatile ("msr elr_el1, %0" :: "r"(current->elr));
    __asm__ volatile ("msr spsr_el1, %0" :: "r"(current->spsr));
    __asm__ volatile ("msr sp_el0, %0" :: "r"(current->sp_el0));
}

/* Moves any BLOCKED task whose sleep deadline has passed back to READY.
   Called once per timer tick, before the current task is swapped out. */
static void wake_sleepers(void) {
    uint64_t now = timer_ticks();
    task_t *t = task0;
    do {
        if (t->state == TASK_STATE_BLOCKED && t->wake_tick != 0 && now >= t->wake_tick) {
            t->state = TASK_STATE_READY;
            t->wake_tick = 0;
        }
        t = t->next;
    } while (t != task0);
}

uint64_t sched_tick(uint64_t *regs) {
    if (!sched_enabled) {
        return (uint64_t)regs;
    }

    wake_sleepers();

    current->sp = (uint64_t)regs;
    __asm__ volatile ("mrs %0, elr_el1" : "=r"(current->elr));
    __asm__ volatile ("mrs %0, spsr_el1" : "=r"(current->spsr));
    __asm__ volatile ("mrs %0, sp_el0" : "=r"(current->sp_el0));

    current = pick_next();
    restore_next();
    vmm_switch(current->as);

    return current->sp;
}

uint64_t sched_exit_current(void) {
    current->state = TASK_STATE_ZOMBIE;

    current = pick_next();
    restore_next();
    vmm_switch(current->as);

    return current->sp;
}

int sched_current_pid(void) {
    return current->pid;
}

int sched_task_alive(int pid) {
    task_t *t = task0;
    do {
        if (t->pid == pid) {
            return t->state != TASK_STATE_ZOMBIE;
        }
        t = t->next;
    } while (t != task0);
    return 0;
}

void sched_kill(int pid) {
    task_t *t = task0;
    do {
        if (t->pid == pid) {
            t->state = TASK_STATE_ZOMBIE;
            return;
        }
        t = t->next;
    } while (t != task0);
}

int64_t sched_sbrk(int64_t increment) {
    uint64_t old_brk = current->heap_brk;
    if (increment == 0) {
        return (int64_t)old_brk;
    }
    if (increment < 0) {
        return -1; /* shrinking isn't supported */
    }
    uint64_t new_brk = old_brk + (uint64_t)increment;
    if (new_brk > TASK_HEAP_MAX || new_brk < old_brk) {
        return -1; /* past the fixed heap window, or overflowed */
    }

    /* Whatever was mapped by the previous call always covers exactly
       [TASK_HEAP_BASE, page_align_up(old_brk)) — recomputing that bound
       from old_brk here (rather than tracking it separately) is always
       correct precisely because that invariant holds after every call. */
    uint64_t cur_end = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t needed_end = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t va = cur_end; va < needed_end; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            return -1; /* out of physical memory */
        }
        vmm_map_user_data_in(current->as, va, phys);
    }

    current->heap_brk = new_brk;
    return (int64_t)old_brk;
}

/* Takes ownership of `data` (a kmalloc'd buffer, e.g. from
   fat16_read_file()) — sched_fd_close() kfree()s it. Returns an fd >= 0 on
   success, -1 if the calling task's fd table is full. */
int sched_fd_open(uint8_t *data, uint32_t size) {
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (!current->fds[i].used) {
            current->fds[i].used = 1;
            current->fds[i].data = data;
            current->fds[i].size = size;
            current->fds[i].pos = 0;
            return i;
        }
    }
    return -1;
}

int sched_fd_read(int fd, void *buf, uint32_t len) {
    if (fd < 0 || fd >= TASK_MAX_FDS || !current->fds[fd].used) {
        return -1;
    }
    fd_slot_t *f = &current->fds[fd];
    uint32_t remaining = f->size - f->pos;
    uint32_t take = len < remaining ? len : remaining;

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < take; i++) {
        dst[i] = f->data[f->pos + i];
    }
    f->pos += take;
    return (int)take;
}

int sched_fd_close(int fd) {
    if (fd < 0 || fd >= TASK_MAX_FDS || !current->fds[fd].used) {
        return -1;
    }
    kfree(current->fds[fd].data);
    current->fds[fd].used = 0;
    return 0;
}

void sched_for_each(sched_visitor_t visit) {
    task_t *t = task0;
    do {
        visit(t->pid, t->name, t->state, t == current);
        t = t->next;
    } while (t != task0);
}

void sched_wake_one(wait_queue_t *wq) {
    task_t *t = wq->head;
    if (!t) {
        return;
    }
    wq->head = t->wait_next;
    t->wait_next = NULL;
    t->state = TASK_STATE_READY;
}

void sched_wake_all(wait_queue_t *wq) {
    task_t *t = wq->head;
    while (t) {
        task_t *next = t->wait_next;
        t->wait_next = NULL;
        t->state = TASK_STATE_READY;
        t = next;
    }
    wq->head = NULL;
}

/* Both sched_block() and sched_sleep_ticks() are plain function calls from
   task context, but only the exception path (save_context/mov sp,x0/
   restore_context/eret in exceptions.S) can safely swap SP/ELR/SPSR/TTBR0
   out from under the caller. `svc` from EL1 lands on vector 4 (Synchronous,
   EL1, SP_ELx) — same vector table, same machinery the timer IRQ already
   uses, just triggered voluntarily instead of on a tick. */
void sched_block(wait_queue_t *wq) {
    register uint64_t x0 __asm__("x0") = (uint64_t)wq;
    register uint64_t x8 __asm__("x8") = KSVC_BLOCK;
    __asm__ volatile ("svc #0" :: "r"(x0), "r"(x8) : "memory");
}

void sched_sleep_ticks(uint64_t ticks) {
    if (ticks == 0) {
        return;
    }
    register uint64_t x0 __asm__("x0") = timer_ticks() + ticks;
    register uint64_t x8 __asm__("x8") = KSVC_SLEEP;
    __asm__ volatile ("svc #0" :: "r"(x0), "r"(x8) : "memory");
}

uint64_t sched_handle_el1_svc(uint64_t *regs) {
    uint64_t op = regs[8];
    uint64_t arg = regs[0];

    current->sp = (uint64_t)regs;
    __asm__ volatile ("mrs %0, elr_el1" : "=r"(current->elr));
    __asm__ volatile ("mrs %0, spsr_el1" : "=r"(current->spsr));
    __asm__ volatile ("mrs %0, sp_el0" : "=r"(current->sp_el0));

    current->state = TASK_STATE_BLOCKED;
    if (op == KSVC_SLEEP) {
        current->wake_tick = arg;
    } else {
        wait_queue_t *wq = (wait_queue_t *)arg;
        current->wait_next = NULL;
        if (!wq->head) {
            wq->head = current;
        } else {
            task_t *t = wq->head;
            while (t->wait_next) {
                t = t->wait_next;
            }
            t->wait_next = current;
        }
    }

    current = pick_next();
    restore_next();
    vmm_switch(current->as);

    return current->sp;
}

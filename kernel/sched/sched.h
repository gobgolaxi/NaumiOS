#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "../mm/vmm.h"

typedef enum {
    TASK_STATE_READY,
    TASK_STATE_BLOCKED, /* waiting on a wait_queue_t, or asleep until a tick */
    TASK_STATE_ZOMBIE,  /* exited via SYS_EXIT; PCB kept around, not reaped */
} task_state_t;

/* A FIFO of blocked tasks, threaded through each task's own wait_next
   field (separate from the round-robin `next` list, so blocking doesn't
   disturb scheduling order). Opaque to callers beyond zero-init. */
typedef struct wait_queue {
    struct task *head;
} wait_queue_t;

#define WAIT_QUEUE_INIT { .head = NULL }

/* Registers task 0 (pid 0, "kmain") as the caller's own execution context.
   Must run before the first timer tick that could preempt it. */
void sched_init(void);

/* Registers a new EL1 (kernel-mode) task that starts executing at `entry`
   once scheduled. All kernel-mode tasks share the kernel's own address
   space (vmm_kernel_addrspace()) — no isolation between them, since
   they're all part of the trusted kernel. Returns its pid, or -1 on
   allocation failure. */
int task_create(const char *name, void (*entry)(void));

/* Registers a new EL0 (user-mode) task — a real process, with its own
   address space `as` (see vmm_new_addrspace() + elf_load()), isolated from
   every other process and from the kernel's own mappings beyond the device
   entries vmm_new_addrspace() replicated into it. `entry_va` and
   `user_sp_top` are addresses within that address space, not kernel
   pointers. Returns its pid, or -1 on allocation failure. */
int task_create_user(const char *name, vmm_addrspace_t as, uint64_t entry_va, uint64_t user_sp_top);

/* Called from the timer IRQ path with a pointer to the just-saved x0-x30
   frame on the interrupted task's own stack. Saves the outgoing task's
   context (frame pointer + ELR_EL1/SPSR_EL1/SP_EL0, none of which are part
   of that frame), picks the next ready task round-robin, restores its
   saved context (including its address space — see vmm_switch()), and
   returns the SP the caller should switch to before eret. */
uint64_t sched_tick(uint64_t *regs);

/* Drops the currently running task from the ready set (it called
   SYS_EXIT — see exceptions.c) and switches to the next one, the same way
   sched_tick() does, minus saving the exiting task's own context. The PCB
   is marked TASK_STATE_ZOMBIE, not freed — there's no wait()/reap
   mechanism yet. */
uint64_t sched_exit_current(void);

/* pid of the task currently executing. For syscalls that want to identify
   their caller (e.g. tagging SYS_PRINT_VAL output). */
int sched_current_pid(void);

/* Visits every task (READY and ZOMBIE alike) in scheduling order, starting
   from task 0. For the shell's `ps` command. */
typedef void (*sched_visitor_t)(int pid, const char *name, task_state_t state, int is_current);
void sched_for_each(sched_visitor_t visit);

/* Blocks the calling task on `wq` and switches away immediately — a
   voluntary, synchronous reschedule (not tied to the next timer tick).
   Returns once some other task calls sched_wake_one()/sched_wake_all() on
   `wq` and the scheduler picks this task again. Must be called from task
   context (not from inside an interrupt handler). */
void sched_block(wait_queue_t *wq);

/* Moves the single longest-waiting task on `wq` back to READY, if any.
   Doesn't itself force an immediate switch — the woken task just becomes
   eligible the next time the scheduler runs. */
void sched_wake_one(wait_queue_t *wq);

/* Moves every task on `wq` back to READY. */
void sched_wake_all(wait_queue_t *wq);

/* Blocks the calling task until at least `ticks` timer ticks have passed.
   Built on the same mechanism as sched_block(), woken from sched_tick()
   instead of an explicit wake call. */
void sched_sleep_ticks(uint64_t ticks);

/* Dispatches a kernel-mode (EL1) SVC — the mechanism sched_block()/
   sched_sleep_ticks() use internally to trigger an immediate, safe
   reschedule via the same save/restore path the timer IRQ uses. Not meant
   to be called directly: exceptions.c wires this to vector 4 (Synchronous,
   EL1, SP_ELx) SVC traps. */
uint64_t sched_handle_el1_svc(uint64_t *regs);

/* Per-task open file table, backing SYS_OPEN/SYS_READ/SYS_CLOSE. Takes
   ownership of a kmalloc'd buffer (sched_fd_open) and frees it on close.
   All three operate on the *currently running* task. */
int sched_fd_open(uint8_t *data, uint32_t size);
int sched_fd_read(int fd, void *buf, uint32_t len);
int sched_fd_close(int fd);

/* 1 if `pid` names a task that exists and hasn't exited (state != ZOMBIE),
   0 otherwise (including pids that never existed). The compositor polls
   this to garbage-collect windows whose owning process has exited without
   an explicit close. */
int sched_task_alive(int pid);

/* Forcibly ends `pid` (marks it ZOMBIE) without it having called SYS_EXIT
   itself — e.g. the compositor closing a window via its titlebar X while
   the owning process is still running. Safe to call on any task other than
   the one currently executing (that's always true here: only the
   compositor task calls this, targeting some other process's pid). Like
   SYS_EXIT, the PCB/stack/address space aren't reaped, matching this
   project's current no-reap limitation. No-op if `pid` doesn't exist or is
   already a zombie. */
void sched_kill(int pid);

#endif

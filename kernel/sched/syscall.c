#include <stdint.h>
#include "syscall.h"
#include "sched.h"
#include "../drivers/uart.h"
#include "../gfx/compositor.h"
#include "../fs/fat16.h"
#include "../mm/heap.h"
#include "../loader/spawn.h"

/* SYS_OPEN/READ/WRITE all dereference caller-supplied pointers (path, buf)
   directly. This is safe *only* because syscall_dispatch() runs with the
   calling task's own TTBR0 still loaded (the SVC path never calls
   vmm_switch()) and its pages are mapped EL1 RW (see vmm_map_user_code/
   vmm_map_user_data) — so an EL1 read/write of a valid user pointer just
   works. There is no validation of these pointers beyond that: a userland
   program passing a bad or unmapped pointer causes a kernel-context data
   abort, which today's exc_handler treats as unrecoverable (prints
   diagnostics and halts the whole system). Fine for this project's own,
   trusted userland; a real syscall boundary would need bounded/faulting
   copy_from_user-style access instead. */

#define MAX_PATH 32
#define MAX_TITLE 32
#define MAX_TEXT 64

static uint64_t sys_open(const char *path) {
    char name[MAX_PATH];
    size_t i = 0;
    for (; path[i] != '\0' && i < MAX_PATH - 1; i++) {
        name[i] = path[i];
    }
    name[i] = '\0';

    uint8_t *data;
    uint32_t size;
    if (fat16_read_file(name, &data, &size) != 0) {
        return (uint64_t)-1;
    }

    int fd = sched_fd_open(data, size);
    if (fd < 0) {
        kfree(data);
        return (uint64_t)-1;
    }
    return (uint64_t)fd;
}

static uint64_t sys_write(uint64_t fd, const uint8_t *buf, uint64_t len) {
    if (fd != 1) { /* only stdout goes anywhere */
        return (uint64_t)-1;
    }
    for (uint64_t i = 0; i < len; i++) {
        uart_putc((char)buf[i]);
    }
    return len;
}

static uint64_t sys_win_create(const struct sys_win_create *req, const char *title) {
    char buf[MAX_TITLE];
    size_t i = 0;
    for (; title[i] != '\0' && i < MAX_TITLE - 1; i++) {
        buf[i] = title[i];
    }
    buf[i] = '\0';

    return (uint64_t)compositor_create_window(sched_current_pid(), req->w, req->h, buf, req->flags);
}

static uint64_t sys_fb_info(struct sys_fb_info *out) {
    uint32_t w = 0, h = 0;
    int rc = compositor_get_client_size(sched_current_pid(), &w, &h);
    out->width = w;
    out->height = h;
    return (uint64_t)rc;
}

static uint64_t sys_fb_fill_rect(const struct sys_fb_rect *r) {
    compositor_fill_rect(sched_current_pid(), r->x, r->y, r->w, r->h, r->color);
    return 0;
}

static uint64_t sys_poll_input(uint64_t which, struct sys_input_event *out) {
    struct sys_win_event ev;
    if (!compositor_poll_event(sched_current_pid(), (int)which, &ev)) {
        return 0;
    }
    out->type = ev.type;
    out->code = ev.code;
    out->value = ev.value;
    out->x = ev.x;
    out->y = ev.y;
    return 1;
}

static uint64_t sys_fb_draw_text(const struct sys_fb_text *req, const char *text) {
    char buf[MAX_TEXT];
    size_t i = 0;
    for (; text[i] != '\0' && i < MAX_TEXT - 1; i++) {
        buf[i] = text[i];
    }
    buf[i] = '\0';

    compositor_draw_text(sched_current_pid(), req->x, req->y, buf, req->fg, req->bg, req->scale);
    return 0;
}

static int contains_slash(const char *s) {
    for (; *s; s++) {
        if (*s == '/') {
            return 1;
        }
    }
    return 0;
}

/* Bare names ("cat.elf") resolve under /bin, same convention as the
   shell's `run` — so a GUI app launching another one doesn't need to know
   the filesystem layout, just the program's name. */
static uint64_t sys_spawn(const char *user_path) {
    char raw[MAX_PATH];
    size_t i = 0;
    for (; user_path[i] != '\0' && i < MAX_PATH - 1; i++) {
        raw[i] = user_path[i];
    }
    raw[i] = '\0';

    char path[MAX_PATH];
    if (contains_slash(raw)) {
        size_t n = 0;
        for (; raw[n] != '\0' && n < sizeof(path) - 1; n++) {
            path[n] = raw[n];
        }
        path[n] = '\0';
    } else {
        const char *prefix = "bin/";
        size_t n = 0;
        for (; prefix[n]; n++) {
            path[n] = prefix[n];
        }
        size_t m = 0;
        for (; raw[m] != '\0' && n + m < sizeof(path) - 1; m++) {
            path[n + m] = raw[m];
        }
        path[n + m] = '\0';
    }

    uint8_t *data;
    uint32_t size;
    if (fat16_read_file(path, &data, &size) != 0) {
        return (uint64_t)-1;
    }

    int pid = spawn_elf_bytes(raw, data, size);
    kfree(data);
    return (uint64_t)pid;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    switch (num) {
    case SYS_PRINT_VAL:
        /* Tags output with the caller's pid — with each process now
           isolated in its own address space, two instances of the same
           program hold independent state at the identical virtual address,
           and this is how that's visible from the log. */
        uart_puts("[pid "); uart_puthex((uint64_t)sched_current_pid()); uart_puts("] val: ");
        uart_puthex(a0);
        uart_puts("\n");
        return 0;
    case SYS_OPEN:
        return sys_open((const char *)a0);
    case SYS_READ:
        return (uint64_t)sched_fd_read((int)a0, (void *)a1, (uint32_t)a2);
    case SYS_WRITE:
        return sys_write(a0, (const uint8_t *)a1, a2);
    case SYS_CLOSE:
        return (uint64_t)sched_fd_close((int)a0);
    case SYS_FB_INFO:
        return sys_fb_info((struct sys_fb_info *)a0);
    case SYS_FB_FILL_RECT:
        return sys_fb_fill_rect((const struct sys_fb_rect *)a0);
    case SYS_POLL_INPUT:
        return sys_poll_input(a0, (struct sys_input_event *)a1);
    case SYS_FB_PRESENT:
        compositor_present(sched_current_pid());
        return 0;
    case SYS_FB_DRAW_TEXT:
        return sys_fb_draw_text((const struct sys_fb_text *)a0, (const char *)a1);
    case SYS_SPAWN:
        return sys_spawn((const char *)a0);
    case SYS_HAS_FOCUS:
        return (uint64_t)compositor_is_focused(sched_current_pid());
    case SYS_WIN_CREATE:
        return sys_win_create((const struct sys_win_create *)a0, (const char *)a1);
    default:
        uart_puts("[user] unknown syscall: ");
        uart_puthex(num);
        uart_puts("\n");
        return (uint64_t)-1;
    }
}

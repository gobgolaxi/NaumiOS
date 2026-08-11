#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* Minimal ABI: syscall number in x8, up to 3 args in x0-x2, result in x0.
   Mirrors the SVC dispatch in exceptions.c (vector 8, EC 0x15). */
#define SYS_PRINT_VAL 0UL
#define SYS_EXIT      1UL
#define SYS_OPEN      2UL /* a0 = path (NUL-terminated, in caller's own address space) */
#define SYS_READ      3UL /* a0 = fd, a1 = buf, a2 = len */
#define SYS_WRITE     4UL /* a0 = fd (only 1/stdout goes anywhere), a1 = buf, a2 = len */
#define SYS_CLOSE     5UL /* a0 = fd */
#define SYS_FB_INFO      6UL /* a0 = struct sys_fb_info* (out) in caller's own address space */
#define SYS_FB_FILL_RECT 7UL /* a0 = struct sys_fb_rect* (in) in caller's own address space */
#define SYS_POLL_INPUT   8UL /* a0 = 0 keyboard / 1 mouse, a1 = struct sys_input_event* (out) */
#define SYS_FB_PRESENT   9UL /* no args — copies the back buffer to the real screen */
#define SYS_FB_DRAW_TEXT 10UL /* a0 = struct sys_fb_text* (in), a1 = text (NUL-terminated) */
#define SYS_SPAWN        11UL /* a0 = path (NUL-terminated) — resolved under /bin like the shell's `run` */
#define SYS_HAS_FOCUS    12UL /* no args — 1 if this task's window currently has keyboard focus, else 0 */
#define SYS_WIN_CREATE   13UL /* a0 = struct sys_win_create* (in), a1 = title (NUL-terminated) */

/* Kernel-mediated drawing/input, now backed by a real per-process window
   (see kernel/gfx/compositor.h) instead of a single shared framebuffer:
   SYS_FB_INFO/FILL_RECT/DRAW_TEXT/PRESENT all operate on the calling
   process's own window (window-relative coordinates, clipped to its client
   area) rather than the whole screen. Layouts here must exactly match the
   userland copies in userland/lib/naumi.h. */
struct sys_fb_info {
    uint64_t width;  /* caller's window client area, once it has one */
    uint64_t height;
};

struct sys_fb_rect {
    uint32_t x, y, w, h;
    uint32_t color;
};

struct sys_input_event {
    uint16_t type;
    uint16_t code;
    int32_t value;
    int32_t x; /* window-relative; only meaningful for mouse click events */
    int32_t y;
};

struct sys_fb_text {
    uint32_t x, y;
    uint32_t fg, bg;
    uint32_t scale;
};

struct sys_win_create {
    uint32_t w, h;      /* client area size; ignored (forced fullscreen) for WIN_FLAG_BORDERLESS */
    uint32_t flags;     /* see WIN_FLAG_BORDERLESS in kernel/gfx/compositor.h */
};

/* SYS_EXIT is intercepted before reaching here (it needs to drop the
   calling task from the scheduler, not just return a value) — see
   exceptions.c. */
uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2);

#endif

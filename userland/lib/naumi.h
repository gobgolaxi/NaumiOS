#ifndef NAUMI_LIBC_H
#define NAUMI_LIBC_H

/* Minimal userland "libc" for NaumiOS: thin wrappers around the syscall
   ABI (number in x8, up to 3 args in x0-x2, result in x0 — see
   kernel/sched/syscall.h). No libc headers exist in this freestanding
   toolchain, so plain `unsigned long`/`long` instead of size_t/ssize_t. */

#define SYS_PRINT_VAL    0UL
#define SYS_EXIT         1UL
#define SYS_OPEN         2UL
#define SYS_READ         3UL
#define SYS_WRITE        4UL
#define SYS_CLOSE        5UL
#define SYS_FB_INFO      6UL
#define SYS_FB_FILL_RECT 7UL
#define SYS_POLL_INPUT   8UL
#define SYS_FB_PRESENT   9UL
#define SYS_FB_DRAW_TEXT 10UL
#define SYS_SPAWN        11UL
#define SYS_HAS_FOCUS    12UL
#define SYS_WIN_CREATE   13UL

/* Layouts must exactly match kernel/sched/syscall.h's struct sys_fb_info /
   sys_fb_rect / sys_input_event / sys_win_create — the kernel reads/writes
   these directly through the caller's own still-live TTBR0 during the
   syscall, no IPC marshalling involved. */
struct fb_info {
    unsigned long width;  /* this window's client area, once it has one */
    unsigned long height;
};

struct fb_rect {
    unsigned int x, y, w, h;
    unsigned int color;
};

/* which=0 (keyboard) events carry type/code/value only (x=y=0). which=1
   (mouse) events are always a BTN_LEFT press/release with (x, y) already
   translated to coordinates relative to this window's own client area —
   raw pointer motion is consumed by the kernel compositor for cursor
   tracking/dragging and never reaches userland. type == EV_RESIZE is
   synthetic (delivered on the keyboard queue): this window's client size
   changed (e.g. after a maximize toggle) — x/y carry the new width/height. */
struct input_event_wire {
    unsigned short type;
    unsigned short code;
    int value;
    int x;
    int y;
};

struct fb_text {
    unsigned int x, y;
    unsigned int fg, bg;
    unsigned int scale;
};

struct win_create {
    unsigned int w, h;   /* client area size; ignored (forced fullscreen) for WIN_BORDERLESS */
    unsigned int flags;
};

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_RESIZE 3
#define REL_X 0
#define REL_Y 1
#define KEY_ESC 1
#define BTN_LEFT 0x110

#define WIN_BORDERLESS 1u

static inline long naumi_syscall(unsigned long num, unsigned long a0,
                                  unsigned long a1, unsigned long a2) {
    register unsigned long x0 __asm__("x0") = a0;
    register unsigned long x1 __asm__("x1") = a1;
    register unsigned long x2 __asm__("x2") = a2;
    register unsigned long x8 __asm__("x8") = num;
    __asm__ volatile ("svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory");
    return (long)x0;
}

static inline void sys_exit(void) {
    naumi_syscall(SYS_EXIT, 0, 0, 0);
    __builtin_unreachable();
}

static inline long sys_print_val(unsigned long val) {
    return naumi_syscall(SYS_PRINT_VAL, val, 0, 0);
}

static inline long sys_open(const char *path) {
    return naumi_syscall(SYS_OPEN, (unsigned long)path, 0, 0);
}

static inline long sys_read(long fd, void *buf, unsigned long len) {
    return naumi_syscall(SYS_READ, (unsigned long)fd, (unsigned long)buf, len);
}

static inline long sys_write(long fd, const void *buf, unsigned long len) {
    return naumi_syscall(SYS_WRITE, (unsigned long)fd, (unsigned long)buf, len);
}

static inline long sys_close(long fd) {
    return naumi_syscall(SYS_CLOSE, (unsigned long)fd, 0, 0);
}

static inline long sys_fb_info(struct fb_info *out) {
    return naumi_syscall(SYS_FB_INFO, (unsigned long)out, 0, 0);
}

static inline long sys_fb_fill_rect(const struct fb_rect *r) {
    return naumi_syscall(SYS_FB_FILL_RECT, (unsigned long)r, 0, 0);
}

static inline long sys_fb_fill(unsigned int x, unsigned int y, unsigned int w,
                                unsigned int h, unsigned int color) {
    struct fb_rect r = { x, y, w, h, color };
    return sys_fb_fill_rect(&r);
}

/* which: 0 = keyboard, 1 = mouse. Returns 1 with *out filled if an event
   was waiting, 0 if not (non-blocking, matches the kernel-side driver). */
static inline long sys_poll_input(int which, struct input_event_wire *out) {
    return naumi_syscall(SYS_POLL_INPUT, (unsigned long)which, (unsigned long)out, 0);
}

/* Nothing drawn (fill_rect, draw_text) is visible until this runs — see
   framebuffer.h's back buffer. Call once per finished frame. */
static inline long sys_fb_present(void) {
    return naumi_syscall(SYS_FB_PRESENT, 0, 0, 0);
}

static inline long sys_fb_draw_text(const struct fb_text *req, const char *text) {
    return naumi_syscall(SYS_FB_DRAW_TEXT, (unsigned long)req, (unsigned long)text, 0);
}

static inline long sys_fb_text(unsigned int x, unsigned int y, const char *text,
                                unsigned int fg, unsigned int bg, unsigned int scale) {
    struct fb_text req = { x, y, fg, bg, scale };
    return sys_fb_draw_text(&req, text);
}

/* Creates this process's window — every graphical program gets at most
   one, and every framebuffer/input call above implicitly targets it.
   `w`/`h` are the desired client area size (ignored, forced to the full
   screen, for WIN_BORDERLESS — that's how the desktop itself works, see
   wm.c). Returns 0 on success, -1 if this process already has a window or
   the window table is full. Must be called before any other framebuffer
   or input call. */
static inline long sys_win_create(unsigned int w, unsigned int h, unsigned int flags,
                                   const char *title) {
    struct win_create req = { w, h, flags };
    return naumi_syscall(SYS_WIN_CREATE, (unsigned long)&req, (unsigned long)title, 0);
}

/* Bare names ("cat.elf") resolve under /bin, same as the shell's `run`.
   Returns the new pid, or -1 if not found / load failed. Runs as an
   ordinary sibling window, not a takeover: the caller keeps its own window
   and keyboard focus unless/until the user clicks into the new one. */
static inline long sys_spawn(const char *path) {
    return naumi_syscall(SYS_SPAWN, (unsigned long)path, 0, 0);
}

/* 1 if this window currently has keyboard focus (i.e. is the topmost
   bordered window, or the desktop if nothing else is up), else 0. */
static inline long sys_has_focus(void) {
    return naumi_syscall(SYS_HAS_FOCUS, 0, 0, 0);
}

static inline unsigned int rgb(unsigned char r, unsigned char g, unsigned char b) {
    /* Matches this project's fb_rgb(): red_shift=16, green_shift=8,
       blue_shift=0 on every backend seen so far (ramfb, virtio-gpu). Not
       queried from the kernel — if that ever changes, this needs to move
       into a real syscall instead of being assumed. */
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
}

#endif

#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>

/* The real window system: every graphical process owns at most one window,
   created via SYS_WIN_CREATE (see syscall.c). The kernel — specifically
   this module, driven by a dedicated kernel task (compositor_task) — owns
   the window table, draws chrome (title bar/border/buttons) generically
   since it already knows every window's geometry, composites all visible
   windows by z-order into the real screen buffer, and does all input
   hit-testing (dragging, close/maximize buttons, click routing). Apps only
   ever draw into their own private buffer using window-relative
   coordinates and never see another window's pixels or chrome.

   WIN_FLAG_BORDERLESS windows get no chrome at all and are pinned to the
   bottom of z-order — this is how the desktop (wm.elf) works: it's just an
   ordinary window that happens to be full-screen and undecorated, not a
   special kernel concept. */

#define WIN_FLAG_BORDERLESS 1u

/* Delivered to a window's keyboard queue (which=0) or mouse queue (which=1)
   — see compositor_poll_event(). Mouse queue entries are always
   BTN_LEFT-style press/release with (x, y) already translated to
   window-relative client coordinates; raw pointer motion never reaches
   userland; the compositor consumes it itself for cursor tracking/dragging.
   type == SYS_EV_RESIZE is a synthetic event (delivered on the keyboard
   queue) telling the window its client size changed (e.g. after a
   maximize toggle) — x/y carry the new client width/height. */
#define SYS_EV_RESIZE 3u

struct sys_win_event {
    uint16_t type;
    uint16_t code;
    int32_t value;
    int32_t x;
    int32_t y;
};

/* Must run once, after fb_init() and before compositor_task ever executes
   (it needs fb_width()/fb_height() to size the desktop). */
void compositor_init(void);

/* Runs forever as its own kernel task (see task_create() in main.c): drains
   virtio keyboard/mouse events, updates the cursor, handles chrome
   hit-testing/dragging, routes events into the right window's queue,
   garbage-collects windows whose owning process has exited, and
   recomposites+presents whenever something changed. */
void compositor_task(void);

/* Everything below is implicitly scoped to `pid`'s own window — one window
   per process. Called from syscall.c's SYS_WIN_CREATE, SYS_FB_ family, and
   SYS_POLL_INPUT handlers, never directly by userland. */

/* Returns 0 on success, -1 if the window table is full or `pid` already
   owns a window. WIN_FLAG_BORDERLESS windows ignore w/h and are always
   sized to the full screen. */
int compositor_create_window(int pid, uint32_t w, uint32_t h, const char *title, uint32_t flags);

/* Client-area size of `pid`'s window. Returns -1 (leaving w and h untouched)
   if it doesn't have one. */
int compositor_get_client_size(int pid, uint32_t *w, uint32_t *h);

void compositor_fill_rect(int pid, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void compositor_draw_text(int pid, uint32_t x, uint32_t y, const char *text,
                           uint32_t fg, uint32_t bg, uint32_t scale);

/* Copies a w*h, tightly packed, row-major 32bpp pixel buffer into `pid`'s
   window at window-relative (x, y), clipped to its client area — the fast
   path for pushing an entire pre-rendered frame at once (e.g. a game's own
   software renderer), instead of many small compositor_fill_rect() calls. */
void compositor_blit(int pid, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels);

/* Marks the whole desktop dirty so compositor_task recomposites+presents on
   its next iteration — cheap to call every frame; the actual work is
   deferred and coalesced onto the compositor task's own cadence. */
void compositor_present(int pid);

/* which: 0 = keyboard, 1 = mouse (see struct sys_win_event above). Returns
   1 with *out filled if an event was waiting for this window, 0 if not. */
int compositor_poll_event(int pid, int which, struct sys_win_event *out);

/* 1 if `pid`'s window currently has keyboard focus, else 0 (including if it
   has no window at all). */
int compositor_is_focused(int pid);

#endif

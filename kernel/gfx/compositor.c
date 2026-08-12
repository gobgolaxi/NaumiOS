#include <stdint.h>
#include <stddef.h>
#include "compositor.h"
#include "../drivers/framebuffer.h"
#include "../drivers/font.h"
#include "../drivers/input.h"
#include "../mm/heap.h"
#include "../sched/sched.h"

#define MAX_WINDOWS 4
#define TITLE_MAX 32
#define EVQ_SIZE 16

#define BORDER 3
#define TITLE_H 20
#define BTN_SIZE 14
#define BTN_MARGIN 3

/* Every window's private buffer is allocated at full screen size regardless
   of its declared client size — wasteful per-byte, but with MAX_WINDOWS
   this small it's a few MB at most, and it means maximize/restore never
   needs to realloc a buffer while a client might be mid-draw into it: only
   the logical client_w/client_h clip rectangle changes. buf's pitch is
   always fb_width(), never client_w. */
typedef struct {
    struct sys_win_event q[EVQ_SIZE];
    int head, tail, count;
} evq_t;

typedef struct {
    int used;
    int pid;
    uint32_t x, y;             /* outer top-left, screen coords */
    uint32_t w, h;              /* outer size (including chrome) */
    uint32_t client_w, client_h;
    uint8_t *buf;
    char title[TITLE_MAX];
    uint32_t flags;
    int maximized;
    uint32_t restore_x, restore_y, restore_w, restore_h;
    evq_t kbd_q;
    evq_t mouse_q;
} window_t;

static window_t windows[MAX_WINDOWS];
static int zorder[MAX_WINDOWS]; /* indices into windows[]; [0]=bottom .. [zcount-1]=top */
static int zcount;
static int focused_idx = -1;
static int cascade;

static int32_t cursor_x, cursor_y;
static int32_t abs_x, abs_y; /* raw QEMU tablet range, 0..32767 on each axis */
static int have_abs;
static int drag_idx = -1;
static int32_t drag_off_x, drag_off_y;
static int dirty = 1;

/* ---- small local helpers (no libc string functions in this freestanding
   build) ---- */

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    for (; src && src[i] != '\0' && i < max - 1; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void push_event(evq_t *q, uint16_t type, uint16_t code, int32_t value, int32_t x, int32_t y) {
    if (q->count >= EVQ_SIZE) {
        return; /* drop oldest-would-be-overwritten; simplest backpressure */
    }
    q->q[q->tail] = (struct sys_win_event){ type, code, value, x, y };
    q->tail = (q->tail + 1) % EVQ_SIZE;
    q->count++;
}

static int pop_event(evq_t *q, struct sys_win_event *out) {
    if (q->count == 0) {
        return 0;
    }
    *out = q->q[q->head];
    q->head = (q->head + 1) % EVQ_SIZE;
    q->count--;
    return 1;
}

static int find_by_pid(int pid) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

/* ---- z-order ---- */

static void zorder_insert(int wi, int at_bottom) {
    if (at_bottom) {
        for (int i = zcount; i > 0; i--) {
            zorder[i] = zorder[i - 1];
        }
        zorder[0] = wi;
    } else {
        zorder[zcount] = wi;
    }
    zcount++;
}

static void zorder_remove(int wi) {
    int pos = -1;
    for (int i = 0; i < zcount; i++) {
        if (zorder[i] == wi) {
            pos = i;
            break;
        }
    }
    if (pos < 0) {
        return;
    }
    for (int i = pos; i < zcount - 1; i++) {
        zorder[i] = zorder[i + 1];
    }
    zcount--;
}

static void raise_and_focus(int wi) {
    focused_idx = wi;
    if (windows[wi].flags & WIN_FLAG_BORDERLESS) {
        return; /* desktop stays pinned to the bottom */
    }
    zorder_remove(wi);
    zorder_insert(wi, 0 /* append handled below via at_bottom=0 path */);
}

/* ---- window lifecycle ---- */

void compositor_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].used = 0;
    }
    zcount = 0;
    focused_idx = -1;
    cascade = 0;
    cursor_x = (int32_t)(fb_width() / 2);
    cursor_y = (int32_t)(fb_height() / 2);
    dirty = 1;
}

int compositor_create_window(int pid, uint32_t w, uint32_t h, const char *title, uint32_t flags) {
    if (find_by_pid(pid) >= 0) {
        return -1;
    }
    int wi = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used) {
            wi = i;
            break;
        }
    }
    if (wi < 0) {
        return -1;
    }

    window_t *win = &windows[wi];
    win->used = 1;
    win->pid = pid;
    win->flags = flags;
    win->maximized = 0;
    str_copy(win->title, title, TITLE_MAX);
    win->kbd_q.head = win->kbd_q.tail = win->kbd_q.count = 0;
    win->mouse_q.head = win->mouse_q.tail = win->mouse_q.count = 0;

    if (flags & WIN_FLAG_BORDERLESS) {
        win->x = 0;
        win->y = 0;
        win->w = (uint32_t)fb_width();
        win->h = (uint32_t)fb_height();
        win->client_w = win->w;
        win->client_h = win->h;
    } else {
        if (w == 0) w = 400;
        if (h == 0) h = 300;
        win->client_w = w;
        win->client_h = h;
        win->w = w + 2 * BORDER;
        win->h = h + 2 * BORDER + TITLE_H;
        win->x = 60 + 30 * (uint32_t)(cascade % 6);
        win->y = 50 + 30 * (uint32_t)(cascade % 6);
        cascade++;
        if (win->x + win->w > fb_width()) win->x = 20;
        if (win->y + win->h > fb_height()) win->y = 20;
    }

    win->buf = (uint8_t *)kmalloc(fb_width() * fb_height() * 4);
    if (!win->buf) {
        win->used = 0;
        return -1;
    }
    fb_buf_fill_rect(win->buf, (uint32_t)fb_width(), (uint32_t)fb_height(),
                      0, 0, (uint32_t)fb_width(), (uint32_t)fb_height(), fb_rgb(192, 192, 192));

    zorder_insert(wi, (flags & WIN_FLAG_BORDERLESS) ? 1 : 0);
    focused_idx = wi;
    dirty = 1;
    return 0;
}

static void free_window(int wi) {
    window_t *win = &windows[wi];
    zorder_remove(wi);
    if (win->buf) {
        kfree(win->buf);
        win->buf = NULL;
    }
    win->used = 0;
    if (focused_idx == wi) {
        focused_idx = (zcount > 0) ? zorder[zcount - 1] : -1;
    }
    if (drag_idx == wi) {
        drag_idx = -1;
    }
    dirty = 1;
}

static void gc_dead_windows(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && !sched_task_alive(windows[i].pid)) {
            free_window(i);
        }
    }
}

/* ---- drawing-facing API (called from syscall.c, implicitly per-pid) ---- */

int compositor_get_client_size(int pid, uint32_t *w, uint32_t *h) {
    int wi = find_by_pid(pid);
    if (wi < 0) {
        return -1;
    }
    *w = windows[wi].client_w;
    *h = windows[wi].client_h;
    return 0;
}

void compositor_fill_rect(int pid, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    int wi = find_by_pid(pid);
    if (wi < 0) {
        return;
    }
    window_t *win = &windows[wi];
    if (x >= win->client_w || y >= win->client_h) {
        return;
    }
    uint32_t cw = (x + w > win->client_w) ? win->client_w - x : w;
    uint32_t ch = (y + h > win->client_h) ? win->client_h - y : h;
    fb_buf_fill_rect(win->buf, (uint32_t)fb_width(), (uint32_t)fb_height(), x, y, cw, ch, color);
}

void compositor_draw_text(int pid, uint32_t x, uint32_t y, const char *text,
                           uint32_t fg, uint32_t bg, uint32_t scale) {
    int wi = find_by_pid(pid);
    if (wi < 0) {
        return;
    }
    window_t *win = &windows[wi];
    font_draw_text_buf(win->buf, (uint32_t)fb_width(), (uint32_t)fb_height(), x, y, text, fg, bg, scale);
}

void compositor_blit(int pid, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels) {
    int wi = find_by_pid(pid);
    if (wi < 0 || w == 0 || h == 0) {
        return;
    }
    window_t *win = &windows[wi];
    if (x >= win->client_w || y >= win->client_h) {
        return;
    }
    uint32_t cw = (x + w > win->client_w) ? win->client_w - x : w;
    uint32_t ch = (y + h > win->client_h) ? win->client_h - y : h;
    /* src_pitch is `w` (the caller's own buffer stride, tightly packed),
       not fb_width() — only the destination is fb_width()-strided. */
    fb_buf_blit(win->buf, (uint32_t)fb_width(), (uint32_t)fb_height(), x, y,
                (const uint8_t *)pixels, w, cw, ch);
}

void compositor_present(int pid) {
    (void)pid;
    dirty = 1;
}

int compositor_poll_event(int pid, int which, struct sys_win_event *out) {
    int wi = find_by_pid(pid);
    if (wi < 0) {
        return 0;
    }
    return pop_event(which == 0 ? &windows[wi].kbd_q : &windows[wi].mouse_q, out);
}

int compositor_is_focused(int pid) {
    int wi = find_by_pid(pid);
    return wi >= 0 && wi == focused_idx;
}

/* ---- chrome geometry/hit-testing ---- */

static int hit_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh, int32_t px, int32_t py) {
    return px >= (int32_t)rx && px < (int32_t)(rx + rw) && py >= (int32_t)ry && py < (int32_t)(ry + rh);
}

static uint32_t close_btn_x(const window_t *win) {
    return win->x + win->w - BORDER - BTN_MARGIN - BTN_SIZE;
}

static uint32_t max_btn_x(const window_t *win) {
    return close_btn_x(win) - BTN_MARGIN - BTN_SIZE;
}

static uint32_t title_bar_y(const window_t *win) {
    return win->y + BORDER;
}

static void resize_client(window_t *win, uint32_t new_client_w, uint32_t new_client_h) {
    win->client_w = new_client_w;
    win->client_h = new_client_h;
    push_event(&win->kbd_q, SYS_EV_RESIZE, 0, 0, (int32_t)new_client_w, (int32_t)new_client_h);
}

static void toggle_maximize(int wi) {
    window_t *win = &windows[wi];
    if (win->maximized) {
        win->x = win->restore_x;
        win->y = win->restore_y;
        win->w = win->restore_w;
        win->h = win->restore_h;
        win->maximized = 0;
    } else {
        win->restore_x = win->x;
        win->restore_y = win->y;
        win->restore_w = win->w;
        win->restore_h = win->h;
        win->x = 0;
        win->y = 0;
        win->w = (uint32_t)fb_width();
        win->h = (uint32_t)fb_height();
        win->maximized = 1;
    }
    resize_client(win, win->w - 2 * BORDER, win->h - 2 * BORDER - TITLE_H);
    dirty = 1;
}

static void close_window(int wi) {
    sched_kill(windows[wi].pid);
    free_window(wi);
}

static void handle_mouse_press(void) {
    for (int zi = zcount - 1; zi >= 0; zi--) {
        int wi = zorder[zi];
        window_t *win = &windows[wi];

        if (win->flags & WIN_FLAG_BORDERLESS) {
            if (hit_rect(win->x, win->y, win->client_w, win->client_h, cursor_x, cursor_y)) {
                raise_and_focus(wi);
                push_event(&win->mouse_q, 1 /* EV_KEY */, 0x110 /* BTN_LEFT */, 1,
                           cursor_x - (int32_t)win->x, cursor_y - (int32_t)win->y);
                dirty = 1;
                return;
            }
            continue;
        }

        if (!hit_rect(win->x, win->y, win->w, win->h, cursor_x, cursor_y)) {
            continue;
        }

        if (hit_rect(close_btn_x(win), title_bar_y(win), BTN_SIZE, BTN_SIZE, cursor_x, cursor_y)) {
            close_window(wi);
            return;
        }
        if (hit_rect(max_btn_x(win), title_bar_y(win), BTN_SIZE, BTN_SIZE, cursor_x, cursor_y)) {
            raise_and_focus(wi);
            toggle_maximize(wi);
            return;
        }
        if (hit_rect(win->x + BORDER, title_bar_y(win), win->w - 2 * BORDER, TITLE_H, cursor_x, cursor_y)) {
            raise_and_focus(wi);
            drag_idx = wi;
            drag_off_x = cursor_x - (int32_t)win->x;
            drag_off_y = cursor_y - (int32_t)win->y;
            dirty = 1;
            return;
        }

        uint32_t cx = win->x + BORDER;
        uint32_t cy = win->y + BORDER + TITLE_H;
        raise_and_focus(wi);
        if (hit_rect(cx, cy, win->client_w, win->client_h, cursor_x, cursor_y)) {
            push_event(&win->mouse_q, 1, 0x110, 1, cursor_x - (int32_t)cx, cursor_y - (int32_t)cy);
        }
        dirty = 1;
        return;
    }
}

/* ---- Win95-style chrome drawing (onto the screen buffer) ---- */

static void draw_bevel(uint8_t *buf, uint32_t bw, uint32_t bh,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h, int raised) {
    uint32_t light = fb_rgb(255, 255, 255);
    uint32_t dark = fb_rgb(64, 64, 64);
    uint32_t tl = raised ? light : dark;
    uint32_t br = raised ? dark : light;
    fb_buf_fill_rect(buf, bw, bh, x, y, w, 1, tl);
    fb_buf_fill_rect(buf, bw, bh, x, y, 1, h, tl);
    fb_buf_fill_rect(buf, bw, bh, x, y + h - 1, w, 1, br);
    fb_buf_fill_rect(buf, bw, bh, x + w - 1, y, 1, h, br);
}

static void draw_button_glyph_close(uint8_t *buf, uint32_t bw, uint32_t bh, uint32_t x, uint32_t y) {
    uint32_t black = fb_rgb(0, 0, 0);
    for (uint32_t i = 0; i < 8; i++) {
        fb_buf_put_pixel(buf, bw, bh, x + i, y + i, black);
        fb_buf_put_pixel(buf, bw, bh, x + i + 1, y + i, black);
        fb_buf_put_pixel(buf, bw, bh, x + 7 - i, y + i, black);
        fb_buf_put_pixel(buf, bw, bh, x + 8 - i, y + i, black);
    }
}

static void draw_button_glyph_maximize(uint8_t *buf, uint32_t bw, uint32_t bh, uint32_t x, uint32_t y) {
    uint32_t black = fb_rgb(0, 0, 0);
    fb_buf_fill_rect(buf, bw, bh, x, y, 9, 2, black);
    fb_buf_fill_rect(buf, bw, bh, x, y, 2, 8, black);
    fb_buf_fill_rect(buf, bw, bh, x + 7, y, 2, 8, black);
    fb_buf_fill_rect(buf, bw, bh, x, y + 6, 9, 2, black);
}

static void draw_titlebar_button(uint8_t *buf, uint32_t bw, uint32_t bh, uint32_t x, uint32_t y, int is_close) {
    fb_buf_fill_rect(buf, bw, bh, x, y, BTN_SIZE, BTN_SIZE, fb_rgb(192, 192, 192));
    draw_bevel(buf, bw, bh, x, y, BTN_SIZE, BTN_SIZE, 1);
    if (is_close) {
        draw_button_glyph_close(buf, bw, bh, x + 3, y + 3);
    } else {
        draw_button_glyph_maximize(buf, bw, bh, x + 3, y + 3);
    }
}

/* Classic Win95 active-titlebar look: a left-to-right gradient rather than
   a flat fill. One 1px-wide fill_rect per column — titlebars are a few
   hundred pixels wide at most, cheap enough not to matter next to
   everything else a composite pass already does. */
static void fill_gradient_h(uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t c0, uint32_t c1) {
    if (w == 0) {
        return;
    }
    int r0 = (int)(c0 >> 16) & 0xFF, g0 = (int)(c0 >> 8) & 0xFF, b0 = (int)c0 & 0xFF;
    int r1 = (int)(c1 >> 16) & 0xFF, g1 = (int)(c1 >> 8) & 0xFF, b1 = (int)c1 & 0xFF;
    uint32_t span = (w > 1) ? (w - 1) : 1;
    for (uint32_t i = 0; i < w; i++) {
        int r = r0 + (r1 - r0) * (int)i / (int)span;
        int g = g0 + (g1 - g0) * (int)i / (int)span;
        int b = b0 + (b1 - b0) * (int)i / (int)span;
        fb_buf_fill_rect(buf, buf_w, buf_h, x + i, y, 1, h, fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
    }
}

static void draw_chrome(const window_t *win, int focused) {
    uint8_t *scr = fb_screen_buffer();
    uint32_t sw = (uint32_t)fb_width();
    uint32_t sh = (uint32_t)fb_height();

    fb_buf_fill_rect(scr, sw, sh, win->x, win->y, win->w, win->h, fb_rgb(192, 192, 192));
    draw_bevel(scr, sw, sh, win->x, win->y, win->w, win->h, 1);

    uint32_t tb_x = win->x + BORDER;
    uint32_t tb_y = win->y + BORDER;
    uint32_t tb_w = win->w - 2 * BORDER;
    uint32_t title_start = focused ? fb_rgb(0, 0, 128) : fb_rgb(96, 96, 96);
    uint32_t title_end = focused ? fb_rgb(16, 132, 208) : fb_rgb(160, 160, 160);
    fill_gradient_h(scr, sw, sh, tb_x, tb_y, tb_w, TITLE_H, title_start, title_end);
    /* Text background is flat (title_start) rather than following the
       gradient under it — font_draw_text_buf() paints one opaque color
       behind each glyph cell, and the title sits at the gradient's left
       edge (closest to title_start) so the seam is minor. True
       gradient-matched text would need the font renderer to blend against
       a per-pixel background instead of a single flat one. */
    font_draw_text_buf(scr, sw, sh, tb_x + 4, tb_y + 6, win->title, fb_rgb(255, 255, 255), title_start, 1);

    draw_titlebar_button(scr, sw, sh, max_btn_x(win), tb_y + 3, 0);
    draw_titlebar_button(scr, sw, sh, close_btn_x(win), tb_y + 3, 1);

    uint32_t cx = win->x + BORDER;
    uint32_t cy = win->y + BORDER + TITLE_H;
    if (win->client_w >= 2 && win->client_h >= 2) {
        draw_bevel(scr, sw, sh, cx - 1, cy - 1, win->client_w + 2, win->client_h + 2, 0);
    }
}

static void draw_cursor(void) {
    /* Classic arrow silhouette, 12 rows, hand-authored. Drawn as a black
       shape offset by (1,1) first (cheap outline trick against light
       backgrounds) then white on top, so it stays visible over any
       window/desktop color. */
    static const uint16_t ARROW[12] = {
        0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00,
        0xFE00, 0xFF00, 0xF800, 0xD800, 0x8C00, 0x0C00,
    };
    uint8_t *scr = fb_screen_buffer();
    uint32_t sw = (uint32_t)fb_width();
    uint32_t sh = (uint32_t)fb_height();
    uint32_t black = fb_rgb(0, 0, 0);
    uint32_t white = fb_rgb(255, 255, 255);

    for (int pass = 0; pass < 2; pass++) {
        int32_t ox = pass == 0 ? 1 : 0;
        int32_t oy = pass == 0 ? 1 : 0;
        uint32_t color = pass == 0 ? black : white;
        for (int row = 0; row < 12; row++) {
            uint16_t bits = ARROW[row];
            for (int col = 0; col < 12; col++) {
                if (bits & (0x8000 >> col)) {
                    fb_buf_put_pixel(scr, sw, sh, (uint32_t)(cursor_x + col + ox),
                                      (uint32_t)(cursor_y + row + oy), color);
                }
            }
        }
    }
}

static void composite_frame(void) {
    fb_clear(fb_rgb(0, 128, 128)); /* teal — shows through if no desktop window exists yet */

    for (int zi = 0; zi < zcount; zi++) {
        window_t *win = &windows[zorder[zi]];
        int focused = (zorder[zi] == focused_idx);

        if (win->flags & WIN_FLAG_BORDERLESS) {
            fb_buf_blit(fb_screen_buffer(), (uint32_t)fb_width(), (uint32_t)fb_height(),
                        win->x, win->y, win->buf, (uint32_t)fb_width(), win->client_w, win->client_h);
            continue;
        }

        draw_chrome(win, focused);
        fb_buf_blit(fb_screen_buffer(), (uint32_t)fb_width(), (uint32_t)fb_height(),
                    win->x + BORDER, win->y + BORDER + TITLE_H,
                    win->buf, (uint32_t)fb_width(), win->client_w, win->client_h);
    }

    draw_cursor();
    fb_present();
}

/* ---- main loop ---- */

void compositor_task(void) {
    for (;;) {
        virtio_input_event_t ev;

        while (input_poll_mouse(&ev)) {
            if (ev.type == 3 /* EV_ABS */ && ev.code == 0 /* ABS_X */) {
                abs_x = (int32_t)ev.value;
                have_abs = 1;
            } else if (ev.type == 3 && ev.code == 1 /* ABS_Y */) {
                abs_y = (int32_t)ev.value;
                have_abs = 1;
            } else if (ev.type == 0 /* EV_SYN */) {
                if (have_abs) {
                    /* QEMU's virtio-tablet reports each axis in a fixed
                       0..32767 logical range regardless of the actual
                       display size, so this scales 1:1 to fb dimensions no
                       matter how the host window is stretched (fullscreen
                       on a bigger monitor, a scaled GTK window, etc) —
                       unlike a relative mouse, whose deltas are tied to
                       physical mouse movement and drift out of sync with a
                       scaled display (see kernel/drivers/input.c). */
                    cursor_x = (abs_x * ((int32_t)fb_width() - 1)) / 32767;
                    cursor_y = (abs_y * ((int32_t)fb_height() - 1)) / 32767;
                    have_abs = 0;
                    if (cursor_x < 0) cursor_x = 0;
                    if (cursor_y < 0) cursor_y = 0;
                    if (cursor_x > (int32_t)fb_width() - 1) cursor_x = (int32_t)fb_width() - 1;
                    if (cursor_y > (int32_t)fb_height() - 1) cursor_y = (int32_t)fb_height() - 1;

                    if (drag_idx >= 0) {
                        window_t *win = &windows[drag_idx];
                        int32_t nx = cursor_x - drag_off_x;
                        int32_t ny = cursor_y - drag_off_y;
                        if (nx < 0) nx = 0;
                        if (ny < 0) ny = 0;
                        if (nx > (int32_t)fb_width() - (int32_t)win->w) nx = (int32_t)fb_width() - (int32_t)win->w;
                        if (ny > (int32_t)fb_height() - (int32_t)win->h) ny = (int32_t)fb_height() - (int32_t)win->h;
                        if (nx < 0) nx = 0;
                        if (ny < 0) ny = 0;
                        win->x = (uint32_t)nx;
                        win->y = (uint32_t)ny;
                    }
                    dirty = 1;
                }
            } else if (ev.type == 1 /* EV_KEY */ && ev.code == 0x110 /* BTN_LEFT */) {
                if (ev.value == 1) {
                    handle_mouse_press();
                } else {
                    drag_idx = -1;
                }
            }
        }

        while (input_poll_keyboard(&ev)) {
            if (focused_idx >= 0) {
                push_event(&windows[focused_idx].kbd_q, ev.type, ev.code, (int32_t)ev.value, 0, 0);
            }
        }

        gc_dead_windows();

        if (dirty) {
            composite_frame();
            dirty = 0;
        }

        /* A real block (sched_sleep_ticks(), kernel tasks can call it
           directly — no syscall needed) instead of a busy-spin: this task
           previously stayed TASK_STATE_READY the whole time, so
           pick_next()'s round-robin gave it exactly one turn per full
           cycle through every other ready task — with several of those
           being pure idle-spin loops themselves (see userland/wm,
           userland/console, userland/doom's DG_SleepMs), the compositor
           was sharing turns with tasks doing nothing, capping real-world
           composite/present frequency far below what "1 tick of work every
           ~10ms" should allow. One tick here caps polling/compositing at
           the timer's own 100 Hz — already imperceptible latency for
           input — while actually giving up the CPU between iterations. */
        sched_sleep_ticks(1);
    }
}

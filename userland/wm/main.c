#include <stddef.h>
#include "../lib/naumi.h"

/* The desktop — and nothing more. Under the old model wm.c *was* the
   window manager: it drew every window's chrome by hand, tracked drag
   state, hit-tested close buttons, and drew its own cursor every frame.
   None of that is wm's job anymore — the kernel compositor
   (kernel/gfx/compositor.c) owns the window table, draws chrome, handles
   dragging/closing/maximizing, and draws the cursor. wm.c is now just an
   ordinary client of that system: it creates one WIN_BORDERLESS window
   sized to the whole screen (which is how "the desktop" is represented —
   not a special kernel concept, just a window with no title bar that's
   pinned to the bottom of z-order), paints a background and some icons
   into it once, and reacts to clicks on those icons by spawning a
   program. Everything else — what happens once that program creates its
   own window, whether it's on top, whether the user drags it around — is
   the compositor's problem, uniformly, for every windowed app. */

#define ICON_SIZE 48

struct icon {
    unsigned int x, y;
    const char *label;
    const char *path;
};

static const struct icon icons[3] = {
    { 700, 400, "Files", "filemgr.elf" },
    { 700, 460, "Editor", "edit.elf" },
    { 700, 520, "Console", "console.elf" },
};

static int hit_icon(const struct icon *ic, long px, long py) {
    return px >= (long)ic->x && px < (long)(ic->x + ICON_SIZE) &&
           py >= (long)ic->y && py < (long)(ic->y + ICON_SIZE);
}

static void draw_icon(const struct icon *ic) {
    sys_fb_fill(ic->x, ic->y, ICON_SIZE, ICON_SIZE, rgb(0, 0, 128));
    sys_fb_fill(ic->x + 4, ic->y + 4, ICON_SIZE - 8, ICON_SIZE - 8, rgb(230, 230, 245));
    sys_fb_text(ic->x, ic->y + ICON_SIZE + 4, ic->label, rgb(255, 255, 255), rgb(0, 128, 128), 1);
}

void _start(void) {
    sys_win_create(0, 0, WIN_BORDERLESS, "Desktop");

    struct fb_info fb;
    sys_fb_info(&fb);

    /* Classic Windows 95 teal, tiled subtly isn't worth the pixel budget
       here — a flat fill reads the same at this resolution. */
    sys_fb_fill(0, 0, (unsigned int)fb.width, (unsigned int)fb.height, rgb(0, 128, 128));
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        draw_icon(&icons[i]);
    }
    sys_fb_present();

    for (;;) {
        struct input_event_wire ev;

        while (sys_poll_input(1, &ev) > 0) { /* mouse clicks landing on the desktop */
            if (ev.type == EV_KEY && ev.code == BTN_LEFT && ev.value == 1) {
                for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
                    if (hit_icon(&icons[i], ev.x, ev.y)) {
                        sys_spawn(icons[i].path);
                        break;
                    }
                }
            }
        }

        while (sys_poll_input(0, &ev) > 0) { /* keyboard */
            if (ev.type == EV_KEY && ev.code == KEY_ESC && ev.value == 1) {
                sys_exit();
            }
        }

        sys_sleep_ms(10); /* real block, not a busy-wait — see naumi.h */
    }
}

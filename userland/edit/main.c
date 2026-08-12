#include "../lib/naumi.h"
#include "../lib/libc.h"
#include "../lib/keymap.h"
#include <stdio.h>

/* A minimal text editor: one flat char buffer with an embedded cursor
   offset (not a line array) — splitting into lines is done purely at
   render time by scanning for '\n', which keeps insert/delete a single
   memmove instead of juggling a line table. No horizontal scrolling
   (long lines just clip at the window edge) and no undo — a plain single-
   buffer editor, not a from-scratch Vim.

   Opens whatever path userland/filemgr/main.c left in ARGFILE.TXT (see
   its open_in_editor() — there's no argv/IPC mechanism to hand a spawned
   process a path otherwise), or starts blank ("Untitled") if launched
   directly (e.g. from the console) with nothing waiting there. */

#define MAX_TEXT (64 * 1024)
#define MAX_PATH 64
#define ROW_H 22
#define TEXT_SCALE 2
#define HANDOFF_PATH "ARGFILE.TXT"

static char text[MAX_TEXT];
static long text_len;
static long cursor;
static char filepath[MAX_PATH];
static int has_path;
static int dirty_flag;

static unsigned int win_w, win_h;
static int scroll_top_line;

static long line_start(long pos) {
    while (pos > 0 && text[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

static long line_end(long pos) {
    while (pos < text_len && text[pos] != '\n') {
        pos++;
    }
    return pos;
}

static int cursor_line_and_col(long *out_col) {
    int line = 0;
    long col = 0;
    for (long i = 0; i < cursor; i++) {
        if (text[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    *out_col = col;
    return line;
}

static void insert_char(char c) {
    if (text_len >= MAX_TEXT - 1) {
        return;
    }
    memmove(text + cursor + 1, text + cursor, (size_t)(text_len - cursor));
    text[cursor] = c;
    text_len++;
    cursor++;
    dirty_flag = 1;
}

static void delete_before_cursor(void) {
    if (cursor == 0) {
        return;
    }
    memmove(text + cursor - 1, text + cursor, (size_t)(text_len - cursor));
    text_len--;
    cursor--;
    dirty_flag = 1;
}

static void move_up(void) {
    long col;
    cursor_line_and_col(&col);
    long ls = line_start(cursor);
    if (ls == 0) {
        return;
    }
    long prev_end = ls - 1;
    long prev_start = line_start(prev_end);
    long prev_len = prev_end - prev_start;
    cursor = prev_start + (col < prev_len ? col : prev_len);
}

static void move_down(void) {
    long col;
    cursor_line_and_col(&col);
    long le = line_end(cursor);
    if (le >= text_len) {
        return;
    }
    long next_start = le + 1;
    long next_end = line_end(next_start);
    long next_len = next_end - next_start;
    cursor = next_start + (col < next_len ? col : next_len);
}

static void load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        text_len = 0;
        cursor = 0;
        return;
    }
    text_len = (long)fread(text, 1, MAX_TEXT - 1, f);
    fclose(f);
    cursor = 0;
    dirty_flag = 0;
}

static void save_file(void) {
    if (!has_path) {
        return;
    }
    FILE *f = fopen(filepath, "w");
    if (!f) {
        return;
    }
    fwrite(text, 1, (size_t)text_len, f);
    fclose(f);
    dirty_flag = 0;
}

static void redraw(void) {
    sys_fb_fill(0, 0, win_w, win_h, rgb(24, 24, 24));

    char header[96];
    snprintf(header, sizeof(header), "%s%s", has_path ? filepath : "Untitled",
              dirty_flag ? " *" : "");
    sys_fb_text(4, 2, header, rgb(180, 220, 255), rgb(24, 24, 24), TEXT_SCALE);

    long cursor_col;
    int cursor_line = cursor_line_and_col(&cursor_col);

    int visible_rows = (int)((win_h - ROW_H) / ROW_H);
    if (cursor_line < scroll_top_line) {
        scroll_top_line = cursor_line;
    }
    if (cursor_line >= scroll_top_line + visible_rows) {
        scroll_top_line = cursor_line - visible_rows + 1;
    }

    /* Walk to the start of scroll_top_line, then render forward. */
    long pos = 0;
    for (int l = 0; l < scroll_top_line && pos < text_len; l++) {
        while (pos < text_len && text[pos] != '\n') pos++;
        if (pos < text_len) pos++;
    }

    for (int row = 0; row < visible_rows && pos <= text_len; row++) {
        long start = pos;
        long end = start;
        while (end < text_len && text[end] != '\n') end++;
        long len = end - start;
        if (len > 118) len = 118;

        char linebuf[120];
        memcpy(linebuf, text + start, (size_t)len);
        linebuf[len] = '\0';

        unsigned int y = ROW_H + (unsigned int)row * ROW_H;
        sys_fb_text(4, y, linebuf, rgb(225, 225, 225), rgb(24, 24, 24), TEXT_SCALE);

        if (scroll_top_line + row == cursor_line) {
            /* Cheap block cursor: a thin filled rect at an assumed flat
               per-column pixel offset. kernel/gfx/ttf.c renders
               proportional glyphs (real widths vary per character) and
               there's no syscall exposing font_glyph_width() to userland
               to measure the actual prefix — so this drifts slightly
               out of alignment on lines with a lot of narrow/wide
               characters mixed together. Good enough to see roughly
               where the cursor is; not pixel-exact. */
            unsigned int glyph_w = 8u * TEXT_SCALE;
            unsigned int cx = 4 + (unsigned int)cursor_col * glyph_w;
            sys_fb_fill(cx, y, 2, ROW_H - 2, rgb(255, 220, 80));
        }

        if (end >= text_len) {
            break;
        }
        pos = end + 1;
    }

    sys_fb_present();
}

void _start(void) {
    sys_win_create(640, 480, 0, "Editor");

    struct fb_info fb;
    sys_fb_info(&fb);
    win_w = (unsigned int)fb.width;
    win_h = (unsigned int)fb.height;

    has_path = 0;
    filepath[0] = '\0';
    text_len = 0;
    cursor = 0;

    FILE *arg = fopen(HANDOFF_PATH, "r");
    if (arg) {
        char pathbuf[MAX_PATH];
        size_t got = fread(pathbuf, 1, sizeof(pathbuf) - 1, arg);
        fclose(arg);
        pathbuf[got] = '\0';
        if (got > 0) {
            size_t n = 0;
            for (; pathbuf[n] != '\0' && n < sizeof(filepath) - 1; n++) {
                filepath[n] = pathbuf[n];
            }
            filepath[n] = '\0';
            has_path = 1;
            load_file(filepath);
        }
        /* Clear the handoff so a later bare launch of edit.elf (no
           filemgr involved) doesn't pick up a stale path. */
        FILE *clear = fopen(HANDOFF_PATH, "w");
        if (clear) {
            fclose(clear);
        }
    }

    int shift = 0;
    int ctrl = 0;

    redraw();

    for (;;) {
        struct input_event_wire ev;
        int dirty = 0;

        while (sys_poll_input(0, &ev) > 0) {
            if (ev.type == EV_RESIZE) {
                win_w = (unsigned int)ev.x;
                win_h = (unsigned int)ev.y;
                dirty = 1;
                continue;
            }
            if (ev.type != EV_KEY) {
                continue;
            }
            if (ev.code == KC_LSHIFT || ev.code == KC_RSHIFT) {
                shift = ev.value != 0;
                continue;
            }
            if (ev.code == KC_LCTRL) {
                ctrl = ev.value != 0;
                continue;
            }
            if (ev.value != 1) {
                continue;
            }

            if (ev.code == KC_ESC) {
                sys_exit();
            } else if (ctrl && ev.code == 31 /* KEY_S */) {
                save_file();
                dirty = 1;
            } else if (ev.code == KC_ENTER) {
                insert_char('\n');
                dirty = 1;
            } else if (ev.code == KC_BACKSPACE) {
                delete_before_cursor();
                dirty = 1;
            } else if (ev.code == KC_LEFT) {
                if (cursor > 0) cursor--;
                dirty = 1;
            } else if (ev.code == KC_RIGHT) {
                if (cursor < text_len) cursor++;
                dirty = 1;
            } else if (ev.code == KC_UP) {
                move_up();
                dirty = 1;
            } else if (ev.code == KC_DOWN) {
                move_down();
                dirty = 1;
            } else if (ev.code == KC_HOME) {
                cursor = line_start(cursor);
                dirty = 1;
            } else if (ev.code == KC_END) {
                cursor = line_end(cursor);
                dirty = 1;
            } else {
                char c = keymap_translate(ev.code, shift);
                if (c != 0) {
                    insert_char(c);
                    dirty = 1;
                }
            }
        }

        while (sys_poll_input(1, &ev) > 0) { }

        if (dirty) {
            redraw();
        }

        sys_sleep_ms(10);
    }
}

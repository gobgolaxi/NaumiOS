#include "../lib/naumi.h"
#include "../lib/libc.h"
#include "../lib/keymap.h"
#include <stdio.h>

/* A minimal graphical file manager: lists the current directory (via
   SYS_LISTDIR — kernel/fs/fat16.c's fat16_list_dir() exposed to userland),
   Up/Down + Enter to navigate. Enter on a directory descends into it (or
   ".." to go back up); on a ".ELF" file it's sys_spawn()'d directly; on
   anything else it's handed to the text editor (userland/edit) via a tiny
   file-based handoff — see open_in_editor() below, there's no argv/IPC
   mechanism to pass a path to a freshly spawned process otherwise. */

#define MAX_PATH 64
#define MAX_ENTRIES 128
#define ROW_H 22
#define TEXT_SCALE 2

/* Both filemgr and edit.elf agree on this path purely by convention: the
   only thing written here is the one path the very next process spawned
   is expected to open. No locking needed — SYS_FILE_WRITE (via fclose())
   is synchronous and completes before sys_spawn() returns, and only one
   thing is ever mid-handoff at a time (a user launching a second file
   before the first editor reads its own ARG.TXT would race, but that's
   not a scenario this single-user desktop needs to defend against). */
#define HANDOFF_PATH "ARGFILE.TXT"

static char cur_dir[MAX_PATH] = "";
static struct dirent_wire entries[MAX_ENTRIES];
static int entry_count;
static int has_dotdot;
static int selected;
static int scroll_top;

static unsigned int win_w, win_h;

static int str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int ends_with_elf(const char *name) {
    size_t n = strlen(name);
    return n >= 4 && str_eq_ci(name + n - 4, ".ELF");
}

static void refresh(void) {
    long n = sys_listdir(cur_dir, entries, MAX_ENTRIES);
    entry_count = (int)n;
    has_dotdot = cur_dir[0] != '\0';
    selected = 0;
    scroll_top = 0;
}

/* Row index 0 is the synthetic ".." entry when not at root; real entries
   start after that. */
static int total_rows(void) {
    return entry_count + (has_dotdot ? 1 : 0);
}

static const char *row_name(int row, int *out_is_dir, unsigned int *out_size) {
    if (has_dotdot) {
        if (row == 0) {
            *out_is_dir = 1;
            *out_size = 0;
            return "..";
        }
        row--;
    }
    *out_is_dir = (int)entries[row].is_dir;
    *out_size = entries[row].size;
    return entries[row].name;
}

static void path_up(void) {
    size_t n = strlen(cur_dir);
    while (n > 0 && cur_dir[n - 1] == '/') {
        n--;
    }
    while (n > 0 && cur_dir[n - 1] != '/') {
        n--;
    }
    cur_dir[n] = '\0';
}

static void path_down(const char *name) {
    size_t n = strlen(cur_dir);
    if (n > 0 && cur_dir[n - 1] != '/') {
        cur_dir[n++] = '/';
    }
    size_t i = 0;
    while (name[i] != '\0' && n < sizeof(cur_dir) - 1) {
        cur_dir[n++] = name[i++];
    }
    cur_dir[n] = '\0';
}

static void open_in_editor(const char *name) {
    char full[MAX_PATH];
    size_t n = 0;
    if (cur_dir[0] != '\0') {
        for (; cur_dir[n] != '\0' && n < sizeof(full) - 2; n++) {
            full[n] = cur_dir[n];
        }
        full[n++] = '/';
    }
    size_t i = 0;
    while (name[i] != '\0' && n < sizeof(full) - 1) {
        full[n++] = name[i++];
    }
    full[n] = '\0';

    FILE *f = fopen(HANDOFF_PATH, "w");
    if (f) {
        fwrite(full, 1, n, f);
        fclose(f);
    }
    sys_spawn("edit.elf");
}

static void draw_size(char *buf, unsigned int size) {
    snprintf(buf, 16, "%u", size);
}

static void redraw(void) {
    sys_fb_fill(0, 0, win_w, win_h, rgb(20, 20, 30));

    char header[80];
    snprintf(header, sizeof(header), "/%s", cur_dir);
    sys_fb_text(4, 4, header, rgb(200, 200, 255), rgb(20, 20, 30), TEXT_SCALE);

    int visible_rows = (int)((win_h - ROW_H * 2) / ROW_H);
    int rows = total_rows();

    if (selected < scroll_top) {
        scroll_top = selected;
    }
    if (selected >= scroll_top + visible_rows) {
        scroll_top = selected - visible_rows + 1;
    }

    for (int i = 0; i < visible_rows && scroll_top + i < rows; i++) {
        int row = scroll_top + i;
        int is_dir;
        unsigned int size;
        const char *name = row_name(row, &is_dir, &size);

        unsigned int y = ROW_H * 2 + (unsigned int)i * ROW_H;
        unsigned int bg = (row == selected) ? rgb(60, 60, 120) : rgb(20, 20, 30);
        unsigned int fg = is_dir ? rgb(255, 220, 120) : rgb(220, 220, 220);

        sys_fb_fill(0, y, win_w, ROW_H, bg);

        char line[80];
        if (is_dir) {
            snprintf(line, sizeof(line), "[%s]", name);
        } else {
            char sizebuf[16];
            draw_size(sizebuf, size);
            snprintf(line, sizeof(line), "%s  (%s)", name, sizebuf);
        }
        sys_fb_text(8, y + 2, line, fg, bg, TEXT_SCALE);
    }

    sys_fb_present();
}

void _start(void) {
    sys_win_create(560, 480, 0, "Files");

    struct fb_info fb;
    sys_fb_info(&fb);
    win_w = (unsigned int)fb.width;
    win_h = (unsigned int)fb.height;

    refresh();
    redraw();

    int shift = 0;

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
            if (ev.value != 1) {
                continue;
            }

            if (ev.code == KC_ESC) {
                sys_exit();
            } else if (ev.code == KC_UP) {
                if (selected > 0) { selected--; dirty = 1; }
            } else if (ev.code == KC_DOWN) {
                if (selected < total_rows() - 1) { selected++; dirty = 1; }
            } else if (ev.code == KC_BACKSPACE) {
                if (has_dotdot) { path_up(); refresh(); dirty = 1; }
            } else if (ev.code == KC_ENTER) {
                if (total_rows() == 0) {
                    continue;
                }
                int is_dir;
                unsigned int size;
                const char *name = row_name(selected, &is_dir, &size);
                if (has_dotdot && selected == 0) {
                    path_up();
                    refresh();
                } else if (is_dir) {
                    path_down(name);
                    refresh();
                } else if (ends_with_elf(name)) {
                    char full[MAX_PATH];
                    size_t n = 0;
                    if (cur_dir[0] != '\0') {
                        for (; cur_dir[n] != '\0' && n < sizeof(full) - 2; n++) full[n] = cur_dir[n];
                        full[n++] = '/';
                    }
                    size_t i = 0;
                    while (name[i] != '\0' && n < sizeof(full) - 1) full[n++] = name[i++];
                    full[n] = '\0';
                    sys_spawn(full);
                } else {
                    open_in_editor(name);
                }
                dirty = 1;
            }
            (void)shift;
        }

        while (sys_poll_input(1, &ev) > 0) {
            if (ev.type == EV_KEY && ev.code == BTN_LEFT && ev.value == 1) {
                if (ev.y >= (int)(ROW_H * 2)) {
                    int row = scroll_top + (ev.y - (int)(ROW_H * 2)) / ROW_H;
                    if (row >= 0 && row < total_rows()) {
                        selected = row;
                        dirty = 1;
                    }
                }
            }
        }

        if (dirty) {
            redraw();
        }

        sys_sleep_ms(10);
    }
}

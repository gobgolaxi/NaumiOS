#include "../lib/naumi.h"
#include "../lib/libc.h"
#include "../lib/keymap.h"
#include <stdio.h>

/* A graphical text console: the userland counterpart to the UART shell,
   but driven by real keyboard events and drawn into its own window
   instead of talking over serial. Under the real windowing system it's an
   ordinary bordered window like any other app — the kernel compositor
   draws its title bar/border/buttons, handles dragging and the close
   button, and only delivers keyboard events here while this window has
   focus (see sys_win_create()/naumi.h). No more manual focus polling to
   avoid drawing over someone else's screen — there's no shared screen to
   fight over anymore, just this window's own buffer.

   Beyond the original bare-bones version: full shift-aware typing (see
   keymap.h) instead of lowercase-only, left/right/Home/End cursor editing
   within the input line instead of append-only, Up/Down command history,
   and ls/cat/mkdir/cd/pwd built straight on the same syscalls
   userland/filemgr and the kernel UART shell use — a fictitious per-
   session working directory tracked only here (the filesystem/kernel
   have no concept of "current directory"). */

#define CELL_H 20
#define TEXT_SCALE 2
#define COLS 60
#define MAX_LINES 27
#define MAX_PATH 64
#define HISTORY_CAP 16

static char lines[MAX_LINES][COLS + 1];
static int line_count;

static char input[COLS + 1];
static int input_len;
static int input_cursor;

static char history[HISTORY_CAP][COLS + 1];
static int history_count;   /* entries actually stored, <= HISTORY_CAP */
static int history_browse;  /* -1 = not browsing; else index into history, most-recent-first */
static char draft[COLS + 1]; /* input line as it was before Up was first pressed */

static char cur_dir[MAX_PATH] = "";

static unsigned int win_w, win_h;

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    for (; src[i] != '\0' && i < max - 1; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void push_line(const char *text) {
    if (line_count >= MAX_LINES) {
        for (int i = 1; i < MAX_LINES; i++) {
            str_copy(lines[i - 1], lines[i], COLS + 1);
        }
        line_count = MAX_LINES - 1;
    }
    str_copy(lines[line_count], text, COLS + 1);
    line_count++;
}

/* Splits arbitrary text on '\n' into however many push_line() calls it
   takes, truncating each resulting line to COLS — used by `cat`, where
   the file content itself decides how many lines are needed. */
static void push_text_block(const char *text, size_t len) {
    char buf[COLS + 1];
    size_t bi = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n') {
            buf[bi] = '\0';
            push_line(buf);
            bi = 0;
        } else if (bi < COLS) {
            buf[bi++] = c;
        }
    }
    if (bi > 0) {
        buf[bi] = '\0';
        push_line(buf);
    }
}

static void redraw(void) {
    sys_fb_fill(0, 0, win_w, win_h, rgb(10, 10, 10));

    for (int i = 0; i < line_count; i++) {
        sys_fb_text(4, 4 + (unsigned int)i * CELL_H, lines[i], rgb(210, 210, 210), rgb(10, 10, 10), TEXT_SCALE);
    }

    char prompt[COLS + 4];
    prompt[0] = '>';
    prompt[1] = ' ';
    str_copy(prompt + 2, input, COLS + 1);
    sys_fb_text(4, 4 + (unsigned int)line_count * CELL_H, prompt, rgb(120, 220, 120), rgb(10, 10, 10), TEXT_SCALE);

    sys_fb_present();
}

static void path_up(void) {
    size_t n = strlen(cur_dir);
    while (n > 0 && cur_dir[n - 1] == '/') n--;
    while (n > 0 && cur_dir[n - 1] != '/') n--;
    cur_dir[n] = '\0';
}

/* Absolute (leading '/') rel paths resolve from the root regardless of
   cur_dir; anything else is joined onto it. */
static void join_path(char *dst, size_t cap, const char *base, const char *rel) {
    if (rel[0] == '/') {
        str_copy(dst, rel + 1, (int)cap);
        return;
    }
    if (base[0] == '\0') {
        str_copy(dst, rel, (int)cap);
        return;
    }
    size_t n = 0;
    for (; base[n] != '\0' && n < cap - 2; n++) dst[n] = base[n];
    dst[n++] = '/';
    size_t i = 0;
    while (rel[i] != '\0' && n < cap - 1) dst[n++] = rel[i++];
    dst[n] = '\0';
}

static void cmd_ls(const char *args) {
    char path[MAX_PATH];
    join_path(path, sizeof(path), cur_dir, args);

    struct dirent_wire entries[48];
    long n = sys_listdir(path, entries, 48);
    if (n <= 0) {
        push_line("(empty)");
        return;
    }
    for (long i = 0; i < n; i++) {
        char line[COLS + 1];
        if (entries[i].is_dir) {
            snprintf(line, sizeof(line), "[%s]", entries[i].name);
        } else {
            snprintf(line, sizeof(line), "%s  %u", entries[i].name, entries[i].size);
        }
        push_line(line);
    }
}

static void cmd_cat(const char *args) {
    if (args[0] == '\0') {
        push_line("usage: cat <path>");
        return;
    }
    char path[MAX_PATH];
    join_path(path, sizeof(path), cur_dir, args);

    FILE *f = fopen(path, "r");
    if (!f) {
        push_line("cat: not found");
        return;
    }
    static char buf[4096];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    push_text_block(buf, got);
}

static void cmd_mkdir(const char *args) {
    if (args[0] == '\0') {
        push_line("usage: mkdir <path>");
        return;
    }
    char path[MAX_PATH];
    join_path(path, sizeof(path), cur_dir, args);
    push_line(sys_mkdir(path) == 0 ? "mkdir: ok" : "mkdir: failed");
}

static void cmd_cd(const char *args) {
    if (args[0] == '\0') {
        cur_dir[0] = '\0';
    } else if (strcmp(args, "..") == 0) {
        path_up();
    } else {
        join_path(cur_dir, sizeof(cur_dir), cur_dir, args);
    }
}

static void cmd_pwd(void) {
    char line[MAX_PATH + 1];
    snprintf(line, sizeof(line), "/%s", cur_dir);
    push_line(line);
}

/* Splits the input line on the first space into cmd/args, same convention
   as the UART shell. Both point into `input` (mutated to insert a NUL). */
static void run_command(char *cmdline) {
    char *cmd = cmdline;
    char *args = cmdline;
    while (*args && *args != ' ') {
        args++;
    }
    if (*args == ' ') {
        *args = '\0';
        args++;
    }

    if (cmd[0] == '\0') {
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        push_line("run <n> clear exit ls [d] cat <p> mkdir <p> cd [d|..] pwd");
    } else if (strcmp(cmd, "clear") == 0) {
        line_count = 0;
    } else if (strcmp(cmd, "exit") == 0) {
        sys_exit();
    } else if (strcmp(cmd, "run") == 0) {
        if (args[0] == '\0') {
            push_line("usage: run <name.elf>");
        } else {
            long pid = sys_spawn(args);
            push_line(pid < 0 ? "run: failed" : "run: started");
        }
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(args);
    } else if (strcmp(cmd, "cat") == 0) {
        cmd_cat(args);
    } else if (strcmp(cmd, "mkdir") == 0) {
        cmd_mkdir(args);
    } else if (strcmp(cmd, "cd") == 0) {
        cmd_cd(args);
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else {
        push_line("unknown command (try 'help')");
    }
}

static void history_push(const char *cmdline) {
    if (cmdline[0] == '\0') {
        return;
    }
    if (history_count < HISTORY_CAP) {
        for (int i = history_count; i > 0; i--) {
            str_copy(history[i], history[i - 1], COLS + 1);
        }
        history_count++;
    } else {
        for (int i = HISTORY_CAP - 1; i > 0; i--) {
            str_copy(history[i], history[i - 1], COLS + 1);
        }
    }
    str_copy(history[0], cmdline, COLS + 1);
}

static void input_insert(char c) {
    if (input_len >= COLS - 1) {
        return;
    }
    memmove(input + input_cursor + 1, input + input_cursor, (size_t)(input_len - input_cursor));
    input[input_cursor] = c;
    input_len++;
    input_cursor++;
    input[input_len] = '\0';
}

static void input_backspace(void) {
    if (input_cursor == 0) {
        return;
    }
    memmove(input + input_cursor - 1, input + input_cursor, (size_t)(input_len - input_cursor));
    input_len--;
    input_cursor--;
    input[input_len] = '\0';
}

void _start(void) {
    sys_win_create(680, 500, 0, "Console");

    struct fb_info fb;
    sys_fb_info(&fb);
    win_w = (unsigned int)fb.width;
    win_h = (unsigned int)fb.height;

    line_count = 0;
    input_len = 0;
    input_cursor = 0;
    input[0] = '\0';
    history_count = 0;
    history_browse = -1;

    push_line("NaumiOS console -- type 'help'");
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
            if (ev.value != 1) { /* presses only */
                continue;
            }

            if (ev.code == KC_ESC) {
                sys_exit();
            } else if (ev.code == KC_ENTER) {
                input[input_len] = '\0';
                char line[COLS + 3];
                line[0] = '>';
                line[1] = ' ';
                str_copy(line + 2, input, COLS + 1);
                push_line(line);
                history_push(input);
                history_browse = -1;
                run_command(input);
                input_len = 0;
                input_cursor = 0;
                input[0] = '\0';
                dirty = 1;
            } else if (ev.code == KC_BACKSPACE) {
                input_backspace();
                dirty = 1;
            } else if (ev.code == KC_LEFT) {
                if (input_cursor > 0) { input_cursor--; dirty = 1; }
            } else if (ev.code == KC_RIGHT) {
                if (input_cursor < input_len) { input_cursor++; dirty = 1; }
            } else if (ev.code == KC_HOME) {
                input_cursor = 0;
                dirty = 1;
            } else if (ev.code == KC_END) {
                input_cursor = input_len;
                dirty = 1;
            } else if (ev.code == KC_UP) {
                if (history_count > 0 && history_browse < history_count - 1) {
                    if (history_browse == -1) {
                        str_copy(draft, input, COLS + 1);
                    }
                    history_browse++;
                    str_copy(input, history[history_browse], COLS + 1);
                    input_len = (int)strlen(input);
                    input_cursor = input_len;
                    dirty = 1;
                }
            } else if (ev.code == KC_DOWN) {
                if (history_browse >= 0) {
                    history_browse--;
                    if (history_browse == -1) {
                        str_copy(input, draft, COLS + 1);
                    } else {
                        str_copy(input, history[history_browse], COLS + 1);
                    }
                    input_len = (int)strlen(input);
                    input_cursor = input_len;
                    dirty = 1;
                }
            } else {
                char c = keymap_translate(ev.code, shift);
                if (c != 0) {
                    input_insert(c);
                    dirty = 1;
                }
            }
        }

        while (sys_poll_input(1, &ev) > 0) { } /* nothing uses mouse clicks here */

        if (dirty) {
            redraw();
        }

        sys_sleep_ms(10); /* real block, not a busy-wait — see naumi.h */
    }
}

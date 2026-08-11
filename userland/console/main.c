#include "../lib/naumi.h"

/* A graphical text console: the userland counterpart to the UART shell,
   but driven by real keyboard events and drawn into its own window
   instead of talking over serial. Under the real windowing system it's an
   ordinary bordered window like any other app — the kernel compositor
   draws its title bar/border/buttons, handles dragging and the close
   button, and only delivers keyboard events here while this window has
   focus (see sys_win_create()/naumi.h). No more manual focus polling to
   avoid drawing over someone else's screen — there's no shared screen to
   fight over anymore, just this window's own buffer. */

#define CELL_W 12
#define CELL_H 20
#define TEXT_SCALE 2
#define COLS 50
#define MAX_LINES 27

static char lines[MAX_LINES][COLS + 1];
static int line_count;

static char input[COLS + 1];
static int input_len;

static unsigned int win_w, win_h;

/* Standard evdev/Linux keycodes for a US QWERTY layout — the same numbers
   confirmed earlier straight off the virtio-input wire (KEY_A = 30, etc).
   No shift/caps handling: lowercase and digits only, which is enough to
   type paths like "cat.elf". Index 0 and anything unmapped is 0 (no
   character). */
static const char KEYMAP[58] = {
    /*0*/ 0, 0 /*ESC*/, '1', '2', '3', '4', '5', '6', '7', '8',
    /*10*/ '9', '0', '-', '=', 0 /*BKSP*/, 0 /*TAB*/, 'q', 'w', 'e', 'r',
    /*20*/ 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0 /*ENTER*/, 0 /*CTRL*/,
    /*30*/ 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /*40*/ '\'', '`', 0 /*SHIFT*/, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    /*50*/ 'm', ',', '.', '/', 0 /*SHIFT*/, 0, 0 /*ALT*/, ' ',
};

#define KEY_BACKSPACE 14
#define KEY_ENTER 28

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

static void redraw(void) {
    sys_fb_fill(0, 0, win_w, win_h, rgb(10, 10, 10));

    for (int i = 0; i < line_count; i++) {
        sys_fb_text(4, 4 + (unsigned int)i * CELL_H, lines[i], rgb(210, 210, 210), rgb(10, 10, 10), TEXT_SCALE);
    }

    char prompt[COLS + 3];
    prompt[0] = '>';
    prompt[1] = ' ';
    str_copy(prompt + 2, input, COLS + 1);
    sys_fb_text(4, 4 + (unsigned int)line_count * CELL_H, prompt, rgb(120, 220, 120), rgb(10, 10, 10), TEXT_SCALE);

    sys_fb_present();
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

    int match_run = cmd[0] == 'r' && cmd[1] == 'u' && cmd[2] == 'n' && cmd[3] == '\0';
    int match_clear = cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r' && cmd[5] == '\0';
    int match_exit = cmd[0] == 'e' && cmd[1] == 'x' && cmd[2] == 'i' && cmd[3] == 't' && cmd[4] == '\0';
    int match_help = cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p' && cmd[4] == '\0';

    if (match_help) {
        push_line("commands: run <name.elf>, clear, exit");
    } else if (match_clear) {
        line_count = 0;
    } else if (match_exit) {
        sys_exit();
    } else if (match_run) {
        if (args[0] == '\0') {
            push_line("usage: run <name.elf>");
        } else {
            long pid = sys_spawn(args);
            if (pid < 0) {
                push_line("run: failed");
            } else {
                push_line("run: started");
            }
        }
    } else {
        push_line("unknown command (try 'help')");
    }
}

void _start(void) {
    sys_win_create(620, 460, 0, "Console");

    struct fb_info fb;
    sys_fb_info(&fb);
    win_w = (unsigned int)fb.width;
    win_h = (unsigned int)fb.height;

    line_count = 0;
    input_len = 0;
    input[0] = '\0';

    push_line("NaumiOS console -- type 'help'");
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
            if (ev.type != EV_KEY || ev.value != 1) { /* presses only */
                continue;
            }
            if (ev.code == KEY_ESC) {
                sys_exit();
            } else if (ev.code == KEY_ENTER) {
                input[input_len] = '\0';
                char line[COLS + 3];
                line[0] = '>';
                line[1] = ' ';
                str_copy(line + 2, input, COLS + 1);
                push_line(line);
                run_command(input);
                input_len = 0;
                input[0] = '\0';
                dirty = 1;
            } else if (ev.code == KEY_BACKSPACE) {
                if (input_len > 0) {
                    input_len--;
                    input[input_len] = '\0';
                    dirty = 1;
                }
            } else if (ev.code < sizeof(KEYMAP) && KEYMAP[ev.code] != 0) {
                if (input_len < COLS - 1) {
                    input[input_len++] = KEYMAP[ev.code];
                    input[input_len] = '\0';
                    dirty = 1;
                }
            }
        }

        while (sys_poll_input(1, &ev) > 0) { } /* nothing uses mouse clicks here */

        if (dirty) {
            redraw();
        }

        for (volatile int i = 0; i < 50000; i++) { }
    }
}

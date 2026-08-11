#include <stdint.h>
#include <stddef.h>
#include "shell.h"
#include "../drivers/uart.h"
#include "../drivers/virtio_blk.h"
#include "../drivers/input.h"
#include "../fs/fat16.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../arch/aarch64/timer.h"
#include "../sched/sched.h"
#include "../loader/spawn.h"

#define LINE_MAX 64

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void print_prompt(void) {
    uart_puts("naumios> ");
}

static void ps_visit(int pid, const char *name, task_state_t state, int is_current) {
    const char *state_str = "ready  ";
    if (state == TASK_STATE_BLOCKED) {
        state_str = "blocked";
    } else if (state == TASK_STATE_ZOMBIE) {
        state_str = "zombie ";
    }

    uart_puts(is_current ? "* " : "  ");
    uart_puts("pid "); uart_puthex((uint64_t)pid);
    uart_puts("  "); uart_puts(state_str);
    uart_puts("  "); uart_puts(name); uart_puts("\n");
}

/* Exercises kmalloc/kfree: allocate three differently-sized blocks, fill
   each with a distinct byte pattern, confirm none of them stomped on the
   others, free the middle one and confirm the freed space gets reused, then
   clean up. Prints OK/FAILED rather than trusting silence. */
static void run_heaptest(void) {
    uint8_t *a = kmalloc(64);
    uint8_t *b = kmalloc(128);
    uint8_t *c = kmalloc(32);
    if (!a || !b || !c) {
        uart_puts("heap test: kmalloc returned NULL\n");
        return;
    }

    for (size_t i = 0; i < 64; i++) a[i] = 0xAA;
    for (size_t i = 0; i < 128; i++) b[i] = 0xBB;
    for (size_t i = 0; i < 32; i++) c[i] = 0xCC;

    int ok = 1;
    for (size_t i = 0; i < 64; i++) if (a[i] != 0xAA) ok = 0;
    for (size_t i = 0; i < 128; i++) if (b[i] != 0xBB) ok = 0;
    for (size_t i = 0; i < 32; i++) if (c[i] != 0xCC) ok = 0;

    kfree(b);
    uint8_t *d = kmalloc(100); /* should fit in the space b just freed */
    if (!d) {
        ok = 0;
    } else {
        for (size_t i = 0; i < 100; i++) d[i] = 0xDD;
        for (size_t i = 0; i < 64; i++) if (a[i] != 0xAA) ok = 0;
        for (size_t i = 0; i < 32; i++) if (c[i] != 0xCC) ok = 0;
    }

    kfree(a);
    kfree(c);
    kfree(d);

    uart_puts(ok ? "heap test: OK\n" : "heap test: FAILED\n");
}

/* Raw sector dump — low-level check that virtio_blk_read() actually pulls
   real disk contents, independent of anything GPT/FAT32 will build on top
   of it later. Prints the first 64 bytes of the requested sector as hex. */
static void run_blkdump(const char *args) {
    uint64_t sector = 0;
    for (const char *p = args; *p >= '0' && *p <= '9'; p++) {
        sector = sector * 10 + (uint64_t)(*p - '0');
    }

    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        uart_puts("blkdump: kmalloc failed\n");
        return;
    }

    if (virtio_blk_read(sector, buf, 1) != 0) {
        uart_puts("blkdump: read failed\n");
        kfree(buf);
        return;
    }

    for (int i = 0; i < 64; i++) {
        static const char digits[] = "0123456789abcdef";
        uart_putc(digits[(buf[i] >> 4) & 0xF]);
        uart_putc(digits[buf[i] & 0xF]);
        uart_putc(' ');
        if ((i + 1) % 16 == 0) {
            uart_putc('\n');
        }
    }
    kfree(buf);
}

static void ls_visit(const char *name, uint32_t size, int is_dir) {
    uart_puts(is_dir ? "d  " : "-  ");
    uart_puthex(size);
    uart_puts("  ");
    uart_puts(name);
    uart_puts("\n");
}

static void run_cat(const char *name) {
    if (*name == '\0') {
        uart_puts("usage: cat <name>\n");
        return;
    }

    uint8_t *data;
    uint32_t size;
    if (fat16_read_file(name, &data, &size) != 0) {
        uart_puts("cat: not found: "); uart_puts(name); uart_puts("\n");
        return;
    }

    for (uint32_t i = 0; i < size; i++) {
        uart_putc((char)data[i]);
    }
    if (size == 0 || data[size - 1] != '\n') {
        uart_putc('\n');
    }
    kfree(data);
}

/* Polls both virtio-input devices for ~5s, printing every event as it
   arrives — type/code/value straight off the wire (see
   virtio_input_event_t), no interpretation. Type through the QEMU window
   (NAUMIOS_DISPLAY=gtk scripts/run-qemu.sh) or move the mouse in it while
   this is running to see real events, not just a "device found" message. */
static void run_keytest(void) {
    uart_puts("keytest: polling for 5s -- type or move the mouse in the QEMU window\n");
    uint64_t deadline = timer_ticks() + 500;

    while (timer_ticks() < deadline) {
        virtio_input_event_t ev;
        if (input_poll_keyboard(&ev)) {
            uart_puts("[kbd]   type="); uart_puthex(ev.type);
            uart_puts(" code="); uart_puthex(ev.code);
            uart_puts(" value="); uart_puthex(ev.value);
            uart_puts("\n");
        }
        if (input_poll_mouse(&ev)) {
            uart_puts("[mouse] type="); uart_puthex(ev.type);
            uart_puts(" code="); uart_puthex(ev.code);
            uart_puts(" value="); uart_puthex(ev.value);
            uart_puts("\n");
        }
    }
    uart_puts("keytest: done\n");
}

static int contains_slash(const char *s) {
    for (; *s; s++) {
        if (*s == '/') {
            return 1;
        }
    }
    return 0;
}

/* Loads and launches an ELF straight off the FAT16 volume as a fresh,
   isolated process — the dynamic counterpart to the two user_demo
   instances main.c spawns at boot from a fixed Limine module. Bare names
   ("cat.elf") are looked up in /bin, so the shell doesn't need
   "run bin/cat.elf" for the common case; an explicit path with a '/' in
   it is used as-is. */
static void run_run(const char *name) {
    if (*name == '\0') {
        uart_puts("usage: run <name.elf>\n");
        return;
    }

    char path[64];
    if (contains_slash(name)) {
        size_t i = 0;
        for (; name[i] != '\0' && i < sizeof(path) - 1; i++) {
            path[i] = name[i];
        }
        path[i] = '\0';
    } else {
        size_t i = 0;
        const char *prefix = "bin/";
        for (; prefix[i]; i++) {
            path[i] = prefix[i];
        }
        size_t j = 0;
        for (; name[j] != '\0' && i + j < sizeof(path) - 1; j++) {
            path[i + j] = name[j];
        }
        path[i + j] = '\0';
    }

    uint8_t *data;
    uint32_t size;
    if (fat16_read_file(path, &data, &size) != 0) {
        uart_puts("run: not found: "); uart_puts(path); uart_puts("\n");
        return;
    }

    int pid = spawn_elf_bytes(name, data, size);
    kfree(data); /* elf_load() copies what it needs into its own pages */

    if (pid < 0) {
        uart_puts("run: failed to load "); uart_puts(name); uart_puts("\n");
    } else {
        uart_puts("run: started pid "); uart_puthex((uint64_t)pid); uart_puts("\n");
    }
}

static void run_command(char *line) {
    char *cmd = line;
    char *args = line;
    while (*args && *args != ' ') {
        args++;
    }
    if (*args == ' ') {
        *args = '\0';
        args++;
    }

    if (*cmd == '\0') {
        return;
    } else if (streq(cmd, "help")) {
        uart_puts("Commands: help, meminfo, ticks, ps, echo <text>, heaptest, sleep <ticks>, blkdump <sector>, ls [dir], cat <name>, run <name.elf>, keytest\n");
    } else if (streq(cmd, "keytest")) {
        run_keytest();
    } else if (streq(cmd, "heaptest")) {
        run_heaptest();
    } else if (streq(cmd, "blkdump")) {
        run_blkdump(args);
    } else if (streq(cmd, "ls")) {
        fat16_list_dir(args, ls_visit);
    } else if (streq(cmd, "cat")) {
        run_cat(args);
    } else if (streq(cmd, "run")) {
        run_run(args);
    } else if (streq(cmd, "sleep")) {
        /* No strtol here — freestanding, no libc. */
        uint64_t n = 0;
        for (const char *p = args; *p >= '0' && *p <= '9'; p++) {
            n = n * 10 + (uint64_t)(*p - '0');
        }
        if (n == 0) {
            n = 100; /* ~1s at 100 Hz */
        }
        uart_puts("sleeping "); uart_puthex(n); uart_puts(" ticks -- other tasks keep running, this shell doesn't\n");
        sched_sleep_ticks(n);
        uart_puts("awake\n");
    } else if (streq(cmd, "meminfo")) {
        uart_puts("free: "); uart_puthex(pmm_free_pages() * PAGE_SIZE);
        uart_puts(" / total: "); uart_puthex(pmm_total_pages() * PAGE_SIZE);
        uart_puts("\n");
    } else if (streq(cmd, "ticks")) {
        uart_puts("ticks: "); uart_puthex(timer_ticks()); uart_puts("\n");
    } else if (streq(cmd, "ps")) {
        sched_for_each(ps_visit);
    } else if (streq(cmd, "echo")) {
        uart_puts(args);
        uart_puts("\n");
    } else {
        uart_puts("unknown command: "); uart_puts(cmd); uart_puts(" (try 'help')\n");
    }
}

/* Arrow keys, Home/End, etc. arrive as multi-byte ANSI escape sequences
   (ESC '[' ... final-byte). Without this, the '[' and letter after ESC
   would leak into the line buffer as if the user had typed them. */
enum escape_state { ESC_NONE, ESC_SAW_ESC, ESC_IN_CSI };

void shell_task(void) {
    char line[LINE_MAX];
    size_t len = 0;
    enum escape_state esc = ESC_NONE;

    uart_puts("\nNaumiOS shell -- type 'help'\n");
    print_prompt();

    for (;;) {
        char c;
        if (!uart_try_getc(&c)) {
            continue; /* nothing typed this slice; timer preemption moves on */
        }

        if (esc == ESC_SAW_ESC) {
            esc = (c == '[') ? ESC_IN_CSI : ESC_NONE;
            continue;
        }
        if (esc == ESC_IN_CSI) {
            if (c >= 0x40 && c <= 0x7e) { /* CSI final byte: sequence done */
                esc = ESC_NONE;
            }
            continue;
        }
        if (c == 0x1b) {
            esc = ESC_SAW_ESC;
            continue;
        }

        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            line[len] = '\0';
            run_command(line);
            len = 0;
            print_prompt();
        } else if (c == 0x7f || c == 0x08) { /* DEL or backspace */
            if (len > 0) {
                len--;
                uart_puts("\b \b");
            }
        } else if (len < LINE_MAX - 1 && c >= 0x20 && c < 0x7f) {
            line[len++] = c;
            uart_putc(c);
        }
    }
}

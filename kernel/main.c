#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include "drivers/uart.h"
#include "drivers/virtio_blk.h"
#include "drivers/framebuffer.h"
#include "drivers/font.h"
#include "drivers/input.h"
#include "drivers/virtio_sound.h"
#include "fs/gpt.h"
#include "fs/fat16.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "arch/aarch64/exceptions.h"
#include "arch/aarch64/gic.h"
#include "arch/aarch64/timer.h"
#include "sched/sched.h"
#include "loader/spawn.h"
#include "shell/shell.h"
#include "gfx/compositor.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static void hcf(void) {
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

/* Demo tasks proving preemptive round-robin scheduling: each just prints
   and sleeps, and the timer tick interleaves their output with kmain's own
   "task 0" loop below. A real sched_sleep_ticks() block (not a busy-wait)
   — these used to spin, which kept them TASK_STATE_READY the whole time,
   so pick_next()'s round-robin gave them a full turn every cycle for
   nothing. That's the difference between "8 tasks sharing turns" and "2
   tasks (compositor + whatever's actually working) sharing turns" once
   enough of the tree's idle loops became real sleeps instead. */
static void task_a(void) {
    uint32_t n = 0;
    for (;;) {
        uart_puts("[A] "); uart_puthex(n++); uart_puts("\n");
        sched_sleep_ticks(200); /* 2s at the 100 Hz tick rate */
    }
}

static void task_b(void) {
    uint32_t n = 0;
    for (;;) {
        uart_puts("[B] "); uart_puthex(n++); uart_puts("\n");
        sched_sleep_ticks(200);
    }
}

/* Proves the pixel-pushing actually works before anything more interesting
   (window manager, DOOM) gets built on top of it: 8 vertical color bars
   plus a border rectangle, entirely distinct from "screen full of one
   color" in case channel order (red_shift/green_shift/blue_shift) is
   silently wrong. */
static void draw_test_pattern(void) {
    uint64_t w = fb_width();
    uint64_t h = fb_height();
    uint32_t colors[8] = {
        fb_rgb(255, 255, 255), fb_rgb(255, 255, 0), fb_rgb(0, 255, 255), fb_rgb(0, 255, 0),
        fb_rgb(255, 0, 255),   fb_rgb(255, 0, 0),   fb_rgb(0, 0, 255),   fb_rgb(0, 0, 0),
    };

    uint32_t band = (uint32_t)(w / 8);
    for (int i = 0; i < 8; i++) {
        fb_fill_rect((uint32_t)i * band, 0, band, (uint32_t)h, colors[i]);
    }

    uint32_t border = fb_rgb(255, 128, 0);
    fb_fill_rect(0, 0, (uint32_t)w, 4, border);
    fb_fill_rect(0, (uint32_t)h - 4, (uint32_t)w, 4, border);
    fb_fill_rect(0, 0, 4, (uint32_t)h, border);
    fb_fill_rect((uint32_t)w - 4, 0, 4, (uint32_t)h, border);

    fb_present(); /* drawing only touches the back buffer now — nothing is
                     visible until this copies it to the real screen */
}

void kmain(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        hcf();
    }

    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        hcf();
    }
    uint64_t hhdm_offset = hhdm_request.response->offset;

    pmm_init(memmap_request.response, hhdm_offset);
    vmm_init(hhdm_offset);
    heap_init();

    uart_init();
    exceptions_init();
    gic_init();
    timer_init(100); /* 100 Hz tick */

    if (virtio_blk_init() != 0) {
        uart_puts("virtio_blk_init failed\n");
        hcf();
    }

    uint64_t part_lba = gpt_find_first_partition_lba();
    if (part_lba == 0 || fat16_mount(part_lba) != 0) {
        uart_puts("GPT/FAT16 mount failed\n");
        hcf();
    }

    int have_fb = 0;
    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        if (fb_init(fb->address, fb->width, fb->height, fb->pitch, fb->bpp,
                     fb->red_mask_shift, fb->green_mask_shift, fb->blue_mask_shift) == 0) {
            have_fb = 1;
            draw_test_pattern();
            font_init(); /* needs FAT16 mounted (see above) — must run before any text is drawn */
            compositor_init();
        }
    }

    input_init();
    virtio_sound_init(); /* optional — no device / setup failure just means silence, not a boot failure */

    sched_init();
    task_create("task_a", task_a);
    task_create("task_b", task_b);
    task_create("shell", shell_task);
    if (have_fb) {
        /* Owns the real screen from here on: draws every window's chrome,
           composites by z-order, and routes input — see
           kernel/gfx/compositor.c. Nothing else should touch fb_present()
           or the screen buffer directly once this is running. */
        task_create("compositor", compositor_task);
    }

    if (module_request.response == NULL || module_request.response->module_count < 1) {
        uart_puts("No user_demo module — check limine.conf module_path\n");
        hcf();
    }
    struct limine_file *user_module = module_request.response->modules[0];

    /* Two independent instances of the same program, each fully isolated —
       the concrete proof that per-process address spaces actually work:
       both use identical virtual addresses but distinct physical backing,
       so their counters can't collide. */
    if (spawn_elf_bytes("user_demo", user_module->address, user_module->size) < 0 ||
        spawn_elf_bytes("user_demo", user_module->address, user_module->size) < 0) {
        uart_puts("Failed to load user_demo ELF\n");
        hcf();
    }

    irq_enable();

    /* Limine's boot menu leaves cursor-positioned junk on screen via ANSI
       codes; clear it so our plain-text log doesn't render interleaved
       with menu leftovers on a real terminal. */
    uart_puts("\x1b[2J\x1b[H");

    uart_puts("NaumiOS boot OK\n");
    uart_puts("Exception vectors installed\n");
    uart_puts("Physical memory: ");
    uart_puthex(pmm_free_pages() * PAGE_SIZE);
    uart_puts(" free / ");
    uart_puthex(pmm_total_pages() * PAGE_SIZE);
    uart_puts(" total\n");
    uart_puts("Timer + GIC armed at 100 Hz\n");
    if (have_fb) {
        uart_puts("Framebuffer: "); uart_puthex(fb_width());
        uart_puts("x"); uart_puthex(fb_height());
        uart_puts(" (test pattern drawn)\n");
    } else {
        uart_puts("Framebuffer: none\n");
    }
    uart_puts("Scheduler running: task 0 (this loop) + task_a + task_b + shell + 2x isolated EL0 user demo\n");

    /* kmain is task 0 from here on — the scheduler round-robins between
       this loop, task_a/task_b, the shell, and the EL0 demo on every timer
       tick. Reports rarely so it doesn't compete with the shell for UART. */
    uint64_t last_reported = 0;
    for (;;) {
        __asm__ volatile ("wfi");
        uint64_t t = timer_ticks();
        if (t - last_reported >= 1000) { /* ~once per 10s */
            last_reported = t;
            uart_puts("[0] tick: ");
            uart_puthex(t);
            uart_puts("\n");
        }
    }
}

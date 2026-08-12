#include "../lib/naumi.h"
#include "../lib/libc.h"
#include <stdio.h>

/* Exercises the DOOM-foundation syscalls end to end: SYS_SBRK + malloc/free
   (allocations that cross several page-growth boundaries, pattern
   verification, coalescing, realloc), SYS_FB_BLIT (a whole-frame pixel
   push into a real window instead of many small fill-rect calls), and
   SYS_GET_TICKS_MS (monotonic timing for a game loop). Prints "OK"/"FAIL"
   over stdout (UART) and leaves a gradient window up for a few seconds so
   the blit result can be screenshotted. A diagnostic, not a permanent
   userland program. */

static void print(const char *s) {
    unsigned long n = 0;
    while (s[n]) {
        n++;
    }
    sys_write(1, s, n);
}

static void print_dec(long v) {
    char buf[24];
    int i = 0;
    int neg = v < 0;
    unsigned long uv = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (uv == 0) {
        buf[i++] = '0';
    }
    while (uv > 0) {
        buf[i++] = (char)('0' + (uv % 10));
        uv /= 10;
    }
    if (neg) {
        buf[i++] = '-';
    }
    char rev[24];
    for (int k = 0; k < i; k++) {
        rev[k] = buf[i - 1 - k];
    }
    rev[i] = '\0';
    print(rev);
}

void _start(void) {
    print("memtest: start\n");

    /* Many mid-size allocations to force several sbrk growths and page
       boundaries in the underlying SYS_SBRK mapping. */
    #define N 200
    static char *ptrs[N];
    static unsigned long sizes[N];

    for (int i = 0; i < N; i++) {
        unsigned long size = (unsigned long)(37 + i * 13) % 4000 + 1;
        char *p = (char *)malloc(size);
        if (!p) {
            print("memtest: malloc failed at i="); print_dec(i); print("\n");
            sys_exit();
        }
        for (unsigned long j = 0; j < size; j++) {
            p[j] = (char)((i + (int)j) & 0xFF);
        }
        ptrs[i] = p;
        sizes[i] = size;
    }

    int fail = 0;
    for (int i = 0; i < N; i++) {
        for (unsigned long j = 0; j < sizes[i]; j++) {
            if (ptrs[i][j] != (char)((i + (int)j) & 0xFF)) {
                fail = 1;
            }
        }
    }
    print(fail ? "memtest: pattern check FAILED\n" : "memtest: pattern check OK\n");

    /* Free every other allocation, then reallocate similarly-sized chunks
       to exercise coalescing + free-list reuse. */
    for (int i = 0; i < N; i += 2) {
        free(ptrs[i]);
    }
    for (int i = 0; i < N; i += 2) {
        unsigned long size = sizes[i];
        char *p = (char *)malloc(size);
        if (!p) {
            print("memtest: reuse-malloc failed at i="); print_dec(i); print("\n");
            sys_exit();
        }
        for (unsigned long j = 0; j < size; j++) {
            p[j] = (char)((100 + i + (int)j) & 0xFF);
        }
        ptrs[i] = p;
    }

    /* realloc growing a block past its original size. */
    char *r = (char *)malloc(64);
    for (int j = 0; j < 64; j++) {
        r[j] = (char)j;
    }
    r = (char *)realloc(r, 4096);
    int realloc_ok = 1;
    for (int j = 0; j < 64; j++) {
        if (r[j] != (char)j) {
            realloc_ok = 0;
        }
    }
    print(realloc_ok ? "memtest: realloc OK\n" : "memtest: realloc FAILED\n");

    /* memcpy/memset/strlen/strcmp sanity. */
    char a[32], b[32];
    memset(a, 0x5A, sizeof(a));
    memcpy(b, a, sizeof(a));
    int mem_ok = memcmp(a, b, sizeof(a)) == 0;
    const char *s1 = "naumios";
    int str_ok = strlen(s1) == 7 && strcmp(s1, "naumios") == 0 && strcmp(s1, "other") != 0;
    print((mem_ok && str_ok) ? "memtest: mem/str OK\n" : "memtest: mem/str FAILED\n");

    /* printf/vsnprintf formatter sanity — the format specifiers doomgeneric
       actually uses: %d %i %u %x %X %o %c %s %p, width/zero-pad, +/space
       flags, precision on %s, negative numbers. */
    char pb[160];
    snprintf(pb, sizeof(pb), "[%d|%5d|%-5d|%05d] [%u|%x|%X|%o] [%c|%s|%.3s] [%+d|% d] STCFN%.3d",
             -42, 7, 7, 7, 4000000000u, 255, 255, 8, 'Q', "hello", "hello", 3, -3, 33);
    print(pb);
    print("\n");
    const char *expected =
        "[-42|    7|7    |00007] [4000000000|ff|FF|10] [Q|hello|hel] [+3|-3] STCFN033";
    print((strcmp(pb, expected) == 0) ? "memtest: printf OK\n" : "memtest: printf FAILED\n");

    /* SYS_GET_TICKS_MS: must be monotonic and actually advance. */
    unsigned long t0 = sys_get_ticks_ms();
    unsigned long t1 = t0;
    while (t1 - t0 < 50) {
        t1 = sys_get_ticks_ms();
    }
    print((t1 >= t0) ? "memtest: ticks OK\n" : "memtest: ticks FAILED\n");

    /* SYS_FB_BLIT: a whole-frame pixel push into a real window, malloc'd
       (not stack — this is the size DOOM's own framebuffer push will be),
       a plain diagonal gradient so a screenshot can confirm it landed
       right instead of just "didn't crash". */
    #define BW 200
    #define BH 150
    sys_win_create(BW, BH, 0, "Blit test");
    unsigned int *pixels = (unsigned int *)malloc(BW * BH * sizeof(unsigned int));
    if (pixels) {
        for (unsigned int y = 0; y < BH; y++) {
            for (unsigned int x = 0; x < BW; x++) {
                pixels[y * BW + x] = rgb((unsigned char)(x * 255 / BW), (unsigned char)(y * 255 / BH), 128);
            }
        }
        sys_fb_blit(0, 0, BW, BH, pixels);
        sys_fb_present();
        print("memtest: blit OK\n");
    } else {
        print("memtest: blit FAILED (no memory)\n");
    }

    print("memtest: done\n");

    /* Keep the window up for a few seconds so it can be screenshotted —
       the compositor tears the window down the instant this process
       exits (see gc_dead_windows() in kernel/gfx/compositor.c). */
    unsigned long deadline = sys_get_ticks_ms() + 4000;
    while (sys_get_ticks_ms() < deadline) { }

    sys_exit();
}

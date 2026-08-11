#include <stdint.h>
#include "framebuffer.h"
#include "../mm/heap.h"

static volatile uint8_t *fb_base; /* real, visible device memory */
static uint8_t *back_buf;         /* off-screen, tightly packed (stride = width*4) */
static uint64_t fb_w;
static uint64_t fb_h;
static uint64_t fb_pitch; /* device stride, may differ from back_buf's */
static uint8_t r_shift, g_shift, b_shift;
static int fb_ready;

int fb_init(void *address, uint64_t width, uint64_t height, uint64_t pitch,
            uint16_t bpp, uint8_t red_shift, uint8_t green_shift, uint8_t blue_shift) {
    if (bpp != 32) {
        return -1;
    }

    back_buf = (uint8_t *)kmalloc(width * height * 4);
    if (!back_buf) {
        return -1;
    }

    fb_base = (volatile uint8_t *)address;
    fb_w = width;
    fb_h = height;
    fb_pitch = pitch;
    r_shift = red_shift;
    g_shift = green_shift;
    b_shift = blue_shift;
    fb_ready = 1;
    return 0;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << r_shift) | ((uint32_t)g << g_shift) | ((uint32_t)b << b_shift);
}

uint8_t *fb_screen_buffer(void) {
    return back_buf;
}

void fb_buf_put_pixel(uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
                       uint32_t x, uint32_t y, uint32_t color) {
    if (!buf || x >= buf_w || y >= buf_h) {
        return;
    }
    uint32_t *p = (uint32_t *)(buf + (uint64_t)y * buf_w * 4 + (uint64_t)x * 4);
    *p = color;
}

void fb_buf_fill_rect(uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!buf || x >= buf_w || y >= buf_h) {
        return;
    }
    uint32_t x2 = x + w > buf_w ? buf_w : x + w;
    uint32_t y2 = y + h > buf_h ? buf_h : y + h;

    /* Clipped once up front, then a tight per-row loop — no per-pixel
       bounds checks or function-call overhead, unlike calling
       fb_buf_put_pixel() in a loop. This is what actually made full-screen
       redraws fast enough to not visibly lag. */
    for (uint32_t yy = y; yy < y2; yy++) {
        uint32_t *row = (uint32_t *)(buf + (uint64_t)yy * buf_w * 4) + x;
        for (uint32_t xx = x; xx < x2; xx++) {
            *row++ = color;
        }
    }
}

void fb_buf_blit(uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_x, uint32_t dst_y,
                  const uint8_t *src, uint32_t src_pitch, uint32_t src_w, uint32_t src_h) {
    if (!dst || !src || dst_x >= dst_w || dst_y >= dst_h) {
        return;
    }
    uint32_t copy_w = src_w;
    uint32_t copy_h = src_h;
    if (dst_x + copy_w > dst_w) {
        copy_w = dst_w - dst_x;
    }
    if (dst_y + copy_h > dst_h) {
        copy_h = dst_h - dst_y;
    }

    for (uint32_t row = 0; row < copy_h; row++) {
        const uint32_t *srow = (const uint32_t *)(src + (uint64_t)row * src_pitch * 4);
        uint32_t *drow = (uint32_t *)(dst + (uint64_t)(dst_y + row) * dst_w * 4) + dst_x;
        for (uint32_t col = 0; col < copy_w; col++) {
            drow[col] = srow[col];
        }
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    fb_buf_put_pixel(back_buf, (uint32_t)fb_w, (uint32_t)fb_h, x, y, color);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    fb_buf_fill_rect(back_buf, (uint32_t)fb_w, (uint32_t)fb_h, x, y, w, h, color);
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, (uint32_t)fb_w, (uint32_t)fb_h, color);
}

void fb_present(void) {
    if (!fb_ready) {
        return;
    }
    uint64_t row_bytes = fb_w * 4;
    uint64_t words = row_bytes / 8;      /* whole 8-byte chunks per row */
    uint64_t tail = row_bytes % 8;       /* leftover bytes if width*4 isn't a multiple of 8 */

    for (uint64_t y = 0; y < fb_h; y++) {
        const uint64_t *src = (const uint64_t *)(back_buf + y * row_bytes);
        uint64_t *dst = (uint64_t *)(fb_base + y * fb_pitch);
        for (uint64_t i = 0; i < words; i++) {
            dst[i] = src[i];
        }
        if (tail) {
            const uint8_t *src_b = (const uint8_t *)(src + words);
            uint8_t *dst_b = (uint8_t *)(dst + words);
            for (uint64_t i = 0; i < tail; i++) {
                dst_b[i] = src_b[i];
            }
        }
    }
}

uint64_t fb_width(void) {
    return fb_w;
}

uint64_t fb_height(void) {
    return fb_h;
}

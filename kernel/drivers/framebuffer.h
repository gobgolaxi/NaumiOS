#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

/* `address` must already be a directly usable kernel pointer — Limine's
   framebuffer response gives one (LIMINE_MEMMAP_FRAMEBUFFER regions are
   HHDM-covered, same as usable RAM), no vmm mapping needed on our end.
   Only 32bpp is supported (what every backend we target — ramfb, virtio-gpu
   — actually hands out). Allocates an off-screen "screen buffer" the same
   size (see fb_present()). Returns 0 on success, -1 if bpp != 32 or the
   buffer allocation failed. */
int fb_init(void *address, uint64_t width, uint64_t height, uint64_t pitch,
            uint16_t bpp, uint8_t red_shift, uint8_t green_shift, uint8_t blue_shift);

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/* All drawing goes to the screen buffer only — nothing is visible on the
   real screen until fb_present() copies it over. Without this, redrawing
   several rects directly against the live, scanned-out framebuffer is
   exactly what causes visible flicker/tearing. */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);

/* Direct pointer to the screen buffer (tightly packed, stride = width*4) —
   for the compositor to draw window chrome/cursor straight into, and to use
   as the destination of fb_buf_blit() when compositing window contents. */
uint8_t *fb_screen_buffer(void);

/* Generic versions of the above that operate on any caller-supplied buffer
   (e.g. a per-window private pixel buffer) instead of the screen buffer.
   `buf_w`/`buf_h` describe that buffer's own dimensions (pitch = buf_w*4,
   tightly packed) and double as the clip bounds. */
void fb_buf_put_pixel(uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
                       uint32_t x, uint32_t y, uint32_t color);
void fb_buf_fill_rect(uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

/* Copies a `src_w`x`src_h` region starting at (0,0) of `src` (stride =
   src_pitch*4) into `dst` (stride = dst_w*4) at (dst_x, dst_y), clipped
   against dst's bounds. This is how the compositor blits each window's
   private buffer into the screen buffer. */
void fb_buf_blit(uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_x, uint32_t dst_y,
                  const uint8_t *src, uint32_t src_pitch, uint32_t src_w, uint32_t src_h);

/* Copies the whole screen buffer to the real framebuffer in one pass
   (8-byte-at-a-time). Call once per finished frame, not per draw call. */
void fb_present(void);

uint64_t fb_width(void);
uint64_t fb_height(void);

#endif

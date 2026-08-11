#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/* Must run once, after the FAT16 volume is mounted and before any text is
   drawn (main.c calls this right after fb_init() succeeds). Loads
   /font.ttf (see kernel/gfx/ttf.h) and rasterizes real glyph outlines from
   then on. If that file is missing or fails to parse, drawing falls back
   to a small hand-authored 5x7 bitmap font (uppercase only, a limited
   punctuation set) rather than rendering nothing. */
void font_init(void);

/* Draws `text` at (x, y) into an arbitrary buffer (screen buffer or a
   window's own private buffer — see framebuffer.h's fb_buf_* family, which
   this is built on). `scale` selects a pixel em size (see TTF_BASE_PX in
   font.c) when the TTF font loaded; the bitmap fallback treats it as an
   integer nearest-neighbor multiplier instead. Unknown/unmapped characters
   render as a blank (but still correctly spaced) cell. */
void font_draw_text_buf(uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
                         uint32_t x, uint32_t y, const char *text,
                         uint32_t fg, uint32_t bg, uint32_t scale);

/* Pixel advance width / line height at the given scale — for callers
   laying out text before drawing it. */
uint32_t font_glyph_width(uint32_t scale);
uint32_t font_glyph_height(uint32_t scale);

#endif

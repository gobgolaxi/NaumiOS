#ifndef TTF_H
#define TTF_H

#include <stdint.h>

/* A from-scratch TrueType outline rasterizer — parses the sfnt tables
   needed to go from a character to an antialiased pixel glyph (head, maxp,
   cmap format 4, loca, glyf, hmtx, hhea), flattens the quadratic Bezier
   contours into line segments, and rasterizes them with a nonzero-winding
   scanline fill plus supersampled antialiasing. Everything is fixed-point
   (26.6, see FX_ONE in ttf.c) — this kernel is built with
   -mgeneral-regs-only -march=armv8-a+nofp+nosimd (no FPU context is saved
   across task switches, so nothing kernel-side may use float/double), not
   a stylistic choice.

   Deliberately scoped to what Latin-alphabet UI text needs: simple glyphs
   only (no composite-glyph support — accented/composed characters outside
   what this project renders), format-4 cmap (covers the Basic Multilingual
   Plane, which is all any of our text needs). A glyph outside that scope
   silently renders as blank rather than risking a misrender. */

typedef struct {
    const uint8_t *data; /* whole font file; caller owns/keeps it alive */
    uint32_t size;
    uint32_t glyf_off, glyf_len;
    uint32_t loca_off;
    uint32_t cmap_off; /* absolute offset of the chosen format-4 subtable */
    uint32_t hmtx_off;
    uint16_t num_glyphs;
    uint16_t units_per_em;
    int16_t index_to_loc_format;
    uint16_t num_hmetrics;
    int16_t ascender;
    int16_t descender;
    int loaded;
} ttf_font_t;

/* Parses the sfnt table directory out of `data` (referenced, not copied —
   caller must keep it alive, e.g. a kmalloc'd buffer that's never freed).
   Returns 0 on success, -1 if it isn't a TrueType-flavored (glyf/loca
   based, not CFF/OpenType) sfnt or is missing a table this renderer needs. */
int ttf_load(const uint8_t *data, uint32_t size, ttf_font_t *font);

/* Renders one glyph's whole advance cell — background fill plus the
   antialiased glyph outline blended over it — at (x0, y0) top-left, into
   `buf` (buf_w x buf_h, tightly packed 32bpp), scaled to `px_size` pixels
   of em height. Unmapped/composite/malformed glyphs still paint the
   background (so spacing stays correct) but draw no ink. */
void ttf_draw_glyph(const ttf_font_t *font, uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
                     uint32_t x0, uint32_t y0, char ch, uint32_t fg, uint32_t bg, uint32_t px_size);

/* This font's monospace advance width in pixels at the given em pixel
   size (every glyph shares it — Liberation Mono is strictly monospace). */
uint32_t ttf_advance_width(const ttf_font_t *font, uint32_t px_size);

/* Tight line height in pixels (ascender - descender, scaled) at the given
   em pixel size — for callers laying out multiple lines of text. */
uint32_t ttf_line_height(const ttf_font_t *font, uint32_t px_size);

#endif

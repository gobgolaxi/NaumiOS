#include <stdint.h>
#include <stddef.h>
#include "ttf.h"
#include "../drivers/framebuffer.h"

#define FX_SHIFT 6
#define FX_ONE (1 << FX_SHIFT) /* 26.6 fixed point: 64 subpixel units per pixel */
#define NSEG 6                 /* line segments per flattened quadratic Bezier */
#define SUBSAMPLES 4            /* vertical supersampling rate for antialiasing */

#define MAX_POINTS 256   /* raw contour points per glyph — plenty for Latin letterforms */
#define MAX_CONTOURS 16
#define MAX_NORM 128     /* normalized (on/off-curve resolved) points per contour */
#define MAX_EDGES 512    /* flattened line segments per glyph */
#define MAX_CELL 64      /* max glyph advance-cell width/height in pixels */

typedef struct {
    int32_t x, y;
    uint8_t on_curve;
} gpoint_t;

typedef struct {
    int32_t x0, y0, x1, y1;
} edge_t;

typedef struct {
    int num_contours;
    uint16_t contour_ends[MAX_CONTOURS];
    gpoint_t points[MAX_POINTS];
    int num_points;
} outline_t;

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

static int16_t rd_i16(const uint8_t *p) {
    return (int16_t)rd_u16(p);
}

static uint32_t rd_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int ttf_load(const uint8_t *data, uint32_t size, ttf_font_t *font) {
    font->loaded = 0;
    if (size < 12) {
        return -1;
    }
    uint32_t sfnt_version = rd_u32(data);
    if (sfnt_version != 0x00010000u && sfnt_version != 0x74727565u) {
        return -1; /* not TrueType-flavored (e.g. 'OTTO' CFF outlines) */
    }
    uint16_t num_tables = rd_u16(data + 4);
    if (size < 12u + 16u * num_tables) {
        return -1;
    }

    uint32_t head_off = 0, maxp_off = 0, cmap_off = 0, loca_off = 0;
    uint32_t glyf_off = 0, glyf_len = 0, hmtx_off = 0, hhea_off = 0;
    int have_head = 0, have_maxp = 0, have_cmap = 0, have_loca = 0;
    int have_glyf = 0, have_hmtx = 0, have_hhea = 0;

    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *rec = data + 12 + 16u * i;
        uint32_t tag = rd_u32(rec);
        uint32_t off = rd_u32(rec + 8);
        uint32_t len = rd_u32(rec + 12);
        if ((uint64_t)off + len > size) {
            continue; /* truncated/corrupt table entry: skip rather than trust it */
        }
        switch (tag) {
        case 0x68656164u: head_off = off; have_head = 1; break; /* 'head' */
        case 0x6D617870u: maxp_off = off; have_maxp = 1; break; /* 'maxp' */
        case 0x636D6170u: cmap_off = off; have_cmap = 1; break; /* 'cmap' */
        case 0x6C6F6361u: loca_off = off; have_loca = 1; break; /* 'loca' */
        case 0x676C7966u: glyf_off = off; glyf_len = len; have_glyf = 1; break; /* 'glyf' */
        case 0x686D7478u: hmtx_off = off; have_hmtx = 1; break; /* 'hmtx' */
        case 0x68686561u: hhea_off = off; have_hhea = 1; break; /* 'hhea' */
        default: break;
        }
    }
    if (!have_head || !have_maxp || !have_cmap || !have_loca || !have_glyf || !have_hmtx || !have_hhea) {
        return -1;
    }
    if (head_off + 52 > size || maxp_off + 6 > size || hhea_off + 36 > size) {
        return -1;
    }

    font->data = data;
    font->size = size;
    font->units_per_em = rd_u16(data + head_off + 18);
    font->index_to_loc_format = rd_i16(data + head_off + 50);
    font->num_glyphs = rd_u16(data + maxp_off + 4);
    font->ascender = rd_i16(data + hhea_off + 4);
    font->descender = rd_i16(data + hhea_off + 6);
    font->num_hmetrics = rd_u16(data + hhea_off + 34);
    font->loca_off = loca_off;
    font->glyf_off = glyf_off;
    font->glyf_len = glyf_len;
    font->hmtx_off = hmtx_off;

    if (font->units_per_em == 0 || font->num_hmetrics == 0) {
        return -1;
    }

    uint16_t ntab = rd_u16(data + cmap_off + 2);
    uint32_t chosen = 0;
    for (uint16_t i = 0; i < ntab; i++) {
        if (cmap_off + 4u + 8u * (i + 1u) > size) {
            break;
        }
        const uint8_t *rec = data + cmap_off + 4 + 8u * i;
        uint16_t pid = rd_u16(rec);
        uint16_t eid = rd_u16(rec + 2);
        uint32_t sub_off = cmap_off + rd_u32(rec + 4);
        if (sub_off + 2 > size) {
            continue;
        }
        uint16_t fmt = rd_u16(data + sub_off);
        if (fmt != 4) {
            continue; /* only format 4 (BMP segment mapping) is supported */
        }
        if (chosen == 0) {
            chosen = sub_off;
        }
        if (pid == 3 && eid == 1) { /* Windows, Unicode BMP — preferred */
            chosen = sub_off;
            break;
        }
    }
    if (chosen == 0) {
        return -1;
    }
    font->cmap_off = chosen;

    font->loaded = 1;
    return 0;
}

static uint16_t cmap_lookup(const ttf_font_t *font, uint32_t ch) {
    const uint8_t *d = font->data;
    uint32_t sub = font->cmap_off;
    uint16_t segX2 = rd_u16(d + sub + 6);
    uint16_t segcount = (uint16_t)(segX2 / 2);
    uint32_t endcodes = sub + 14;
    uint32_t startcodes = endcodes + segX2 + 2;
    uint32_t iddeltas = startcodes + segX2;
    uint32_t idranges = iddeltas + segX2;

    for (uint16_t i = 0; i < segcount; i++) {
        uint16_t end = rd_u16(d + endcodes + 2u * i);
        if (ch > end) {
            continue;
        }
        uint16_t start = rd_u16(d + startcodes + 2u * i);
        if (ch < start) {
            return 0;
        }
        int16_t delta = rd_i16(d + iddeltas + 2u * i);
        uint16_t range = rd_u16(d + idranges + 2u * i);
        if (range == 0) {
            return (uint16_t)(ch + (uint32_t)delta);
        }
        uint32_t addr = idranges + 2u * i + range + (ch - start) * 2u;
        if (addr + 2 > font->size) {
            return 0;
        }
        uint16_t gid = rd_u16(d + addr);
        if (gid == 0) {
            return 0;
        }
        return (uint16_t)(gid + delta);
    }
    return 0;
}

static void glyph_range(const ttf_font_t *font, uint16_t gid, uint32_t *off, uint32_t *len) {
    const uint8_t *d = font->data;
    uint32_t o1, o2;
    if (font->index_to_loc_format == 0) {
        o1 = (uint32_t)rd_u16(d + font->loca_off + gid * 2u) * 2u;
        o2 = (uint32_t)rd_u16(d + font->loca_off + gid * 2u + 2u) * 2u;
    } else {
        o1 = rd_u32(d + font->loca_off + gid * 4u);
        o2 = rd_u32(d + font->loca_off + gid * 4u + 4u);
    }
    *off = font->glyf_off + o1;
    *len = (o2 > o1) ? (o2 - o1) : 0;
}

static uint16_t advance_width_units(const ttf_font_t *font, uint16_t gid) {
    uint16_t idx = gid < font->num_hmetrics ? gid : (uint16_t)(font->num_hmetrics - 1);
    return rd_u16(font->data + font->hmtx_off + (uint32_t)idx * 4u);
}

/* Parses a simple (non-composite) glyph outline: contour endpoint indices,
   then flag/coordinate arrays per the TrueType spec (RLE flags, delta-
   encoded x then delta-encoded y coordinates). Returns -1 for composite
   glyphs (negative numberOfContours), anything too large for the static
   buffers above, or truncated/malformed data — callers treat that as "draw
   nothing" rather than trust a partial parse. */
static int parse_simple_glyph(const uint8_t *g, uint32_t len, outline_t *out) {
    if (len < 10) {
        return -1;
    }
    int16_t numberOfContours = rd_i16(g);
    if (numberOfContours <= 0 || numberOfContours > MAX_CONTOURS) {
        return -1;
    }
    const uint8_t *p = g + 10;
    const uint8_t *gend = g + len;

    for (int i = 0; i < numberOfContours; i++) {
        if (p + 2 > gend) {
            return -1;
        }
        out->contour_ends[i] = rd_u16(p);
        p += 2;
    }
    int num_points = out->contour_ends[numberOfContours - 1] + 1;
    if (num_points <= 0 || num_points > MAX_POINTS) {
        return -1;
    }

    if (p + 2 > gend) {
        return -1;
    }
    uint16_t instrLen = rd_u16(p);
    p += 2;
    p += instrLen;
    if (p > gend) {
        return -1;
    }

    static uint8_t flags[MAX_POINTS];
    int fi = 0;
    while (fi < num_points) {
        if (p + 1 > gend) {
            return -1;
        }
        uint8_t f = *p++;
        flags[fi++] = f;
        if (f & 0x08) { /* REPEAT_FLAG */
            if (p + 1 > gend) {
                return -1;
            }
            uint8_t rep = *p++;
            for (uint8_t r = 0; r < rep && fi < num_points; r++) {
                flags[fi++] = f;
            }
        }
    }

    int32_t x = 0;
    for (int i = 0; i < num_points; i++) {
        uint8_t f = flags[i];
        if (f & 0x02) { /* X_SHORT_VECTOR */
            if (p + 1 > gend) {
                return -1;
            }
            uint8_t dx = *p++;
            x += (f & 0x10) ? (int32_t)dx : -(int32_t)dx;
        } else if (!(f & 0x10)) { /* not SAME_X: 16-bit signed delta */
            if (p + 2 > gend) {
                return -1;
            }
            int16_t dx = rd_i16(p);
            p += 2;
            x += dx;
        }
        out->points[i].x = x;
        out->points[i].on_curve = (uint8_t)(f & 0x01);
    }

    int32_t y = 0;
    for (int i = 0; i < num_points; i++) {
        uint8_t f = flags[i];
        if (f & 0x04) { /* Y_SHORT_VECTOR */
            if (p + 1 > gend) {
                return -1;
            }
            uint8_t dy = *p++;
            y += (f & 0x20) ? (int32_t)dy : -(int32_t)dy;
        } else if (!(f & 0x20)) {
            if (p + 2 > gend) {
                return -1;
            }
            int16_t dy = rd_i16(p);
            p += 2;
            y += dy;
        }
        out->points[i].y = y;
    }

    out->num_contours = numberOfContours;
    out->num_points = num_points;
    return 0;
}

/* Resolves a raw contour's on/off-curve points into a sequence with the
   invariant "no two consecutive entries are off-curve" (TrueType allows
   consecutive off-curve points, implying an on-curve point exactly at
   their midpoint — this makes that midpoint an explicit entry, so the
   walk in build_edges() never has to special-case it). Always starts and
   ends on the same synthesized/real on-curve point (`begin`), i.e. the
   contour is closed. Returns the number of points written to `norm`. */
static int normalize_contour(const gpoint_t *pts, int n, gpoint_t *norm) {
    int start = -1;
    for (int i = 0; i < n; i++) {
        if (pts[i].on_curve) {
            start = i;
            break;
        }
    }

    gpoint_t begin;
    int begin_idx;
    if (start >= 0) {
        begin = pts[start];
        begin_idx = start;
    } else {
        begin.x = (pts[0].x + pts[n - 1].x) / 2;
        begin.y = (pts[0].y + pts[n - 1].y) / 2;
        begin.on_curve = 1;
        begin_idx = -1;
    }

    int nn = 0;
    if (nn < MAX_NORM) {
        norm[nn++] = begin;
    }

    int remaining = (begin_idx >= 0) ? (n - 1) : n;
    int i = begin_idx;
    int have_pending = 0;
    gpoint_t pending = { 0, 0, 0 };

    for (int k = 0; k < remaining; k++) {
        i = i + 1;
        int idx = ((i % n) + n) % n;
        gpoint_t P = pts[idx];
        if (P.on_curve) {
            if (have_pending) {
                if (nn < MAX_NORM) {
                    norm[nn++] = pending;
                }
                have_pending = 0;
            }
            if (nn < MAX_NORM) {
                norm[nn++] = P;
            }
        } else {
            if (have_pending) {
                gpoint_t mid;
                mid.x = (pending.x + P.x) / 2;
                mid.y = (pending.y + P.y) / 2;
                mid.on_curve = 1;
                if (nn < MAX_NORM) {
                    norm[nn++] = pending;
                }
                if (nn < MAX_NORM) {
                    norm[nn++] = mid;
                }
                pending = P;
            } else {
                pending = P;
                have_pending = 1;
            }
        }
    }
    if (have_pending && nn < MAX_NORM) {
        norm[nn++] = pending;
    }
    if (nn < MAX_NORM) {
        norm[nn++] = begin;
    }
    return nn;
}

static int32_t font_to_px(int32_t v, uint32_t px_size, uint16_t units_per_em) {
    return (int32_t)(((int64_t)v * (int32_t)px_size * FX_ONE) / units_per_em);
}

static void add_line(edge_t *edges, int *ne, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (y0 == y1) {
        return; /* horizontal edges never cross a scanline; skip them */
    }
    if (*ne < MAX_EDGES) {
        edges[*ne].x0 = x0;
        edges[*ne].y0 = y0;
        edges[*ne].x1 = x1;
        edges[*ne].y1 = y1;
        (*ne)++;
    }
}

/* Flattens one quadratic Bezier into NSEG line segments via exact integer
   Bernstein-basis evaluation (no float/division-by-varying-t needed):
   weights (N-i)^2, 2(N-i)i, i^2 over N^2 at each of the N sample steps. */
static void flatten_quad(edge_t *edges, int *ne,
                          int32_t p0x, int32_t p0y, int32_t p1x, int32_t p1y, int32_t p2x, int32_t p2y) {
    int32_t px = p0x, py = p0y;
    for (int i = 1; i <= NSEG; i++) {
        int32_t w0 = (NSEG - i) * (NSEG - i);
        int32_t w1 = 2 * (NSEG - i) * i;
        int32_t w2 = i * i;
        int32_t denom = NSEG * NSEG;
        int32_t x = (int32_t)(((int64_t)p0x * w0 + (int64_t)p1x * w1 + (int64_t)p2x * w2) / denom);
        int32_t y = (int32_t)(((int64_t)p0y * w0 + (int64_t)p1y * w1 + (int64_t)p2y * w2) / denom);
        add_line(edges, ne, px, py, x, y);
        px = x;
        py = y;
    }
}

/* Builds the flattened edge list for a whole glyph outline, in pixel-space
   26.6 fixed point, relative to (origin_x_fx, baseline_y_fx) — the origin
   is the glyph cell's left edge; y is flipped and offset from the
   baseline since font units increase upward but pixel space increases
   downward. */
static int build_edges(const outline_t *ol, uint32_t px_size, uint16_t units_per_em,
                        int32_t origin_x_fx, int32_t baseline_y_fx, edge_t *edges) {
    static gpoint_t norm[MAX_NORM];
    int ne = 0;
    int start = 0;

    for (int c = 0; c < ol->num_contours; c++) {
        int end = ol->contour_ends[c];
        int n = end - start + 1;
        if (n <= 0) {
            start = end + 1;
            continue;
        }

        int nn = normalize_contour(&ol->points[start], n, norm);
        if (nn < 2) {
            start = end + 1;
            continue;
        }

        int32_t cur_x = origin_x_fx + font_to_px(norm[0].x, px_size, units_per_em);
        int32_t cur_y = baseline_y_fx - font_to_px(norm[0].y, px_size, units_per_em);

        int i = 1;
        while (i < nn) {
            gpoint_t P = norm[i];
            if (P.on_curve) {
                int32_t nx = origin_x_fx + font_to_px(P.x, px_size, units_per_em);
                int32_t ny = baseline_y_fx - font_to_px(P.y, px_size, units_per_em);
                add_line(edges, &ne, cur_x, cur_y, nx, ny);
                cur_x = nx;
                cur_y = ny;
                i += 1;
            } else {
                int32_t c1x = origin_x_fx + font_to_px(P.x, px_size, units_per_em);
                int32_t c1y = baseline_y_fx - font_to_px(P.y, px_size, units_per_em);
                /* normalize_contour() guarantees the point after an
                   off-curve entry is always on-curve. */
                gpoint_t E = (i + 1 < nn) ? norm[i + 1] : norm[0];
                int32_t ex = origin_x_fx + font_to_px(E.x, px_size, units_per_em);
                int32_t ey = baseline_y_fx - font_to_px(E.y, px_size, units_per_em);
                flatten_quad(edges, &ne, cur_x, cur_y, c1x, c1y, ex, ey);
                cur_x = ex;
                cur_y = ey;
                i += 2;
            }
        }
        start = end + 1;
    }
    return ne;
}

/* Nonzero-winding scanline rasterization with SUBSAMPLES vertical
   supersampling and exact horizontal span coverage — a standard, if
   unoptimized, antialiased polygon fill. Writes fractional pixel coverage
   (0..FX_ONE*SUBSAMPLES) into `cov`. */
static void rasterize(const edge_t *edges, int ne, uint16_t cov[MAX_CELL][MAX_CELL], int px_w, int px_h) {
    for (int y = 0; y < px_h; y++) {
        for (int x = 0; x < px_w; x++) {
            cov[y][x] = 0;
        }
    }

    static int32_t xs[MAX_EDGES];
    static int8_t dirs[MAX_EDGES];
    int32_t step = FX_ONE / SUBSAMPLES;

    for (int py = 0; py < px_h; py++) {
        for (int s = 0; s < SUBSAMPLES; s++) {
            int32_t sample_y = py * FX_ONE + s * step + step / 2;
            int nx = 0;
            for (int e = 0; e < ne; e++) {
                int32_t y0 = edges[e].y0, y1 = edges[e].y1;
                if (y0 == y1 || nx >= MAX_EDGES) {
                    continue;
                }
                if (sample_y >= y0 && sample_y < y1) {
                    int32_t x = edges[e].x0 + (int32_t)(((int64_t)(edges[e].x1 - edges[e].x0) * (sample_y - y0)) / (y1 - y0));
                    xs[nx] = x; dirs[nx] = 1; nx++;
                } else if (sample_y >= y1 && sample_y < y0) {
                    int32_t x = edges[e].x1 + (int32_t)(((int64_t)(edges[e].x0 - edges[e].x1) * (sample_y - y1)) / (y0 - y1));
                    xs[nx] = x; dirs[nx] = -1; nx++;
                }
            }

            /* Insertion sort by x — nx is small (a handful of crossings
               per scanline for Latin letterforms), so O(n^2) is fine. */
            for (int a = 1; a < nx; a++) {
                int32_t xv = xs[a];
                int8_t dv = dirs[a];
                int b = a - 1;
                while (b >= 0 && xs[b] > xv) {
                    xs[b + 1] = xs[b];
                    dirs[b + 1] = dirs[b];
                    b--;
                }
                xs[b + 1] = xv;
                dirs[b + 1] = dv;
            }

            int wind = 0;
            int32_t span_start = 0;
            for (int k = 0; k < nx; k++) {
                int prev = wind;
                wind += dirs[k];
                if (prev == 0 && wind != 0) {
                    span_start = xs[k];
                } else if (prev != 0 && wind == 0) {
                    int32_t a = span_start < 0 ? 0 : span_start;
                    int32_t maxb = (int32_t)px_w * FX_ONE;
                    int32_t b = xs[k] > maxb ? maxb : xs[k];
                    if (b > a) {
                        int pxa = a / FX_ONE;
                        int pxb = (b - 1) / FX_ONE;
                        if (pxa == pxb) {
                            if (pxa >= 0 && pxa < px_w) {
                                cov[py][pxa] = (uint16_t)(cov[py][pxa] + (b - a));
                            }
                        } else {
                            if (pxa >= 0 && pxa < px_w) {
                                cov[py][pxa] = (uint16_t)(cov[py][pxa] + (FX_ONE - (a - pxa * FX_ONE)));
                            }
                            for (int c = pxa + 1; c < pxb; c++) {
                                if (c >= 0 && c < px_w) {
                                    cov[py][c] = (uint16_t)(cov[py][c] + FX_ONE);
                                }
                            }
                            if (pxb >= 0 && pxb < px_w) {
                                cov[py][pxb] = (uint16_t)(cov[py][pxb] + (b - pxb * FX_ONE));
                            }
                        }
                    }
                }
            }
        }
    }
}

static uint32_t blend(uint32_t bg, uint32_t fg, uint8_t t) {
    int bg_r = (int)(bg >> 16) & 0xFF, bg_g = (int)(bg >> 8) & 0xFF, bg_b = (int)bg & 0xFF;
    int fg_r = (int)(fg >> 16) & 0xFF, fg_g = (int)(fg >> 8) & 0xFF, fg_b = (int)fg & 0xFF;
    int r = bg_r + ((fg_r - bg_r) * t) / 255;
    int g = bg_g + ((fg_g - bg_g) * t) / 255;
    int b = bg_b + ((fg_b - bg_b) * t) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void ttf_draw_glyph(const ttf_font_t *font, uint8_t *buf, uint32_t buf_w, uint32_t buf_h,
                     uint32_t x0, uint32_t y0, char ch, uint32_t fg, uint32_t bg, uint32_t px_size) {
    if (!font->loaded || px_size == 0) {
        return;
    }
    if (px_size > MAX_CELL - 8) {
        px_size = MAX_CELL - 8; /* keep the static coverage buffer safely bounded */
    }

    uint16_t gid = cmap_lookup(font, (uint32_t)(unsigned char)ch);
    uint32_t adv_gid = gid ? gid : cmap_lookup(font, 'M');
    uint32_t adv_units = advance_width_units(font, (uint16_t)adv_gid);
    uint32_t cell_w = (uint32_t)(font_to_px((int32_t)adv_units, px_size, font->units_per_em) / FX_ONE);
    if (cell_w == 0 || cell_w > MAX_CELL) {
        cell_w = px_size / 2 + 1;
    }
    uint32_t cell_h = ttf_line_height(font, px_size);
    if (cell_h == 0 || cell_h > MAX_CELL) {
        cell_h = px_size + px_size / 2;
    }

    /* Background always painted over the full advance cell, even for
       unmapped/space characters — matches the old bitmap font's opaque
       cell behavior that callers (console, chrome titles) rely on for
       correct highlighting/erasing. */
    fb_buf_fill_rect(buf, buf_w, buf_h, x0, y0, cell_w, cell_h, bg);

    if (gid == 0) {
        return;
    }

    uint32_t off, len;
    glyph_range(font, gid, &off, &len);
    if (len == 0 || off + len > font->size) {
        return; /* space glyph (empty outline) or out-of-range: nothing to draw */
    }

    static outline_t ol;
    if (parse_simple_glyph(font->data + off, len, &ol) != 0) {
        return; /* composite or malformed glyph: skip rather than misrender */
    }

    static edge_t edges[MAX_EDGES];
    int32_t baseline_y_fx = font_to_px(font->ascender, px_size, font->units_per_em);
    int ne = build_edges(&ol, px_size, font->units_per_em, 0, baseline_y_fx, edges);

    static uint16_t cov[MAX_CELL][MAX_CELL];
    rasterize(edges, ne, cov, (int)cell_w, (int)cell_h);

    uint16_t maxcov = (uint16_t)(FX_ONE * SUBSAMPLES);
    for (uint32_t yy = 0; yy < cell_h; yy++) {
        for (uint32_t xx = 0; xx < cell_w; xx++) {
            uint16_t c = cov[yy][xx];
            if (c == 0) {
                continue;
            }
            uint32_t alpha = (c >= maxcov) ? 255u : ((uint32_t)c * 255u / maxcov);
            fb_buf_put_pixel(buf, buf_w, buf_h, x0 + xx, y0 + yy, blend(bg, fg, (uint8_t)alpha));
        }
    }
}

uint32_t ttf_advance_width(const ttf_font_t *font, uint32_t px_size) {
    if (!font->loaded) {
        return 0;
    }
    uint16_t gid = cmap_lookup(font, 'M');
    uint32_t adv_units = advance_width_units(font, gid);
    return (uint32_t)(font_to_px((int32_t)adv_units, px_size, font->units_per_em) / FX_ONE);
}

uint32_t ttf_line_height(const ttf_font_t *font, uint32_t px_size) {
    if (!font->loaded) {
        return 0;
    }
    int32_t total_units = (int32_t)font->ascender - (int32_t)font->descender;
    return (uint32_t)(font_to_px(total_units, px_size, font->units_per_em) / FX_ONE) + 1;
}

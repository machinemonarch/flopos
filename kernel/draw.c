/* ===========================================================================
 * draw.c -- text and primitives on an 8-bit buffer.
 *
 * All of it clips against the 320x200 buffer.  The terminal has its own
 * tighter glyph blitter in term.c; what is here is for everything drawn at
 * arbitrary coordinates, where a stray pixel would otherwise be a fault.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

/* The glyph slots are published by fontmap.h; what maps onto them is here. */
int glyph_index(int c)
{
    c &= 0xFF;

    if (c >= 32 && c <= 95)  return c - 32;
    if (c >= 'a' && c <= 'z') return G_LOWER_BASE + (c - 'a');

    switch (c) {
    case CH_ARING:   return G_A_RING;
    case CH_ADIA:    return G_A_DIAERESIS;
    case CH_ODIA:    return G_O_DIAERESIS;
    case CH_ARING_L: return G_LOWER_ARING;
    case CH_ADIA_L:  return G_LOWER_ADIA;
    case CH_ODIA_L:  return G_LOWER_ODIA;
    case CH_BLOCK:   return G_SOLID_BLOCK;
    case CH_SOFT:    return G_SOFT_BLOCK;
    case CH_CARET:   return G_LEFT_CARET;
    case CH_NOODLER: return G_NOODLER;
    case '{':        return G_BRACE_OPEN;
    case '}':        return G_BRACE_CLOSE;
    case '|':        return G_PIPE;
    case '~':        return G_TILDE;
    case '`':        return G_BACKTICK;
    default:         return '?' - 32;
    }
}

void draw_char(u8 *buf, int x, int y, int c, u8 colour)
{
    const u8 *g = g_font5x8[glyph_index(c)];
    int col, row;

    for (col = 0; col < GLYPH_W; col++) {
        int px = x + col;
        u8 bits = g[col];
        if (!bits) continue;
        if (px < 0 || px >= SCR_W) continue;
        for (row = 0; row < GLYPH_H; row++) {
            int py = y + row;
            if (!(bits & (1 << row))) continue;
            if (py < 0 || py >= SCR_H) continue;
            buf[py * SCR_W + px] = colour;
        }
    }
}

void draw_text(u8 *buf, int x, int y, const char *s, u8 colour)
{
    while (*s) {
        draw_char(buf, x, y, (u8)*s++, colour);
        x += GLYPH_ADV;
        if (x >= SCR_W) break;
    }
}

/* A one-pixel drop shadow is the difference between readable and not, when
 * the thing behind the text is a moving field of colour. */
void draw_text_shadow(u8 *buf, int x, int y, const char *s, u8 fg, u8 bg)
{
    draw_text(buf, x + 1, y + 1, s, bg);
    draw_text(buf, x, y, s, fg);
}

/* A shadow on all four sides, not just below.  Over a photograph there is
 * no telling what is behind any given pixel, and a one-sided shadow leaves
 * half the strokes sitting on something the same brightness as they are. */
void draw_text_outline(u8 *buf, int x, int y, const char *s, u8 fg, u8 bg)
{
    draw_text(buf, x - 1, y, s, bg);
    draw_text(buf, x + 1, y, s, bg);
    draw_text(buf, x, y - 1, s, bg);
    draw_text(buf, x, y + 1, s, bg);
    draw_text(buf, x, y, s, fg);
}

void draw_char_outline(u8 *buf, int x, int y, int c, u8 fg, u8 bg)
{
    draw_char(buf, x - 1, y, c, bg);
    draw_char(buf, x + 1, y, c, bg);
    draw_char(buf, x, y - 1, c, bg);
    draw_char(buf, x, y + 1, c, bg);
    draw_char(buf, x, y, c, fg);
}

int text_width(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n * GLYPH_ADV - 1;
}

void draw_hline(u8 *buf, int x, int y, int w, u8 colour)
{
    if (y < 0 || y >= SCR_H) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (w <= 0) return;
    memset8(buf + y * SCR_W + x, colour, (usize)w);
}

void draw_vline(u8 *buf, int x, int y, int h, u8 colour)
{
    int i;
    if (x < 0 || x >= SCR_W) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > SCR_H) h = SCR_H - y;
    for (i = 0; i < h; i++) buf[(y + i) * SCR_W + x] = colour;
}

/* Bresenham, clipped to a box rather than to the screen, so a caller can
 * keep something inside its own corner without checking every point. */
void draw_line_clipped(u8 *buf, int x0, int y0, int x1, int y1, u8 colour,
                       int bx, int by, int bw, int bh)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err, n;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    err = dx - dy;
    n = (dx > dy ? dx : dy) + 1;
    if (n > 512) return;                 /* nothing sane is this long      */

    while (n--) {
        if (x0 >= bx && x0 < bx + bw && y0 >= by && y0 < by + bh &&
            x0 >= 0 && x0 < SCR_W && y0 >= 0 && y0 < SCR_H)
            buf[y0 * SCR_W + x0] = colour;
        {
            int e2 = err * 2;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 <  dx) { err += dx; y0 += sy; }
        }
    }
}

void draw_rect(u8 *buf, int x, int y, int w, int h, u8 colour)
{
    int i;
    for (i = 0; i < h; i++) draw_hline(buf, x, y + i, w, colour);
}

void draw_frame(u8 *buf, int x, int y, int w, int h, u8 colour)
{
    draw_hline(buf, x, y, w, colour);
    draw_hline(buf, x, y + h - 1, w, colour);
    draw_vline(buf, x, y, h, colour);
    draw_vline(buf, x + w - 1, y, h, colour);
}

/* Pull a region's palette indices down, which on every ramp reads as
 * "fade towards black".  Used to seat a panel over a live background. */
void draw_shade_box(u8 *buf, int x, int y, int w, int h, u8 sub)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    for (j = 0; j < h; j++) {
        u8 *p = buf + (y + j) * SCR_W + x;
        for (i = 0; i < w; i++) {
            int v = p[i] - sub;
            p[i] = (u8)(v < 0 ? 0 : v);
        }
    }
}

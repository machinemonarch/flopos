/* ===========================================================================
 * kbpick.c -- the way home: Ctrl+Shift+F1, on any keyboard, anywhere.
 *
 * Pressed with the wrong keyboard chosen, a typed command may be a puzzle:
 * on a Hebrew or a French keyboard "keyboard se" is not where it was.  The
 * F keys are in the same place on every keyboard, so Ctrl+Shift+F1 opens a
 * small window over whatever is on the screen, an app's included, listing
 * every keyboard with Swedish at the top.  The arrows pick, Enter chooses
 * (and keeps the choice, as `keyboard` does), Esc leaves it as it was.
 * ===========================================================================
 */
#include "flopos.h"

volatile int g_kbpick_open;              /* Ctrl+Shift+F1, from the interrupt */

static int s_open, s_sel, s_drawn;
static int s_px, s_py, s_w, s_h, s_cw, s_ch;

int kbpick_takes_keys(void) { return s_open; }

static void close_it(void)
{
    s_open = 0;
    term_refresh();                      /* what was under it, drawn again */
}

void kbpick_key(int ev)
{
    int sc = KEV_SCAN(ev), n = keymap_count();
    if (sc == SC_UP)   { s_sel = (s_sel + n - 1) % n; return; }
    if (sc == SC_DOWN) { s_sel = (s_sel + 1) % n; return; }
    if (sc == SC_ESC)  { close_it(); return; }
    if (sc == SC_ENTER) {
        keymap_set(keymap_code_at(s_sel));
        store_keys_set(keymap_code());
        if (serial_present()) serial_printf("[keyboard] %s\n", keymap_code());
        close_it();
    }
}

static void put(u8 *dst, int cx, int cy, int c, u8 fg, u8 bg)
{
    term_overlay_cell(dst, s_px + cx * s_cw, s_py + cy * s_ch, c, fg, bg);
}

static void text(u8 *dst, int cx, int cy, const char *t, u8 fg, u8 bg)
{
    int c, key = 0;
    while ((c = ch_next(&t)) != 0 && cx < s_w - 1) {
        if (c == 1) { key = !key; continue; }
        put(dst, cx++, cy, c, key ? C_KEY : fg, bg);
    }
}

void kbpick_frame(u8 *dst, u32 t)
{
    int n = keymap_count(), i, x, y, top;
    (void)t;
    s_drawn = 0;
    if (g_kbpick_open) {
        int k;
        g_kbpick_open = 0;
        s_open = 1;
        s_sel = 0;
        for (k = 0; k < n; k++) if (keymap_code_at(k) == keymap_code()) s_sel = k;
    }
    if (!s_open) return;

    term_cell_size(&s_cw, &s_ch);
    s_w = 26;
    s_h = n + 4;
    if (s_h * s_ch > vga_height()) s_h = vga_height() / s_ch;
    s_px = ((vga_width() - s_w * s_cw) / 2) & ~3;
    s_py = (vga_height() - s_h * s_ch) / 2;
    if (s_px < 0) s_px = 0;
    if (s_py < 0) s_py = 0;

    for (y = 0; y < s_h; y++)
        for (x = 0; x < s_w; x++) {
            int c = ' ', tp = y == 0, bt = y == s_h - 1, l = x == 0, r = x == s_w - 1;
            if ((tp || bt) && !l && !r) c = FR_H;
            else if ((l || r) && !tp && !bt) c = FR_V;
            else if (tp && l) c = FR_TL;
            else if (tp && r) c = FR_TR;
            else if (bt && l) c = FR_BL;
            else if (bt && r) c = FR_BR;
            put(dst, x, y, c, C_GREY, WIN_PAPER);
        }
    text(dst, 2, 0, " keyboard ", C_BRIGHT, WIN_PAPER);

    /* As many as fit, the chosen one always among them. */
    {
        int rows = s_h - 4;
        top = s_sel - rows + 1;
        if (top < 0) top = 0;
        for (i = 0; i < rows && top + i < n; i++) {
            int k = top + i, sel = k == s_sel;
            u8 bg = sel ? WIN_HILITE : WIN_PAPER;
            for (x = 1; x < s_w - 1; x++) put(dst, x, 1 + i, ' ', C_TEXT, bg);
            text(dst, 2, 1 + i, keymap_code_at(k), sel ? C_BRIGHT : C_ACCENT, bg);
            text(dst, 6, 1 + i, keymap_name_at(k), sel ? C_BRIGHT : C_TEXT, bg);
        }
    }
    text(dst, 2, s_h - 2, "\1up\1 \1down\1 \1enter\1 \1esc\1", C_DIM, WIN_PAPER);
    s_drawn = 1;
}

/* The rectangle it drew, for sending to the card: 0 if none. */
int kbpick_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (!s_drawn) return 0;
    *x0 = s_px; *y0 = s_py;
    *x1 = s_px + s_w * s_cw - 1; *y1 = s_py + s_h * s_ch - 1;
    return 1;
}

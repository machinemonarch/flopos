/* ===========================================================================
 * clockui.c -- the clock, as a menu (like netui.c).
 *
 * What time it is, where the time last came from, and how far to trust
 * the network for it (nts.c): signed only; signed, then the time server
 * the local network names; or signed, then any.  The three signed (NTS)
 * servers are asked in order and can be typed over.  Changes are written
 * to STATE.BIN at once.  Tab goes round, Enter changes or edits, Esc leaves.
 * ===========================================================================
 */
#include "flopos.h"

#define W 54
#define H 23
#define WIN_TEXT 134

enum { R_TRUST, R_S1, R_S2, R_S3, B_SET, B_CLOSE, NSEL };

static const char *s_trust_text[3] = {
    "signed only", "signed, then the network's", "signed, then any"
};

static int  s_trust;
static char s_srv[3][64];
static int  s_sel, s_edit = -1, s_len;
static char s_buf[64];
static char s_status[96];
static u8   s_status_col;

static void status(const char *t, u8 col)
{
    fmt_str(s_status, sizeof s_status, "%s", t);
    s_status_col = col;
}

static void keep(void)
{
    if (nts_settings(s_trust, (const char (*)[64])s_srv)) status("Written down for next time.", C_DIM);
    else status("The disk would not keep it.", C_ACCENT);
}

/* Text from column x, wrapped at spaces to fit inside the frame of a
 * window ww wide, each row banded in bg when band is set.  Returns the
 * rows used (at least one). */
static int wrap(int ww, int x, int y, const char *t, u8 fg, u8 bg, int band)
{
    char row[WIN_TEXT];
    int room = ww - 2 - x, rows = 0;
    if (room < 8) room = 8;
    do {
        int n = 0, cut;
        while (t[n]) n++;
        cut = n;
        if (n > room) {
            for (cut = room; cut > 0 && t[cut] != ' '; cut--) { }
            if (!cut) cut = room;                    /* one long word: cut it */
        }
        if (cut > (int)sizeof row - 1) cut = (int)sizeof row - 1;
        { int k; for (k = 0; k < cut; k++) row[k] = t[k]; } row[cut] = 0;
        if (band) term_win_band(y + rows, 2, ww - 3, bg);
        term_win_text(x, y + rows, row, fg, bg);
        rows++;
        t += cut;
        while (*t == ' ') t++;
    } while (*t);
    return rows;
}

static void draw(void)
{
    char line[96];
    u8 h, mi, se, mo, da;
    u16 yr;
    u8 hl = WIN_HILITE, pp = WIN_PAPER;
    int i, y, ww;

    ww = term_win_open(W, H);
    term_win_frame("clock", C_GREY);

    term_win_text(3, 2, "now", C_DIM, pp);
    if (rtc_read(&h, &mi, &se, &yr, &mo, &da))
        fmt_str(line, sizeof line, "%02u:%02u:%02u  %02u-%02u-%u", h, mi, se, da, mo, yr);
    else fmt_str(line, sizeof line, "the clock cannot be read");
    term_win_text(14, 2, line, C_TEXT, pp);
    term_win_text(3, 3, "last set", C_DIM, pp);
    y = 3 + wrap(ww, 14, 3, nts_source()[0] ? nts_source() : "not yet, this time", C_TEXT, pp, 0);

    y++;
    if (s_sel == R_TRUST) term_win_band(y, 2, ww - 3, hl);
    term_win_text(3, y, s_sel == R_TRUST ? ">" : " ", C_BRIGHT, s_sel == R_TRUST ? hl : pp);
    term_win_text(5, y, "trust", C_DIM, s_sel == R_TRUST ? hl : pp);
    term_win_text(14, y, s_trust_text[s_trust], C_BRIGHT, s_sel == R_TRUST ? hl : pp);
    y++;

    for (i = 0; i < 3; i++) {
        int sel = s_sel == R_S1 + i;
        u8 bg = sel ? hl : pp;
        if (sel) term_win_band(y, 2, ww - 3, hl);
        term_win_text(3, y, sel ? ">" : " ", C_BRIGHT, bg);
        if (i == 0) term_win_text(5, y, "signed", C_DIM, bg);
        if (s_edit == i) {                           /* the end of it, where the typing is */
            int room = ww - 2 - 14 - 1, at = s_len > room ? s_len - room : 0, x;
            x = term_win_text(14, y, s_buf + at, C_BRIGHT, bg);
            term_win_put(x, y, '_', C_ACCENT, bg);
            y++;
        } else y += wrap(ww, 14, y, s_srv[i][0] ? s_srv[i] : "-", C_TEXT, bg, sel);
    }

    term_win_text(5, y, "plain", C_DIM, pp);
    if (s_trust == 0) fmt_str(line, sizeof line, "never");
    else {
        u32 a = ip_ntp_server();
        if (a) fmt_str(line, sizeof line, "the network's own: %u.%u.%u.%u%s", a >> 24, (a >> 16) & 255,
                       (a >> 8) & 255, a & 255, s_trust == 2 ? ", then any" : "");
        else fmt_str(line, sizeof line, "the network's own (none named)%s", s_trust == 2 ? ", then any" : "");
    }
    y += wrap(ww, 14, y, line, C_TEXT, pp, 0);

    {
        static const char *names[2] = { "set it now", "close" };
        static const int at[2] = { 5, 20 };
        y++;
        for (i = 0; i < 2; i++) {
            int sel = s_sel == B_SET + i;
            u8 bg = sel ? hl : pp;
            int x = at[i];
            term_win_put(x - 1, y, sel ? '[' : ' ', C_BRIGHT, bg);
            x = term_win_text(x, y, names[i], sel ? C_BRIGHT : C_TEXT, bg);
            term_win_put(x, y, sel ? ']' : ' ', C_BRIGHT, bg);
        }
        y += 2;
    }
    if (s_status[0]) wrap(ww, 3, y, s_status, s_status_col, pp, 0);
    term_win_text(3, H - 3, s_edit >= 0 ? "type it, \1enter\1 keeps it, \1esc\1 undoes"
                                        : "\1tab\1 move   \1enter\1 change   \1esc\1 leave", C_DIM, pp);
}

static void set_now(void)
{
    status("Asking...", C_DIM);
    draw();
    main_pump(1);
    if (!net_online()) { status("No network.", C_ACCENT); return; }
    if (clock_from_network()) status("Set.", C_BRIGHT);
    else status(nts_source()[0] ? nts_source() : "Nothing answered.", C_ACCENT);
}

void clockui_run(void)
{
    int i, k;

    s_trust = nts_trust();
    for (i = 0; i < 3; i++) fmt_str(s_srv[i], sizeof s_srv[i], "%s", nts_server(i));
    s_sel = R_TRUST;
    s_edit = -1;
    s_status[0] = 0;

    for (;;) {
        draw();
        k = ui_key();
        if (k < 0) { main_pump(0); continue; }

        if (s_edit >= 0) {
            if (k == UK_ENTER || k == UK_TAB || k == UK_BTAB) {
                for (i = 0; i <= s_len; i++) s_srv[s_edit][i] = s_buf[i];
                s_edit = -1;
                keep();
                if (k == UK_TAB) s_sel = (s_sel + 1) % NSEL;
                if (k == UK_BTAB) s_sel = (s_sel + NSEL - 1) % NSEL;
            } else if (k == UK_ESC) {
                s_edit = -1;
            } else if (k == UK_BKSP) {
                if (s_len) s_buf[--s_len] = 0;
            } else if (((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') ||
                        k == '.' || k == '-') && s_len < 63) {
                s_buf[s_len++] = (char)(k >= 'A' && k <= 'Z' ? k + 32 : k);
                s_buf[s_len] = 0;
            }
            continue;
        }

        switch (k) {
        case UK_ESC:
            term_win_close();
            return;
        case UK_TAB: case UK_DOWN:
            s_sel = (s_sel + 1) % NSEL;
            break;
        case UK_BTAB: case UK_UP:
            s_sel = (s_sel + NSEL - 1) % NSEL;
            break;
        case UK_LEFT:
            if (s_sel == B_CLOSE) s_sel = B_SET;
            break;
        case UK_RIGHT:
            if (s_sel == B_SET) s_sel = B_CLOSE;
            break;
        case ' ':
        case UK_ENTER:
            if (s_sel == R_TRUST) { s_trust = (s_trust + 1) % 3; keep(); }
            else if (s_sel <= R_S3) {
                int e = s_sel - R_S1;
                fmt_str(s_buf, sizeof s_buf, "%s", s_srv[e]);
                for (s_len = 0; s_buf[s_len]; s_len++) { }
                s_edit = e;
                s_status[0] = 0;
            } else if (s_sel == B_SET) set_now();
            else { term_win_close(); return; }
            break;
        default:
            break;
        }
    }
}

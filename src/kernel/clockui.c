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
#define H 18

enum { R_TRUST, R_S1, R_S2, R_S3, B_SET, B_CLOSE, NSEL };

static const char *s_trust_text[3] = {
    "signed only", "signed, then the network's", "signed, then any"
};

static int  s_trust;
static char s_srv[3][64];
static int  s_sel, s_edit = -1, s_len;
static char s_buf[64];
static char s_status[52];
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

static void draw(void)
{
    char line[64];
    u8 h, mi, se, mo, da;
    u16 yr;
    u8 hl = WIN_HILITE, pp = WIN_PAPER;
    int i, y;

    term_win_open(W, H);
    term_win_frame("clock", C_GREY);

    term_win_text(3, 2, "now", C_DIM, pp);
    if (rtc_read(&h, &mi, &se, &yr, &mo, &da))
        fmt_str(line, sizeof line, "%02u:%02u:%02u  %02u-%02u-%u", h, mi, se, da, mo, yr);
    else fmt_str(line, sizeof line, "the clock cannot be read");
    term_win_text(14, 2, line, C_TEXT, pp);
    term_win_text(3, 3, "last set", C_DIM, pp);
    term_win_text(14, 3, nts_source()[0] ? nts_source() : "not yet, this time", C_TEXT, pp);

    y = 5;
    if (s_sel == R_TRUST) term_win_band(y, 2, W - 3, hl);
    term_win_text(3, y, s_sel == R_TRUST ? ">" : " ", C_BRIGHT, s_sel == R_TRUST ? hl : pp);
    term_win_text(5, y, "trust", C_DIM, s_sel == R_TRUST ? hl : pp);
    term_win_text(14, y, s_trust_text[s_trust], C_BRIGHT, s_sel == R_TRUST ? hl : pp);

    for (i = 0; i < 3; i++) {
        int sel = s_sel == R_S1 + i;
        u8 bg = sel ? hl : pp;
        y = 6 + i;
        if (sel) term_win_band(y, 2, W - 3, hl);
        term_win_text(3, y, sel ? ">" : " ", C_BRIGHT, bg);
        if (i == 0) term_win_text(5, y, "signed", C_DIM, bg);
        if (s_edit == i) {
            int x = term_win_text(14, y, s_buf, C_BRIGHT, bg);
            term_win_put(x, y, '_', C_ACCENT, bg);
        } else term_win_text(14, y, s_srv[i][0] ? s_srv[i] : "-", C_TEXT, bg);
    }

    term_win_text(5, 9, "plain", C_DIM, pp);
    if (s_trust == 0) fmt_str(line, sizeof line, "never");
    else {
        u32 a = ip_ntp_server();
        if (a) fmt_str(line, sizeof line, "the network's own: %u.%u.%u.%u%s", a >> 24, (a >> 16) & 255,
                       (a >> 8) & 255, a & 255, s_trust == 2 ? ", then any" : "");
        else fmt_str(line, sizeof line, "the network's own (none named)%s", s_trust == 2 ? ", then any" : "");
    }
    term_win_text(14, 9, line, C_TEXT, pp);

    {
        static const char *names[2] = { "set it now", "close" };
        static const int at[2] = { 5, 20 };
        y = 11;
        for (i = 0; i < 2; i++) {
            int sel = s_sel == B_SET + i;
            u8 bg = sel ? hl : pp;
            int x = at[i];
            term_win_put(x - 1, y, sel ? '[' : ' ', C_BRIGHT, bg);
            x = term_win_text(x, y, names[i], sel ? C_BRIGHT : C_TEXT, bg);
            term_win_put(x, y, sel ? ']' : ' ', C_BRIGHT, bg);
        }
    }
    if (s_status[0]) term_win_text(3, 13, s_status, s_status_col, pp);
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

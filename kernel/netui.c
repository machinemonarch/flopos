/* ===========================================================================
 * netui.c -- the network, as a menu.
 *
 * Everything `ip` and `net` do, in a window: what card there is and
 * whether anything is plugged into it, where this machine is, and the
 * choice between asking the network for all that and setting it by hand.
 * Settings made by hand are written to STATE.BIN, so they are still there
 * after the machine has been switched off.
 *
 * Up and down move, Enter changes, Escape leaves.  Over the serial line
 * the arrow keys work too, and Tab moves down.
 * ===========================================================================
 */
#include "flopos.h"

#define W 48
#define H 18

enum { R_HOW, R_ADDR, R_MASK, R_GW, R_DNS, B_APPLY, B_ASK, B_CLOSE, NSEL };

static const char *s_label[4] = { "address", "mask", "way out", "names" };

static int  s_how;                       /* 0 automatic, 1 by hand         */
static char s_field[4][16];
static int  s_sel, s_edit = -1;
static char s_buf[16];
static int  s_len;
static char s_status[48];
static u8   s_status_col;

static void ip_text(char *out, u32 a)
{
    if (!a) { out[0] = 0; return; }
    fmt_str(out, 16, "%u.%u.%u.%u", (a >> 24) & 0xFF, (a >> 16) & 0xFF,
            (a >> 8) & 0xFF, a & 0xFF);
}

static void status(const char *t, u8 col)
{
    int i;
    for (i = 0; t[i] && i < (int)sizeof s_status - 1; i++) s_status[i] = t[i];
    s_status[i] = 0;
    s_status_col = col;
}

/* What the machine is using now, or failing that what was saved. */
static void load_fields(void)
{
    u32 how, a, m, g, d;

    if (ip_addr()) {
        ip_text(s_field[0], ip_addr());
        ip_text(s_field[1], ip_netmask());
        ip_text(s_field[2], ip_gateway());
        ip_text(s_field[3], ip_dns());
        s_how = (ip_how() == 1);
    } else if (store_net_get(&how, &a, &m, &g, &d)) {
        ip_text(s_field[0], a); ip_text(s_field[1], m);
        ip_text(s_field[2], g); ip_text(s_field[3], d);
        s_how = (how == 1);
    } else {
        s_field[0][0] = s_field[1][0] = s_field[2][0] = s_field[3][0] = 0;
        s_how = 0;
    }
}

static void draw(void)
{
    char line[48];
    int i, y;
    u8 hl = WIN_HILITE, pp = WIN_PAPER;

    term_win_open(W, H);
    term_win_frame("network", C_GREY);

    /* What there is. */
    term_win_text(3, 2, "card", C_DIM, pp);
    if (net_ready()) {
        const u8 *m = net_mac();
        fmt_str(line, sizeof line, "%s  %02x:%02x:%02x:%02x:%02x:%02x",
                net_name(), m[0], m[1], m[2], m[3], m[4], m[5]);
        term_win_text(12, 2, line, C_TEXT, pp);
        term_win_text(3, 3, "link", C_DIM, pp);
        term_win_text(12, 3, net_link() ? "yes" : "no -- is the cable in?",
                      net_link() ? C_TEXT : C_ACCENT, pp);
    } else {
        term_win_text(12, 2, "not started", C_DIM, pp);
    }

    /* How, and where. */
    y = 5;
    if (s_sel == R_HOW) term_win_band(y, 2, W - 3, hl);
    term_win_text(3, y, s_sel == R_HOW ? ">" : " ", C_BRIGHT, s_sel == R_HOW ? hl : pp);
    term_win_text(5, y, "setting", C_DIM, s_sel == R_HOW ? hl : pp);
    term_win_text(14, y, s_how ? "by hand" : "automatic, from the network",
                  C_BRIGHT, s_sel == R_HOW ? hl : pp);

    for (i = 0; i < 4; i++) {
        int sel = (s_sel == R_ADDR + i);
        u8 bg = sel ? hl : pp;
        y = 6 + i;
        if (sel) term_win_band(y, 2, W - 3, hl);
        term_win_text(3, y, sel ? ">" : " ", C_BRIGHT, bg);
        term_win_text(5, y, s_label[i], C_DIM, bg);
        if (s_edit == i) {
            int x = term_win_text(14, y, s_buf, C_BRIGHT, bg);
            term_win_put(x, y, '_', C_ACCENT, bg);
        } else {
            term_win_text(14, y, s_field[i][0] ? s_field[i] : "-",
                          s_how ? C_TEXT : C_DIM, bg);
        }
    }

    /* The three things to do. */
    {
        static const char *names[3] = { "apply", "ask again", "close" };
        static const int at[3] = { 5, 15, 29 };
        y = 11;
        for (i = 0; i < 3; i++) {
            int sel = (s_sel == B_APPLY + i);
            u8 bg = sel ? hl : pp;
            int x = at[i];
            term_win_put(x - 1, y, sel ? '[' : ' ', C_BRIGHT, bg);
            x = term_win_text(x, y, names[i], sel ? C_BRIGHT : C_TEXT, bg);
            term_win_put(x, y, sel ? ']' : ' ', C_BRIGHT, bg);
        }
    }

    if (s_status[0]) term_win_text(3, 13, s_status, s_status_col, pp);

    term_win_text(3, H - 3, s_edit >= 0 ? "type the address, enter to keep it, esc to undo"
                                        : "up down move   enter change   esc leave",
                  C_DIM, pp);
}

/* Put the window up and draw a frame now, before something slow. */
static void show_now(void)
{
    draw();
    main_pump(1);
}

static void start_edit(int i)
{
    int k;
    if (!s_how) {
        status("Set this to by hand first to type these.", C_ACCENT);
        return;
    }
    for (k = 0; s_field[i][k]; k++) s_buf[k] = s_field[i][k];
    s_buf[k] = 0;
    s_len = k;
    s_edit = i;
    s_status[0] = 0;
}

static void ask_network(void)
{
    status("Asking the network for an address...", C_DIM);
    show_now();
    if (!net_up()) { status("No network card came up.", C_ACCENT); return; }
    ip_configure(0, 0, 0);
    if (ip_dhcp()) {
        load_fields();
        status("Done. That came from the network.", C_BRIGHT);
    } else {
        status(net_link() ? "Nobody answered. Try by hand."
                          : "Nothing is plugged into the card.", C_ACCENT);
    }
    store_net_set(0, 0, 0, 0, 0);
}

static void apply(void)
{
    u32 v[4];
    int i;

    if (!s_how) { ask_network(); return; }

    for (i = 0; i < 4; i++) {
        v[i] = 0;
        if (i == 3 && !s_field[3][0]) continue;        /* names: optional */
        if (!ip_parse(s_field[i], &v[i])) {
            status(i == 3 ? "The names address does not read right."
                          : "That one does not read as an address.", C_ACCENT);
            s_sel = R_ADDR + i;
            return;
        }
    }

    status("Starting the card...", C_DIM);
    show_now();
    (void)net_up();
    ip_configure(v[0], v[1], v[2]);
    ip_set_dns(v[3], 53);
    if (store_net_set(1, v[0], v[1], v[2], v[3]))
        status("Set, and written down for next time.", C_BRIGHT);
    else
        status("Set. The disk would not keep it.", C_ACCENT);
}

void netui_run(void)
{
    int k;

    s_sel = R_HOW;
    s_edit = -1;
    s_status[0] = 0;
    load_fields();

    if (!net_ready()) {
        status("Starting the card...", C_DIM);
        show_now();
        if (net_up()) s_status[0] = 0;
        else status("No network card came up.", C_ACCENT);
    }

    for (;;) {
        draw();
        k = ui_key();
        if (k < 0) { main_pump(0); continue; }

        if (s_edit >= 0) {
            if (k == UK_ENTER) {
                u32 a;
                s_buf[s_len] = 0;
                if (s_len && !ip_parse(s_buf, &a)) {
                    status("Four numbers and three dots, please.", C_ACCENT);
                    continue;
                }
                for (k = 0; k <= s_len; k++) s_field[s_edit][k] = s_buf[k];
                s_edit = -1;
                s_status[0] = 0;
            } else if (k == UK_ESC) {
                s_edit = -1;
            } else if (k == UK_BKSP) {
                if (s_len) s_buf[--s_len] = 0;
            } else if (((k >= '0' && k <= '9') || k == '.') && s_len < 15) {
                s_buf[s_len++] = (char)k;
                s_buf[s_len] = 0;
            }
            continue;
        }

        switch (k) {
        case UK_ESC:
            term_win_close();
            return;
        case UK_BTAB:
        case UK_UP:
            if (s_sel >= B_APPLY) s_sel = R_DNS;
            else if (s_sel > 0) s_sel--;
            break;
        case UK_TAB:
        case UK_DOWN:
            if (s_sel < R_DNS) s_sel++;
            else if (s_sel == R_DNS) s_sel = B_APPLY;
            break;
        case UK_LEFT:
            if (s_sel > B_APPLY) s_sel--;
            break;
        case UK_RIGHT:
            if (s_sel >= B_APPLY && s_sel < B_CLOSE) s_sel++;
            break;
        case ' ':
        case UK_ENTER:
            if (s_sel == R_HOW) { s_how = !s_how; s_status[0] = 0; }
            else if (s_sel <= R_DNS) start_edit(s_sel - R_ADDR);
            else if (s_sel == B_APPLY) apply();
            else if (s_sel == B_ASK) { s_how = 0; ask_network(); }
            else { term_win_close(); return; }
            break;
        default:
            break;
        }
    }
}

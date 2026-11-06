/* ===========================================================================
 * line.c -- the line editor.
 *
 * Insert, delete either way, move by character or to either end, and a
 * history ring on the up and down arrows.  It knows nothing about the
 * terminal: it holds a string and a caret, and whoever owns the screen
 * asks for them.
 * ===========================================================================
 */
#include "flopos.h"

#define HIST_N 24

static char s_buf[LINE_MAX + 1];
static int  s_len, s_caret;

static char s_hist[HIST_N][LINE_MAX + 1];
static int  s_hist_count;                /* how many are filled           */
static int  s_hist_next;                 /* where the next one goes        */
static int  s_hist_pos;                  /* 0 = editing, 1.. = looking back */

void line_init(void)
{
    s_len = s_caret = 0;
    s_buf[0] = 0;
    s_hist_count = s_hist_next = s_hist_pos = 0;
}

void line_reset(void)
{
    s_len = s_caret = 0;
    s_buf[0] = 0;
    s_hist_pos = 0;
}

const char *line_text(void)  { return s_buf; }
int         line_len(void)   { return s_len; }
int         line_caret(void) { return s_caret; }

static void set_from(const char *s)
{
    int i = 0;
    while (s[i] && i < LINE_MAX) { s_buf[i] = s[i]; i++; }
    s_buf[i] = 0;
    s_len = i;
    s_caret = i;
}

static void history_push(const char *s)
{
    int i;
    if (!s[0]) return;

    /* Do not store the same thing twice in a row. */
    if (s_hist_count) {
        int last = (s_hist_next - 1 + HIST_N) % HIST_N;
        const char *p = s_hist[last];
        for (i = 0; p[i] == s[i]; i++) if (!s[i]) return;
    }

    for (i = 0; s[i] && i < LINE_MAX; i++) s_hist[s_hist_next][i] = s[i];
    s_hist[s_hist_next][i] = 0;
    s_hist_next = (s_hist_next + 1) % HIST_N;
    if (s_hist_count < HIST_N) s_hist_count++;
}

static void history_recall(int delta)
{
    int pos = s_hist_pos + delta;
    int idx;

    if (pos < 0) pos = 0;
    if (pos > s_hist_count) pos = s_hist_count;
    s_hist_pos = pos;

    if (pos == 0) { s_len = s_caret = 0; s_buf[0] = 0; return; }
    idx = (s_hist_next - pos + HIST_N * 2) % HIST_N;
    set_from(s_hist[idx]);
}

static void insert(int c)
{
    int i;
    if (s_len >= LINE_MAX) return;
    for (i = s_len; i > s_caret; i--) s_buf[i] = s_buf[i - 1];
    s_buf[s_caret] = (char)c;
    s_len++;
    s_caret++;
    s_buf[s_len] = 0;
}

static void erase_at(int at)
{
    int i;
    if (at < 0 || at >= s_len) return;
    for (i = at; i < s_len - 1; i++) s_buf[i] = s_buf[i + 1];
    s_len--;
    s_buf[s_len] = 0;
}

/* A character from somewhere that has no scancodes.  The far end of a
 * serial line sends CR for return and either backspace or delete for
 * rubout, depending on whose terminal it is, so accept all of them. */
int line_inject(int c)
{
    if (c == '\r' || c == '\n') {
        s_buf[s_len] = 0;
        history_push(s_buf);
        return 1;
    }
    if (c == 0x08 || c == 0x7F) {        /* backspace or delete            */
        if (s_caret > 0) { s_caret--; erase_at(s_caret); }
        return 0;
    }
    if (c == 0x15) {                     /* ^U                             */
        s_len = s_caret = 0;
        s_buf[0] = 0;
        return 0;
    }
    if (c == 0x1B) {                     /* escape: abandon the line       */
        s_len = s_caret = 0;
        s_buf[0] = 0;
        return 0;
    }
    if (c >= 32 && c < 127) insert(c);
    return 0;
}

int line_key(int event)
{
    int sc = KEV_SCAN(event);
    int c;

    switch (sc) {
    case SC_ENTER:
        s_buf[s_len] = 0;
        history_push(s_buf);
        return 1;

    case SC_BKSP:
        if (s_caret > 0) { s_caret--; erase_at(s_caret); }
        return 0;

    case SC_DEL:                         /* keypad or the real Delete key  */
        erase_at(s_caret);
        return 0;

    case SC_LEFT:
        if (s_caret > 0) s_caret--;
        return 0;

    case SC_RIGHT:
        if (s_caret < s_len) s_caret++;
        return 0;

    case SC_HOME:
        s_caret = 0;
        return 0;

    case SC_END:
        s_caret = s_len;
        return 0;

    case SC_UP:
        history_recall(+1);
        return 0;

    case SC_DOWN:
        history_recall(-1);
        return 0;

    case SC_ESC:
        s_len = s_caret = 0;
        s_buf[0] = 0;
        s_hist_pos = 0;
        return 0;

    default:
        break;
    }

    /* Control codes we honour, before the keymap gets a look in. */
    if (event & KEV_CTRL) {
        switch (sc) {
        case 0x16: s_len = s_caret = 0; s_buf[0] = 0; return 0;  /* ^U     */
        case 0x1E: s_caret = 0; return 0;                        /* ^A     */
        case 0x12: s_caret = s_len; return 0;                    /* ^E     */
        case 0x25: while (s_len > s_caret) erase_at(s_caret); return 0; /* ^K */
        default: return 0;
        }
    }

    c = keymap_char(event);
    if (c) insert(c);
    return 0;
}

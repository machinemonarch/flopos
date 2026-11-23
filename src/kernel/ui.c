/* ===========================================================================
 * ui.c -- keys for things that are not the command line.
 *
 * The line editor wants whole lines.  A menu or an editor wants single
 * keys, now, and sometimes which modifiers were held: Shift to select,
 * Ctrl for commands.  They come from the keyboard, or from the serial
 * line, where a terminal program spells special keys as escape sequences
 * -- ESC [ A for up, ESC [ 1 ; 2 C for Shift-Right, ESC [ 3 ~ for Delete --
 * and Ctrl-B as the single byte 2.  Telling a lone Escape from the start
 * of a sequence means waiting a moment after it to see what follows.
 * ===========================================================================
 */
#include "flopos.h"

static int serial_wait(void)
{
    u32 until = ms_now() + 40;
    int b;
    while ((b = serial_getc()) < 0 && (s32)(ms_now() - until) < 0) { }   /* safe across the 49-day wrap */
    return b;
}

static int from_serial(int *mods)
{
    int c = serial_getc(), b, p1 = 0, p2 = 0;

    if (c < 0) return -1;
    if (c == '\r' || c == '\n') return UK_ENTER;
    if (c == 8 || c == 127)     return UK_BKSP;
    if (c == '\t')              return UK_TAB;
    if (c >= 1 && c <= 26) { *mods |= KEV_CTRL; return 'a' + c - 1; }
    if (c != 27) return (c >= 32 && c < 127) ? c : -1;

    b = serial_wait();
    if (b != '[' && b != 'O') return UK_ESC;
    b = serial_wait();
    /* Numbers, then perhaps ";" and a modifier number, then a letter or ~. */
    while (b >= '0' && b <= '9') { p1 = p1 * 10 + (b - '0'); b = serial_wait(); }
    if (b == ';') {
        b = serial_wait();
        while (b >= '0' && b <= '9') { p2 = p2 * 10 + (b - '0'); b = serial_wait(); }
    }
    if (p2 == 2 || p2 == 4) *mods |= KEV_SHIFT;
    if (p2 == 5 || p2 == 6) *mods |= KEV_CTRL;
    if (p2 == 6) *mods |= KEV_SHIFT;
    switch (b) {
    case 'A': return UK_UP;
    case 'B': return UK_DOWN;
    case 'C': return UK_RIGHT;
    case 'D': return UK_LEFT;
    case 'H': return UK_HOME;
    case 'F': return UK_END;
    case 'Z': return UK_BTAB;
    case '~':
        switch (p1) {
        case 1: case 7: return UK_HOME;
        case 3: return UK_DEL;
        case 4: case 8: return UK_END;
        case 5: return UK_PGUP;
        case 6: return UK_PGDN;
        }
        return -1;
    }
    return -1;
}

/* A key and the modifiers held with it, or -1 if nothing is waiting.
 * Ctrl with a letter comes back as the letter and M_CTRL. */
int ui_key_mods(int *mods)
{
    int ev, c;

    *mods = 0;
    while ((ev = kbd_event()) >= 0) {
        *mods = ev & (KEV_SHIFT | KEV_CTRL | KEV_ALT);
        switch (KEV_SCAN(ev)) {
        case SC_UP:    return UK_UP;
        case SC_DOWN:  return UK_DOWN;
        case SC_LEFT:  return UK_LEFT;
        case SC_RIGHT: return UK_RIGHT;
        case SC_ENTER: return UK_ENTER;
        case SC_ESC:   return UK_ESC;
        case SC_BKSP:  return UK_BKSP;
        case SC_DEL:   return UK_DEL;
        case SC_TAB:   return (ev & KEV_SHIFT) ? UK_BTAB : UK_TAB;
        case SC_PGUP:  return UK_PGUP;
        case SC_PGDN:  return UK_PGDN;
        case SC_HOME:  return UK_HOME;
        case SC_END:   return UK_END;
        default: break;
        }
        if (ev & KEV_CTRL) {
            c = keymap_key(ev & ~KEV_CTRL);
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c > 32 && c < 127) return c;
            continue;
        }
        c = keymap_char(ev);
        if (c >= 32 && c != 127) { *mods &= ~KEV_SHIFT; return c; }
    }
    return from_serial(mods);
}

/* Without modifiers, for menus: a key pressed with Ctrl or Alt is not
 * one of theirs, so it is taken and ignored. */
int ui_key(void)
{
    int mods, k = ui_key_mods(&mods);
    if (k >= 0 && (mods & (KEV_CTRL | KEV_ALT))) return -1;
    return k;
}

/* Wait for one, keeping the machine alive meanwhile. */
int ui_wait_key(void)
{
    int k;
    while ((k = ui_key()) < 0) main_pump(0);
    return k;
}

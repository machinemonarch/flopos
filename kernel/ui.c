/* ===========================================================================
 * ui.c -- keys for things that are not the command line.
 *
 * The line editor wants whole lines.  A menu wants single keys, now:
 * up, down, enter, escape, and the occasional character typed into a
 * field.  They come from the keyboard, or from the serial line, where a
 * terminal program spells an arrow key as three bytes -- ESC [ A -- and
 * Escape on its own as one.  Telling those apart means waiting a moment
 * after an ESC to see whether a bracket follows.
 * ===========================================================================
 */
#include "flopos.h"

/* Returns a UK_ code or a plain character, or -1 if nothing is waiting. */
int ui_key(void)
{
    int ev, c;

    while ((ev = kbd_event()) >= 0) {
        switch (KEV_SCAN(ev)) {
        case SC_UP:    return UK_UP;
        case SC_DOWN:  return UK_DOWN;
        case SC_LEFT:  return UK_LEFT;
        case SC_RIGHT: return UK_RIGHT;
        case SC_ENTER: return UK_ENTER;
        case SC_ESC:   return UK_ESC;
        case SC_BKSP:  return UK_BKSP;
        case SC_TAB:   return (ev & KEV_SHIFT) ? UK_UP : UK_DOWN;
        default: break;
        }
        c = keymap_char(ev);
        if (c >= 32 && c < 127) return c;
    }

    c = serial_getc();
    if (c < 0) return -1;
    if (c == '\r' || c == '\n') return UK_ENTER;
    if (c == 8 || c == 127)     return UK_BKSP;
    if (c == '\t')              return UK_DOWN;
    if (c == 27) {
        u32 until = ms_now() + 40;
        int b;
        while ((b = serial_getc()) < 0 && ms_now() < until) { }
        if (b != '[' && b != 'O') return UK_ESC;
        until = ms_now() + 40;
        while ((b = serial_getc()) < 0 && ms_now() < until) { }
        switch (b) {
        case 'A': return UK_UP;
        case 'B': return UK_DOWN;
        case 'C': return UK_RIGHT;
        case 'D': return UK_LEFT;
        default:  return -1;
        }
    }
    if (c >= 32 && c < 127) return c;
    return -1;
}

/* Wait for one, keeping the machine alive meanwhile. */
int ui_wait_key(void)
{
    int k;
    while ((k = ui_key()) < 0) main_pump(0);
    return k;
}

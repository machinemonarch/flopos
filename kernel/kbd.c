/* ===========================================================================
 * kbd.c -- AT keyboard on IRQ1, scancode set 1.
 *
 * Produces two things.  A bitmap of what is held down right now, and a ring
 * of key-down events, each carrying the modifier state at the instant the
 * key went down so nothing can race between the press and it being read.
 *
 * The E0 prefix is kept rather than dropped, because on a Swedish keyboard
 * the right Alt key is AltGr and it is E0 38 -- indistinguishable from the
 * left Alt without it, and AltGr is where the braces live.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define QLEN 48

static volatile u8  s_down[128];
static volatile u16 s_queue[QLEN];
static volatile u8  s_head, s_tail;
static volatile u8  s_ext;               /* an E0 prefix is pending        */
static volatile u8  s_shift_l, s_shift_r;
static volatile u8  s_ctrl, s_alt, s_altgr;
static volatile u8  s_caps;

void kbd_init(void)
{
    int i;
    for (i = 0; i < 128; i++) s_down[i] = 0;
    s_head = s_tail = 0;
    s_ext = 0;
    s_shift_l = s_shift_r = s_ctrl = s_alt = s_altgr = s_caps = 0;
    while (inb(0x64) & 1) (void)inb(0x60);   /* drain the BIOS leftovers   */
}

static u16 modifier_bits(void)
{
    u16 m = 0;
    if (s_shift_l || s_shift_r) m |= KEV_SHIFT;
    if (s_ctrl)  m |= KEV_CTRL;
    if (s_alt)   m |= KEV_ALT;
    if (s_altgr) m |= KEV_ALTGR;
    if (s_caps)  m |= KEV_CAPS;
    return m;
}

/* Unstick the keyboard controller.
 *
 * The 8042 holds one byte in its output buffer and will not assert IRQ1
 * again until that byte is read.  If a key is pressed while the line is
 * masked -- which it is for the whole of a BIOS disk call -- the interrupt
 * edge that would have delivered it is lost when the controller is
 * reinitialised afterwards, and the byte sits there forever.  The keyboard
 * is then dead for good, while everything on another IRQ carries on
 * perfectly, which makes it look like anything but what it is.
 *
 * Draining the buffer is all it takes.  Harmless if there is nothing in it.
 */
void kbd_irq(void);

void kbd_resync(void)
{
    int guard = 0;
    while ((inb(0x64) & 1) && ++guard < 64) (void)inb(0x60);
    s_ext = 0;
}

/* After the BIOS has had the machine: anything typed meanwhile is still
 * sitting in the controller, holding its interrupt line up.  Hand it to
 * the ordinary handler, so it is a keypress rather than a lost one, and
 * so the line comes down and the next key can raise it again. */
void kbd_drain(void)
{
    int guard = 0;
    while ((inb(0x64) & 1) && ++guard < 32) kbd_irq();
}

void kbd_irq(void)
{
    u8 st, sc, code, ext;
    int release;

    st = inb(0x64);
    if (!(st & 1)) return;
    sc = inb(0x60);

    /* A byte from the pointing stick, not the keyboard.  It has to be read
     * -- the controller holds one byte and passes on nothing else until it
     * goes -- but it is not a key. */
    if (st & 0x20) return;

    if (sc == 0xE0) { s_ext = 1; return; }
    if (sc == 0xE1) { s_ext = 2; return; }
    if (s_ext == 2) { s_ext = 0; return; }   /* Pause: swallow it          */

    ext = s_ext;
    s_ext = 0;

    /* The fake shifts wrapped around some extended keys would otherwise
     * look like the user reaching for Shift. */
    if (ext && ((sc & 0x7F) == 0x2A || (sc & 0x7F) == 0x36)) return;

    code = (u8)(sc & 0x7F);
    release = (sc & 0x80) != 0;

    /* Modifiers are state, not events. */
    switch (code) {
    case 0x2A: if (!ext) { s_shift_l = (u8)!release; return; } break;
    case 0x36: s_shift_r = (u8)!release; return;
    case 0x1D: s_ctrl = (u8)!release; return;
    case 0x38:
        if (ext) s_altgr = (u8)!release;
        else     s_alt   = (u8)!release;
        return;
    case 0x3A:
        if (!release) s_caps = (u8)!s_caps;
        return;
    default: break;
    }

    if (release) {
        s_down[code] = 0;
        return;
    }

    if (!s_down[code]) {                 /* drop the hardware auto-repeat  */
        u8 nxt = (u8)((s_head + 1) % QLEN);
        if (nxt != s_tail) {
            s_queue[s_head] = (u16)(code | modifier_bits()
                                         | (ext ? KEV_EXT : 0));
            s_head = nxt;
        }
    }
    s_down[code] = 1;

    /* Ctrl+Alt+Del always works, whatever else has gone wrong. */
    if (code == 0x53 && s_ctrl && (s_alt || s_altgr)) reboot();
}

int kbd_event(void)
{
    int e;
    if (s_tail == s_head) return -1;
    e = s_queue[s_tail];
    s_tail = (u8)((s_tail + 1) % QLEN);
    return e;
}

int kbd_down(u8 scancode)
{
    return scancode < 128 ? s_down[scancode] : 0;
}

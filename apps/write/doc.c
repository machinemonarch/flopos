/* ===========================================================================
 * doc.c -- a document, and the text file it is kept in.
 *
 * The file is meant to be read on any other computer as it is: one line
 * per paragraph, a title as "% ", headings as "# " to "#### ", bold as
 * **this**, italic as *this*, underline as <u>this</u> (Markdown has no
 * underline, but its HTML passes through), the Swedish letters as UTF-8.
 * A '*', '<' or '\' in the text itself is written with a backslash in
 * front, and so is a '#' or '%' that would otherwise start a paragraph
 * and turn it into a heading.
 *
 * Bold and italic markers are written only where the styling changes,
 * and read back as switches: two stars flip bold, one flips italic.  So
 * "***" where bold ends and italic begins reads back exactly as written,
 * which is the one place this kind of markup is usually ambiguous.
 * Round trips were tested on many thousands of random documents.
 * ===========================================================================
 */
#include "doc.h"

void doc_empty(Doc *d)
{
    d->c[0].ch = '\n';
    d->c[0].at = ST_BODY;
    d->len = 1;
}

/* ---- out ------------------------------------------------------------------ */
typedef struct { u8 *p; int n, cap, over; } Out;

static void put(Out *o, u8 b) { if (o->n < o->cap) o->p[o->n++] = b; else o->over = 1; }
static void puts_(Out *o, const char *s) { while (*s) put(o, (u8)*s++); }

static void put_char(Out *o, u8 ch)
{
    u32 cp = 0;
    switch (ch) {
    case A_CH_ARING: cp = 0xC5; break;   case A_CH_ARING_L: cp = 0xE5; break;
    case A_CH_ADIA:  cp = 0xC4; break;   case A_CH_ADIA_L:  cp = 0xE4; break;
    case A_CH_ODIA:  cp = 0xD6; break;   case A_CH_ODIA_L:  cp = 0xF6; break;
    }
    if (cp) { put(o, (u8)(0xC0 | (cp >> 6))); put(o, (u8)(0x80 | (cp & 0x3F))); return; }
    if (ch == '*' || ch == '<' || ch == '\\') put(o, '\\');
    put(o, ch);
}

/* Switch from one set of styles to another: endings first, then
 * beginnings, so the markup nests the way a reader expects. */
static void switch_to(Out *o, u8 from, u8 to)
{
    u8 off = (u8)(from & ~to), on = (u8)(to & ~from);
    if (off & AT_UNDERLINE) puts_(o, "</u>");
    if ((off | on) & AT_BOLD) puts_(o, "**");
    if ((off | on) & AT_ITALIC) put(o, '*');
    if (on & AT_UNDERLINE) puts_(o, "<u>");
}

int doc_to_text(const Doc *d, u8 *out, int cap)
{
    static const char *prefix[ST_COUNT] = { "", "% ", "# ", "## ", "### ", "#### " };
    Out o;
    int i = 0;

    o.p = out; o.n = 0; o.cap = cap; o.over = 0;
    while (i < d->len) {
        int e = i, first = 1;
        u8 state = 0, style;
        while (d->c[e].ch != '\n') e++;
        style = d->c[e].at < ST_COUNT ? d->c[e].at : ST_BODY;
        puts_(&o, prefix[style]);
        for (; i < e; i++) {
            u8 at = (u8)(d->c[i].at & (AT_BOLD | AT_ITALIC | AT_UNDERLINE));
            if (at != state) { switch_to(&o, state, at); state = at; }
            if (first && (d->c[i].ch == '#' || d->c[i].ch == '%')) put(&o, '\\');
            put_char(&o, d->c[i].ch);
            first = 0;
        }
        if (state) switch_to(&o, state, 0);
        put(&o, '\n');
        i = e + 1;
    }
    return o.over ? -1 : o.n;
}

/* ---- in ------------------------------------------------------------------- */
static int add(Doc *d, u8 ch, u8 at)
{
    if (d->len >= DOC_MAX - 1) return 0;     /* room kept for the last end */
    d->c[d->len].ch = ch;
    d->c[d->len].at = at;
    d->len++;
    return 1;
}

static int starts(const u8 *p, const u8 *end, const char *s)
{
    while (*s) { if (p >= end || *p != (u8)*s) return 0; p++; s++; }
    return 1;
}

void doc_from_text(Doc *d, const u8 *in, int len)
{
    const u8 *p = in, *end = in + len;
    const u8 *stop = in;

    while (stop < end && *stop != 0x1A && *stop != 0) stop++;   /* DOS EOF */
    end = stop;
    d->len = 0;

    while (p < end && d->len < DOC_MAX - 1) {
        const u8 *eol = p;
        u8 style = ST_BODY, at = 0;
        while (eol < end && *eol != '\n') eol++;
        if (eol == end && p == end) break;

        if      (starts(p, eol, "#### ")) { style = ST_H4; p += 5; }
        else if (starts(p, eol, "### "))  { style = ST_H3; p += 4; }
        else if (starts(p, eol, "## "))   { style = ST_H2; p += 3; }
        else if (starts(p, eol, "# "))    { style = ST_H1; p += 2; }
        else if (starts(p, eol, "% "))    { style = ST_TITLE; p += 2; }

        while (p < eol) {
            u8 b = *p;
            if (b == '\r') { p++; continue; }
            if (b == '\\' && p + 1 < eol) { add(d, p[1], at); p += 2; continue; }
            if (starts(p, eol, "<u>"))  { at |= AT_UNDERLINE; p += 3; continue; }
            if (starts(p, eol, "</u>")) { at &= (u8)~AT_UNDERLINE; p += 4; continue; }
            if (b == '*') {
                int n = 0;
                while (p < eol && *p == '*') { n++; p++; }
                if ((n / 2) & 1) at ^= AT_BOLD;
                if (n & 1) at ^= AT_ITALIC;
                continue;
            }
            if (b >= 0xC2 && b <= 0xDF && p + 1 < eol && (p[1] & 0xC0) == 0x80) {
                u32 cp = ((u32)(b & 0x1F) << 6) | (p[1] & 0x3F);
                u8 g = cp == 0xC5 ? A_CH_ARING : cp == 0xE5 ? A_CH_ARING_L :
                       cp == 0xC4 ? A_CH_ADIA  : cp == 0xE4 ? A_CH_ADIA_L  :
                       cp == 0xD6 ? A_CH_ODIA  : cp == 0xF6 ? A_CH_ODIA_L  : '?';
                add(d, g, at);
                p += 2;
                continue;
            }
            if (b >= 0x80) {                 /* longer UTF-8, or anything else */
                p++;
                while (p < eol && (*p & 0xC0) == 0x80) p++;
                add(d, '?', at);
                continue;
            }
            if (b == '\t') b = ' ';
            if (b >= 32) add(d, b, at);
            p++;
        }
        d->c[d->len].ch = '\n';
        d->c[d->len].at = style;
        d->len++;
        p = eol + 1;
        if (eol >= end) break;
    }
    if (d->len == 0) doc_empty(d);
}

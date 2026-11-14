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

/* A letter past ASCII (Swedish, German, French, Spanish, Italian, the
 * pound and the euro) goes out as UTF-8; returns 0 for anything else. */
static int put_letter(Out *o, u8 ch)
{
    u32 cp = a_unicode(ch);
    if (!cp) return 0;
    if (cp < 0x800) {
        put(o, (u8)(0xC0 | (cp >> 6)));
    } else {
        put(o, (u8)(0xE0 | (cp >> 12)));
        put(o, (u8)(0x80 | ((cp >> 6) & 0x3F)));
    }
    put(o, (u8)(0x80 | (cp & 0x3F)));
    return 1;
}

static void put_char(Out *o, u8 ch)
{
    if (put_letter(o, ch)) return;
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
    return doc_to_text_at(d, out, cap, -1, 0);
}

int doc_to_text_at(const Doc *d, u8 *out, int cap, int want, int *got)
{
    static const char *prefix[ST_COUNT] = { "", "% ", "# ", "## ", "### ", "#### " };
    Out o;
    int i = 0;

    o.p = out; o.n = 0; o.cap = cap; o.over = 0;
    if (got) *got = -1;
    while (i < d->len) {
        int e = i, first = 1;
        u8 state = 0, style;
        while (d->c[e].ch != '\n') e++;
        style = d->c[e].at < ST_COUNT ? d->c[e].at : ST_BODY;
        puts_(&o, prefix[style]);
        for (; i < e; i++) {
            u8 at = (u8)(d->c[i].at & (AT_BOLD | AT_ITALIC | AT_UNDERLINE));
            if (i == want && got) *got = o.n;
            if (at != state) { switch_to(&o, state, at); state = at; }
            if (first && (d->c[i].ch == '#' || d->c[i].ch == '%')) put(&o, '\\');
            put_char(&o, d->c[i].ch);
            first = 0;
        }
        if (e == want && got) *got = o.n;
        if (state) switch_to(&o, state, 0);
        put(&o, '\n');
        i = e + 1;
    }
    if (got && *got < 0) *got = o.n;
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

/* A UTF-8 letter at p (two bytes, or three for the euro and the like)
 * becomes the font's letter, or '?' if the font has none.  Returns the
 * bytes it used, 0 if p is not the start of one. */
static int get_letter(const u8 *p, const u8 *eol, u8 *g)
{
    u32 cp;
    int n, c;
    if (p[0] >= 0xC2 && p[0] <= 0xDF && p + 1 < eol && (p[1] & 0xC0) == 0x80) {
        cp = ((u32)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        n = 2;
    } else if (p[0] >= 0xE0 && p[0] <= 0xEF && p + 2 < eol &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        cp = ((u32)(p[0] & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        n = 3;
    } else {
        return 0;
    }
    c = a_from_unicode(cp);
    *g = c ? (u8)c : '?';
    return n;
}

static int text_end(const u8 *in, int len)
{
    int n = 0;
    while (n < len && in[n] != 0x1A && in[n] != 0) n++;     /* DOS EOF */
    return n;
}

#define MARK(q) do { if (got && *got < 0 && (q) - in >= want) *got = d->len; } while (0)

void doc_from_text(Doc *d, const u8 *in, int len)
{
    doc_from_text_at(d, in, len, -1, 0);
}

void doc_from_text_at(Doc *d, const u8 *in, int len, int want, int *got)
{
    const u8 *p = in, *end = in + len;
    const u8 *stop = in;

    if (got) *got = want < 0 ? 0 : -1;

    stop = in + text_end(in, len);
    end = stop;
    d->len = 0;

    while (p < end && d->len < DOC_MAX - 1) {
        const u8 *eol = p;
        u8 style = ST_BODY, at = 0;
        while (eol < end && *eol != '\n') eol++;
        if (eol == end && p == end) break;

        MARK(p);
        if      (starts(p, eol, "#### ")) { style = ST_H4; p += 5; }
        else if (starts(p, eol, "### "))  { style = ST_H3; p += 4; }
        else if (starts(p, eol, "## "))   { style = ST_H2; p += 3; }
        else if (starts(p, eol, "# "))    { style = ST_H1; p += 2; }
        else if (starts(p, eol, "% "))    { style = ST_TITLE; p += 2; }

        while (p < eol) {
            u8 b = *p, g;
            MARK(p);
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
            { int k = get_letter(p, eol, &g); if (k) { add(d, g, at); p += k; continue; } }
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
        MARK(p);
        d->c[d->len].ch = '\n';
        d->c[d->len].at = style;
        d->len++;
        p = eol + 1;
        if (eol >= end) break;
    }
    if (d->len == 0) doc_empty(d);
    if (got && (*got < 0 || *got > d->len - 1)) *got = d->len - 1;
}

/* ---- plain ---------------------------------------------------------------- */
/* What the Markdown view edits: every character as it is in the file, the
 * marks included, nothing interpreted except the Swedish letters. */
int doc_to_plain(const Doc *d, u8 *out, int cap, int want, int *got)
{
    Out o;
    int i;
    o.p = out; o.n = 0; o.cap = cap; o.over = 0;
    if (got) *got = -1;
    for (i = 0; i < d->len; i++) {
        if (i == want && got) *got = o.n;
        if (!put_letter(&o, d->c[i].ch)) put(&o, d->c[i].ch);
    }
    if (got && *got < 0) *got = o.n;
    return o.over ? -1 : o.n;
}

int doc_from_plain(Doc *d, const u8 *in, int len, int want, int *got)
{
    const u8 *p = in, *end = in + text_end(in, len);
    int fits = 1;

    d->len = 0;
    if (got) *got = want < 0 ? 0 : -1;
    while (p < end) {
        u8 b = *p, g;
        MARK(p);
        if (d->len >= DOC_MAX - 1) { fits = 0; break; }
        if (b == '\n') { d->c[d->len].ch = '\n'; d->c[d->len].at = ST_BODY; d->len++; p++; continue; }
        if (b == '\r') { p++; continue; }
        { int k = get_letter(p, end, &g); if (k) { add(d, g, 0); p += k; continue; } }
        if (b >= 0x80) {
            p++;
            while (p < end && (*p & 0xC0) == 0x80) p++;
            add(d, '?', 0);
            continue;
        }
        if (b == '\t') b = ' ';
        if (b >= 32) add(d, b, 0);
        p++;
    }
    MARK(p);
    if (!d->len || d->c[d->len - 1].ch != '\n') {          /* always ends in an end */
        d->c[d->len].ch = '\n'; d->c[d->len].at = ST_BODY; d->len++;
    }
    if (got && (*got < 0 || *got > d->len - 1)) *got = d->len - 1;
    return fits;
}

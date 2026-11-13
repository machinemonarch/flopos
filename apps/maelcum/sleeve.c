/* ===========================================================================
 * sleeve.c -- the paper that goes in a cassette's box (a J-card), as a
 * picture to print on another computer.
 *
 * The card is 4 inches square laid flat (101.6 mm): the front, 65 mm
 * wide, which shows through the lid; the spine, 12.7 mm; and the flap
 * behind the cassette, 23.9 mm.  The picture is 600 dots square and says
 * of itself that it is 150 dots to the inch, so printed at its own size
 * (100%, not "fit to page") it comes out exact, on A4 or anything larger
 * than the card.  Cut along the outer line, fold along the dashed ones.
 *
 * It is a plain BMP of one bit a dot, black on white: 45 KB, and there
 * is no computer that cannot open it.  The letters are the system's own
 * (api->glyph), drawn two, three or four times their size.
 *
 * Two pictures: the outside (when, what was said of the tape, how much
 * is on it) and the inside (the files), to print on the back of the
 * same sheet.  The inside is laid out mirrored, so that its wide panel
 * falls behind the front.
 * ===========================================================================
 */
#include "sleeve.h"

#define PX      600                      /* dots a side                    */
#define ROWB    76                       /* bytes a row: 600 bits, to a multiple of four */
#define HEAD    62                       /* the file's header and its two colours */
#define FLAP_W  141                      /* 23.9 mm                        */
#define SPINE_W 75                       /* 12.7 mm                        */
#define FRONT_X (FLAP_W + SPINE_W)       /* the front begins here: 65 mm to the edge */

static u8 s_bmp[HEAD + ROWB * PX];
static const Api *A;

static void dot(int x, int y)
{
    if (x < 0 || y < 0 || x >= PX || y >= PX) return;
    s_bmp[HEAD + (PX - 1 - y) * ROWB + (x >> 3)] |= (u8)(0x80 >> (x & 7));   /* a BMP's rows run from the bottom up */
}

static void hline(int x, int y, int w) { while (w-- > 0) dot(x++, y); }
static void vline(int x, int y, int h) { while (h-- > 0) dot(x, y++); }
static void rect(int x, int y, int w, int h) { hline(x, y, w); hline(x, y + h - 1, w); vline(x, y, h); vline(x + w - 1, y, h); }
static void fold(int x) { int y; for (y = 4; y < PX - 4; y++) if ((y & 7) < 3) dot(x, y); }

static void ring(int cx, int cy, int r)
{
    int x = r, y = 0, e = 1 - r;
    while (x >= y) {
        dot(cx + x, cy + y); dot(cx - x, cy + y); dot(cx + x, cy - y); dot(cx - x, cy - y);
        dot(cx + y, cy + x); dot(cx - y, cy + x); dot(cx + y, cy - x); dot(cx - y, cy - x);
        y++;
        if (e < 0) e += 2 * y + 1;
        else { x--; e += 2 * (y - x) + 1; }
    }
}

/* One letter, k times its size.  turned: on its side, to be read from the
 * top of the card down, as on a cassette's spine. */
static void letter(int x, int y, int ch, int k, int turned)
{
    u8 cols[5];
    int c, r, i, j;
    if (!A->glyph(ch, cols)) return;
    for (c = 0; c < 5; c++)
        for (r = 0; r < 8; r++) {
            if (!(cols[c] >> r & 1)) continue;
            for (i = 0; i < k; i++)
                for (j = 0; j < k; j++)
                    if (turned) dot(x + (7 - r) * k + i, y + c * k + j);
                    else dot(x + c * k + i, y + r * k + j);
        }
}

static int len_of(const char *t) { int n = 0; while (t[n]) n++; return n; }

static void words(int x, int y, const char *t, int k)
{
    for (; *t; t++, x += 6 * k) letter(x, y, (u8)*t, k, 0);
}

static void words_down(int x, int y, const char *t, int k)
{
    for (; *t; t++, y += 6 * k) letter(x, y, (u8)*t, k, 1);
}

/* Text broken between words into lines of at most `wide` letters; returns
 * the y under the last line. */
static int wrapped(int x, int y, const char *t, int k, int wide, int most)
{
    char line[64];
    int n, cut, i;
    if (wide > 63) wide = 63;
    while (*t && most-- > 0) {
        for (n = 0; t[n] && n < wide; n++) { }
        cut = n;
        if (t[n]) { for (cut = n; cut > 0 && t[cut] != ' '; cut--) { } if (!cut) cut = n; }
        for (i = 0; i < cut; i++) line[i] = t[i];
        line[cut] = 0;
        words(x, y, line, k);
        y += 8 * k + k;
        t += cut;
        while (*t == ' ') t++;
    }
    return y;
}

static void when_text(const Sleeve *s, char *m, u32 cap)
{
    if (s->have_when) A->format(m, cap, "%02u-%02u-%u %02u:%02u", (u32)s->when[2], (u32)s->when[1], 2000u + s->when[0], (u32)s->when[3], (u32)s->when[4]);
    else A->format(m, cap, "%s", "(no clock)");
}

/* A cassette, in line: for the front. */
static void cassette(int x, int y)
{
    int i;
    rect(x, y, 240, 150); rect(x + 1, y + 1, 238, 148);
    rect(x + 14, y + 14, 212, 88);                       /* the label          */
    rect(x + 48, y + 40, 144, 44);                       /* the window         */
    for (i = 0; i < 2; i++) {
        int cx = x + 84 + i * 72, cy = y + 62;
        ring(cx, cy, 15); ring(cx, cy, 14); ring(cx, cy, 5);
        hline(cx - 13, cy, 7); hline(cx + 7, cy, 7); vline(cx, cy - 13, 7); vline(cx, cy + 7, 7);
    }
    hline(x + 24, y + 28, 192);                          /* a line to write on */
    /* the opening at the bottom, where the heads go */
    for (i = 0; i < 30; i++) { dot(x + 44 - i / 2, y + 119 + i); dot(x + 195 + i / 2, y + 119 + i); }
    hline(x + 44, y + 119, 152);
    ring(x + 82, y + 134, 5); ring(x + 158, y + 134, 5);
    rect(x + 110, y + 128, 20, 12);
}

static void outside(const Sleeve *s)
{
    char m[96];
    int x = FRONT_X + 16, y, wide = (PX - FRONT_X - 28) / 12;

    rect(0, 0, PX, PX);                                  /* cut here           */
    fold(FLAP_W); fold(FRONT_X);

    /* the front */
    words(x, 18, "maelcum", 4);
    words(x, 58, "Magnetic Rust:", 2);
    words(x, 78, "Riddim 'n' Vibrationz Engine", 2);
    hline(x, 100, PX - x - 14); hline(x, 101, PX - x - 14);
    when_text(s, m, sizeof m);
    words(x, 112, m, 3);
    A->format(m, sizeof m, "%s: %u file%s, %u bytes", s->did, s->files, s->files == 1 ? "" : "s", s->bytes);
    y = wrapped(x, 146, m, 2, wide, 2);
    A->format(m, sizeof m, "%s speed%s%s%s", s->fast ? "Fast" : "Normal", s->packed ? ", packed (LZSS)" : "", s->cipher ? ", encrypted: " : "", s->cipher ? s->cipher : "");
    y = wrapped(x, y, m, 2, wide, 3) + 10;
    if (s->verdict[0]) {
        static char q[220];                              /* what was said of it, in quotes */
        A->format(q, sizeof q, "\"%s\"", s->verdict);
        y = wrapped(x, y, q, 2, wide, 9);
    }
    if (y < PX - 170) cassette(x + (PX - x - 14 - 240) / 2, PX - 166);

    /* the spine, and the flap: on their sides */
    when_text(s, m, sizeof m);
    {
        char sp[64];
        A->format(sp, sizeof sp, "FLOPOS backup  %s", m);
        words_down(FLAP_W + (SPINE_W - 24) / 2, 20, sp, 3);
    }
    A->format(m, sizeof m, "maelcum, FLOPOS %s", s->version);
    words_down(FLAP_W - 40, 20, m, 2);
    A->format(m, sizeof m, "%u file%s, %s speed%s", s->files, s->files == 1 ? "" : "s", s->fast ? "fast" : "normal", s->cipher ? ", encrypted" : "");
    words_down(FLAP_W - 70, 20, m, 2);
}

static void inside(const Sleeve *s)
{
    char m[96];
    const char *name;
    u32 size;
    int i, y = 78, front_w = PX - FRONT_X, right = front_w - 14, most;

    rect(0, 0, PX, PX);
    /* Seen from the back the front is on the left, and everything is
     * written on it.  Only ticks at the edges show the folds; the lines
     * are on the other side. */
    vline(front_w, 0, 8); vline(front_w, PX - 8, 8);
    vline(front_w + SPINE_W, 0, 8); vline(front_w + SPINE_W, PX - 8, 8);

    words(16, 16, "On this tape", 3);
    when_text(s, m, sizeof m);
    words(16, 46, m, 2);
    hline(16, 68, right - 16); hline(16, 69, right - 16);

    if (s->cipher) {
        A->format(m, sizeof m, "%u file%s, encrypted (%s).", s->files, s->files == 1 ? "" : "s", s->cipher);
        y = wrapped(16, y, m, 2, 29, 3) + 8;
        wrapped(16, y, "The names are sealed on the tape, so they are not printed here.", 2, 29, 4);
        return;
    }
    most = (PX - 16 - y) / 20;
    for (i = 0; s->file(i, &name, &size); i++) {
        int n;
        if (i == most - 1 && s->file(i + 1, &name, &size)) {     /* no room for the rest */
            int all = i;
            while (s->file(all, &name, &size)) all++;
            A->format(m, sizeof m, "... and %u more", (u32)(all - i));
            words(16, y, m, 2);
            return;
        }
        s->file(i, &name, &size);
        n = len_of(name);
        if (n > 21) name += n - 21;                      /* its end says most  */
        words(16, y, name, 2);
        A->format(m, sizeof m, "%u", size);
        words(right - len_of(m) * 12, y, m, 2);
        y += 20;
    }
}

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

u32 sleeve_draw(const Api *api, const Sleeve *s, int in, const u8 **bmp)
{
    u32 i;
    A = api;
    for (i = 0; i < sizeof s_bmp; i++) s_bmp[i] = 0;
    s_bmp[0] = 'B'; s_bmp[1] = 'M';
    put32(s_bmp + 2, sizeof s_bmp);
    put32(s_bmp + 10, HEAD);
    put32(s_bmp + 14, 40);                               /* the plain old header   */
    put32(s_bmp + 18, PX); put32(s_bmp + 22, PX);
    s_bmp[26] = 1; s_bmp[28] = 1;                        /* one plane, one bit a dot */
    put32(s_bmp + 34, ROWB * PX);
    put32(s_bmp + 38, 5906); put32(s_bmp + 42, 5906);    /* 150 dots to the inch, as dots to the metre */
    put32(s_bmp + 46, 2);
    s_bmp[54] = s_bmp[55] = s_bmp[56] = 255;             /* a 0 is white; a 1 is black */
    if (in) inside(s); else outside(s);
    *bmp = s_bmp;
    return sizeof s_bmp;
}

/* ===========================================================================
 * sleeve.c -- the paper that goes in a cassette's box (a J-card), as a
 * document to print on another computer.
 *
 * The card is 4 inches square laid flat (101.6 mm): the front, 65 mm
 * wide, which shows through the lid; the spine, 12.7 mm; and the flap
 * behind the cassette, 23.9 mm.  It is drawn 600 dots square, 150 to the
 * inch.  Cut along the outer line, fold along the dashed ones.
 *
 * It is saved as a PDF of one A4 page: the outside (when, what was said
 * of the tape, how much is on it) at the top and the inside (the files)
 * right under it, the two touching.  A page of a PDF has a size of its
 * own, so the cards come out at their true size with nothing to set in
 * the print program, which a bare picture cannot promise.  Printed on one
 * side, the two are cut out as one piece and the inside folded back
 * along the line between them.  So the inside is printed upside down, and
 * laid out mirrored, so that its wide panel falls behind the front.
 * (Side by side would not fit: two cards are 203 mm, A4 is 210 mm wide,
 * and few printers print that near the edge.)
 *
 * The PDF is the plainest there is (version 1.3, 1999): two pictures of
 * one bit a dot, packed by run length (a filter every reader has had
 * since PDF 1.0), and two lines of Helvetica, a face every reader has
 * built in.  About 23 KB, so it fits on a tape like any other file.  The letters on the card
 * are the system's own (api->glyph), drawn two, three or four times
 * their size.
 * ===========================================================================
 */
#include "sleeve.h"

#define PX      600                      /* dots a side                    */
#define ROWB    75                       /* bytes a row: 600 bits          */
#define IMG     (ROWB * PX)              /* one side, a bit a dot          */
#define FLAP_W  141                      /* 23.9 mm                        */
#define SPINE_W 75                       /* 12.7 mm                        */
#define FRONT_X (FLAP_W + SPINE_W)       /* the front begins here: 65 mm to the edge */

static u8 s_pdf[2 * (IMG + IMG / 128 + 2) + 2048];
static u8 s_side[IMG];                   /* one side, drawn here, then packed */
static u8 s_turned[IMG];                 /* the inside, turned upside down for the page */
static u8 s_packed[IMG + IMG / 128 + 2]; /* run-length at its worst        */
static u32 s_n;                          /* how much of the file is written */
static u8 *s_img;                        /* the side being drawn           */
static const Api *A;

static void dot(int x, int y)
{
    if (x < 0 || y < 0 || x >= PX || y >= PX) return;
    s_img[y * ROWB + (x >> 3)] |= (u8)(0x80 >> (x & 7));   /* rows from the top down; a 1 is black */
}

static void hline(int x, int y, int w) { while (w-- > 0) dot(x++, y); }
static void vline(int x, int y, int h) { while (h-- > 0) dot(x, y++); }
static void rect(int x, int y, int w, int h) { hline(x, y, w); hline(x, y + h - 1, w); vline(x, y, h); vline(x + w - 1, y, h); }
static void fold(int x) { int y; for (y = 4; y < PX - 4; y++) if ((y & 7) < 3) dot(x, y); }
static void hfold(int y) { int x; for (x = 0; x < PX; x++) if ((x & 7) < 3) dot(x, y); }
/* The outer line to cut along, on three sides: the fourth is where the
 * two cards meet. */
static void edges(void) { hline(0, 0, PX); vline(0, 0, PX); vline(PX - 1, 0, PX); }

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

    edges();                                             /* cut here           */
    hfold(PX - 1);                                       /* fold the inside back here */
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

    edges();                     /* the side where it meets the outside is the outside's fold */
    /* Seen from the back the front is on the left, and everything is
     * written on it.  The folds are dashed here too, at the places that
     * fall behind the outside's once it is turned for the page. */
    fold(front_w - 1); fold(front_w + SPINE_W - 1);

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

/* ---- the file -------------------------------------------------------------------- */
/* PDF's RunLengthDecode (in PDF since 1.0): a length byte n, then n + 1
 * bytes as they are (n < 128), or one byte said 257 - n times (n > 128);
 * 128 ends it.  The sleeve is mostly white paper, so it shrinks about
 * tenfold. */
static u32 run_length(const u8 *in, u32 n, u8 *out)
{
    u32 i = 0, o = 0;
    while (i < n) {
        u32 r = 1, lit;
        while (i + r < n && r < 128 && in[i + r] == in[i]) r++;
        if (r >= 2) { out[o++] = (u8)(257 - r); out[o++] = in[i]; i += r; continue; }
        /* bytes as they are, up to the next run of two or more */
        for (lit = 1; i + lit < n && lit < 128; lit++)
            if (i + lit + 1 < n && in[i + lit] == in[i + lit + 1]) break;
        out[o++] = (u8)(lit - 1);
        for (r = 0; r < lit; r++) out[o++] = in[i + r];
        i += lit;
    }
    out[o++] = 128;
    return o;
}

static void put(const char *t) { while (*t) s_pdf[s_n++] = (u8)*t++; }

static void number(u32 v, int digits)
{
    char m[12];
    int i;
    for (i = digits - 1; i >= 0; i--) { m[i] = (char)('0' + v % 10); v /= 10; }
    m[digits] = 0;
    put(m);
}

static void whole(u32 v)                 /* a number, as few digits as it needs */
{
    char m[12];
    A->format(m, sizeof m, "%u", v);
    put(m);
}

u32 sleeve_pdf(const Api *api, const Sleeve *s, const u8 **pdf)
{
    /* The one page: the outside at the top, 20 mm from the edge, the
     * inside right under it, upside down, each 288 points (4 inches)
     * square and centred across; under them how to print, cut and fold. */
    static const char page[] =
        "q 288 0 0 288 153.64 497.2 cm /Im1 Do Q\n"
        "q 288 0 0 288 153.64 209.2 cm /Im2 Do Q\n"
        "BT /F1 8 Tf 153.64 196 Td (Print at 100% \\(actual size\\). Cut around both cards as one piece.) Tj ET\n"
        "BT /F1 8 Tf 153.64 185 Td (Fold the lower card back along the line between them, then fold on the other dashed lines.) Tj ET\n";
    u32 at[8], i, xref, n;
    int side;
    A = api;
    s_n = 0;
    put("%PDF-1.3\n%\xE2\xE3\xCF\xD3\n");
    at[1] = s_n; put("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    at[2] = s_n; put("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    at[3] = s_n;
    put("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.28 841.89] /Contents 6 0 R\n"
        "   /Resources << /ProcSet [/PDF /Text /ImageB] /Font << /F1 7 0 R >> /XObject << /Im1 4 0 R /Im2 5 0 R >> >> >>\nendobj\n");
    for (side = 0; side < 2; side++) {
        at[4 + side] = s_n;
        put(side ? "5 0 obj\n" : "4 0 obj\n");
        s_img = s_side;
        for (i = 0; i < IMG; i++) s_img[i] = 0;
        if (side) inside(s); else outside(s);
        if (side) {                      /* half a turn: folded back under the outside, it reads upright */
            u32 x, y;
            for (i = 0; i < IMG; i++) s_turned[i] = 0;
            for (y = 0; y < PX; y++)
                for (x = 0; x < PX; x++)
                    if (s_side[y * ROWB + (x >> 3)] & (0x80 >> (x & 7)))
                        s_turned[(PX - 1 - y) * ROWB + ((PX - 1 - x) >> 3)] |= (u8)(0x80 >> ((PX - 1 - x) & 7));
        }
        n = run_length(side ? s_turned : s_side, IMG, s_packed);
        put("<< /Type /XObject /Subtype /Image /Width 600 /Height 600 /ColorSpace /DeviceGray\n"
            "   /BitsPerComponent 1 /Decode [1 0] /Filter /RunLengthDecode /Length ");
        whole(n); put(" >>\nstream\n");
        for (i = 0; i < n; i++) s_pdf[s_n + i] = s_packed[i];
        s_n += n;
        put("\nendstream\nendobj\n");
    }
    at[6] = s_n;
    put("6 0 obj\n<< /Length "); whole(sizeof page - 1); put(" >>\nstream\n"); put(page); put("endstream\nendobj\n");
    at[7] = s_n;
    put("7 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");
    xref = s_n;
    put("xref\n0 8\n0000000000 65535 f \n");
    for (i = 1; i < 8; i++) { number(at[i], 10); put(" 00000 n \n"); }
    put("trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n"); whole(xref); put("\n%%EOF\n");
    *pdf = s_pdf;
    return s_n;
}

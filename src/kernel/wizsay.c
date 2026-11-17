/* ===========================================================================
 * wizsay.c -- the wizard answers the words the shell doesn't know.
 *
 * WIZARD.WIZ on the floppy is a very small language model: a transformer
 * of about 240,000 weights, trained on another computer (tools/wizard) and
 * read here once, at boot.  It knows no facts and doesn't read what was
 * typed.  It keeps a voice, and says one line in it.
 *
 * The file: "WIZ1"; six 16-bit numbers (word pieces, width, layers, heads,
 * inner width, memory); the word pieces, each a length and its letters;
 * then the weights.  A table of weights is one 32-bit scale for each row
 * and then a byte for each weight; the short lists (norms, biases) are
 * plain 32-bit floats.  tools/wizard/speak.py does the same sums from the
 * same file, and tools/wizard/enginetest holds this file up against it.
 *
 * This is the one file in the kernel that uses the floating-point unit.
 * Nothing else in the kernel does, so there is no state to save: the sound
 * routine that runs in the timer interrupt saves and restores it itself.
 *
 * He thinks a layer at a time and lets the machine draw its frame between
 * layers, so neither the screen nor the music waits for him.
 * ===========================================================================
 */
#ifdef WIZ_HOST
typedef unsigned char u8;
typedef unsigned int u32;
#else
#include "flopos.h"
#endif

#define MAXL  4
#define TOPK  30
#define TEMP  0.8f

typedef struct {
    float *g1, *b1, *wq, *wk, *wv, *wo, *g2, *b2, *w1, *c1, *w2, *c2;
    float *K, *V;                        /* what each past piece offered   */
} Layer;

static struct {
    int V, d, L, H, ff, ctx;
    const u8 **voc;                      /* a length, then the letters      */
    float *tok, *pos, *g, *b;
    Layer l[MAXL];
    float *x, *h, *q, *a, *f, *z, *s;
    int t;                               /* pieces said so far              */
    int ok;
} W;

/* ---- sums ----------------------------------------------------------------- */
static float w_sqrt(float x)
{
    float r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

/* e to the x, for x at or below zero: two to a whole power, times a short
 * series for what is left over. */
static float w_exp(float x)
{
    union { float f; u32 u; } p;
    float y, r;
    int n;
    if (x < -40.0f) return 0.0f;
    y = x * 1.44269504f;
    n = (int)y;
    if ((float)n > y) n--;
    r = (y - (float)n) * 0.69314718f;
    r = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.041666667f
        + r * (0.0083333333f + r * 0.0013888889f)))));
    p.u = (u32)(n + 127) << 23;
    return r * p.f;
}

static float dot(const float *a, const float *b, int n)
{
    float s = 0.0f;
    while (n--) s += *a++ * *b++;
    return s;
}

static void norm(float *out, const float *x, const float *g, const float *b)
{
    float m = 0.0f, v = 0.0f, r;
    int i, d = W.d;
    for (i = 0; i < d; i++) m += x[i];
    m /= (float)d;
    for (i = 0; i < d; i++) v += (x[i] - m) * (x[i] - m);
    r = 1.0f / w_sqrt(v / (float)d + 1e-5f);
    for (i = 0; i < d; i++) out[i] = (x[i] - m) * r * g[i] + b[i];
}

/* ---- reading the file ------------------------------------------------------- */
static const u8 *s_p, *s_end;
static void *(*s_alloc)(u32);

static float *floats(int n)
{
    float *v = (float *)s_alloc((u32)n * 4u);
    u8 *o = (u8 *)v;
    int i;
    if (!v || s_p + n * 4 > s_end) { W.ok = 0; return v; }
    for (i = 0; i < n * 4; i++) o[i] = *s_p++;
    return v;
}

static float *table(int rows, int cols)
{
    float *v = (float *)s_alloc((u32)(rows * cols) * 4u);
    const u8 *sc = s_p;
    int r, c;
    if (!v || s_p + rows * 4 + rows * cols > s_end) { W.ok = 0; return v; }
    s_p += rows * 4;
    for (r = 0; r < rows; r++) {
        union { float f; u8 b[4]; } k;
        for (c = 0; c < 4; c++) k.b[c] = sc[r * 4 + c];
        for (c = 0; c < cols; c++) v[r * cols + c] = (float)(signed char)*s_p++ * k.f;
    }
    return v;
}

static int rd16(void) { int v = s_p[0] | (s_p[1] << 8); s_p += 2; return v; }

int wiz_load(const u8 *file, u32 size, void *(*alloc)(u32))
{
    int i, d;
    W.ok = 0;
    if (size < 16 || file[0] != 'W' || file[1] != 'I' || file[2] != 'Z' || file[3] != '1') return 0;
    s_p = file + 4; s_end = file + size; s_alloc = alloc;
    W.V = rd16(); W.d = rd16(); W.L = rd16(); W.H = rd16(); W.ff = rd16(); W.ctx = rd16();
    d = W.d;
    if (W.V < 2 || W.V > 4096 || d < W.H || d > 512 || W.L < 1 || W.L > MAXL ||
        W.H < 1 || d % W.H || W.ff < 1 || W.ff > 2048 || W.ctx < 2 || W.ctx > 512) return 0;
    W.voc = (const u8 **)alloc((u32)W.V * sizeof(u8 *));
    if (!W.voc) return 0;
    for (i = 0; i < W.V; i++) {
        if (s_p >= s_end || s_p + 1 + *s_p > s_end) return 0;
        W.voc[i] = s_p;
        s_p += 1 + *s_p;
    }
    W.ok = 1;
    W.tok = table(W.V, d);
    W.pos = table(W.ctx, d);
    for (i = 0; i < W.L && W.ok; i++) {
        Layer *l = &W.l[i];
        l->g1 = floats(d);        l->b1 = floats(d);
        l->wq = table(d, d);      l->wk = table(d, d);
        l->wv = table(d, d);      l->wo = table(d, d);
        l->g2 = floats(d);        l->b2 = floats(d);
        l->w1 = table(W.ff, d);   l->c1 = floats(W.ff);
        l->w2 = table(d, W.ff);   l->c2 = floats(d);
        l->K = (float *)alloc((u32)(W.ctx * d) * 4u);
        l->V = (float *)alloc((u32)(W.ctx * d) * 4u);
        if (!l->K || !l->V) W.ok = 0;
    }
    W.g = floats(d); W.b = floats(d);
    W.x = (float *)alloc((u32)d * 4u);     W.h = (float *)alloc((u32)d * 4u);
    W.q = (float *)alloc((u32)d * 4u);     W.a = (float *)alloc((u32)d * 4u);
    W.f = (float *)alloc((u32)W.ff * 4u);  W.z = (float *)alloc((u32)W.V * 4u);
    W.s = (float *)alloc((u32)W.ctx * 4u);
    if (!W.x || !W.h || !W.q || !W.a || !W.f || !W.z || !W.s || s_p != s_end) W.ok = 0;
    W.t = 0;
    return W.ok;
}

/* ---- one piece further ------------------------------------------------------ */
void wiz_begin(void) { W.t = 0; }

/* Takes the piece just said, and returns how likely each next one is (as
 * scores, before any are turned into chances).  0 when his memory of the
 * line is full.  breathe is called between layers. */
const float *wiz_next(int id, void (*breathe)(void))
{
    int d = W.d, hd = W.d / W.H, t = W.t, i, j, li, hh;
    float inv = 1.0f / w_sqrt((float)hd);
    if (!W.ok || t >= W.ctx || id < 0 || id >= W.V) return 0;
    for (i = 0; i < d; i++) W.x[i] = W.tok[id * d + i] + W.pos[t * d + i];
    for (li = 0; li < W.L; li++) {
        Layer *l = &W.l[li];
        float *K = l->K + t * d, *V = l->V + t * d;
        norm(W.h, W.x, l->g1, l->b1);
        for (i = 0; i < d; i++) {
            W.q[i] = dot(l->wq + i * d, W.h, d);
            K[i]   = dot(l->wk + i * d, W.h, d);
            V[i]   = dot(l->wv + i * d, W.h, d);
        }
        for (hh = 0; hh < W.H; hh++) {
            float top = -1e30f, sum = 0.0f;
            int o = hh * hd;
            for (j = 0; j <= t; j++) {
                W.s[j] = dot(W.q + o, l->K + j * d + o, hd) * inv;
                if (W.s[j] > top) top = W.s[j];
            }
            for (j = 0; j <= t; j++) { W.s[j] = w_exp(W.s[j] - top); sum += W.s[j]; }
            for (i = 0; i < hd; i++) W.a[o + i] = 0.0f;
            for (j = 0; j <= t; j++) {
                float p = W.s[j] / sum;
                const float *v = l->V + j * d + o;
                for (i = 0; i < hd; i++) W.a[o + i] += p * v[i];
            }
        }
        for (i = 0; i < d; i++) W.x[i] += dot(l->wo + i * d, W.a, d);
        norm(W.h, W.x, l->g2, l->b2);
        for (i = 0; i < W.ff; i++) {
            float v = dot(l->w1 + i * d, W.h, d) + l->c1[i];
            W.f[i] = v > 0.0f ? v : 0.0f;
        }
        for (i = 0; i < d; i++) W.x[i] += dot(l->w2 + i * W.ff, W.f, W.ff) + l->c2[i];
        if (breathe) breathe();
    }
    norm(W.h, W.x, W.g, W.b);
    for (i = 0; i < W.V; i++) W.z[i] = dot(W.tok + i * d, W.h, d);
    W.t = t + 1;
    return W.z;
}

/* Choose among the TOPK likeliest, by their chances.  u is 0..1. */
int wiz_pick(const float *z, float u)
{
    int best[TOPK] = { 0 }, n = 0, i, j;
    float p[TOPK], top, sum = 0.0f;
    for (i = 0; i < W.V; i++) {
        if (n == TOPK && z[i] <= z[best[n - 1]]) continue;
        j = n < TOPK ? n++ : n - 1;
        while (j > 0 && z[best[j - 1]] < z[i]) { best[j] = best[j - 1]; j--; }
        best[j] = i;
    }
    top = z[best[0]];
    for (i = 0; i < n; i++) { p[i] = w_exp((z[best[i]] - top) / TEMP); sum += p[i]; }
    u *= sum;
    for (i = 0; i < n - 1; i++) { if (u < p[i]) break; u -= p[i]; }
    return best[i];
}

const u8 *wiz_piece(int id) { return W.voc[id]; }

#ifndef WIZ_HOST
/* ---- in FLOPOS -------------------------------------------------------------- */
static void *take(u32 n) { return halloc(n); }
static void breathe(void) { main_pump(0); }

/* Read him off the floppy.  Needs a floating-point unit and about two
 * megabytes; without either, the shell keeps its own plain answers. */
int wizsay_init(void)
{
    FsFile f;
    u8 *buf;
    u32 a, b, c, d;
    if (!g_has_cpuid || heap_is_low()) return 0;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    (void)a; (void)b; (void)c;
    if (!(d & 1)) return 0;
    if (heap_total() - heap_used() < 0x200000u) return 0;
    if (!fs_mount() || !fs_find("WIZARD.WIZ", &f) || f.size < 16 || f.size > 0x80000u) return 0;
    buf = (u8 *)halloc(f.size);
    if (!buf || fs_read(&f, buf, f.size) != (int)f.size) return 0;
    __asm__("fninit");
    return wiz_load(buf, f.size, take);
}

/* A word of his goes on the screen whole: on the next row if this one has
 * no room left.  (The HUD breaks its own lines, so there it is left be.) */
static int  s_word[48];                  /* characters, wide ones too      */
static int  s_wn;

static void word_out(void)
{
    int i = 0;
    if (!s_wn) return;
    if (!term_muted() && term_cursor_col() > 0 && term_cursor_col() + s_wn > term_cols()) {
        term_newline();
        if (s_word[0] == ' ') i = 1;
    }
    for (; i < s_wn; i++) term_putc(s_word[i]);
    s_wn = 0;
}

static void letter(int c)
{
    if (c == ' ' || s_wn >= (int)(sizeof s_word / sizeof *s_word)) word_out();
    s_word[s_wn++] = c;
}

/* One line from him, written as he thinks of it, with the typed word put
 * wherever he means it.  0 if he isn't here or had nothing to say. */
int wizsay(const char *typed)
{
    int id = 0, said = 0, i, n;
    if (!W.ok) return 0;
    wiz_begin();
    s_wn = 0;
    g_ctrl_c = 0;
    term_colour(C_ACCENT);
    for (;;) {
        if (g_ctrl_c) {                  /* Ctrl-C: he stops mid-line */
            word_out();
            term_print(" ^C");
            while (kbd_event() >= 0) { }     /* and the keys typed meanwhile go */
            g_ctrl_c = 0;
            said = 1;
            break;
        }
        const float *z = wiz_next(id, breathe);
        const u8 *p;
        u32 r;
        if (!z) break;
        rand_bytes((u8 *)&r, 4);
        id = wiz_pick(z, (float)(r >> 8) * (1.0f / 16777216.0f));
        if (id == 0) break;
        p = wiz_piece(id);
        for (i = 1; i <= p[0]; i++) {
            if (p[i] == '@') {
                const char *t = typed;
                int c;
                for (n = 0; n < 24 && (c = ch_next(&t)) != 0; n++) letter(c);
            } else letter(p[i]);
        }
        said = 1;
    }
    word_out();
    if (said) term_newline();
    serial_printf("[wizard] %d pieces\n", W.t);
    return said;
}
#endif

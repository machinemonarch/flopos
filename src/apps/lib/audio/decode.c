/* ===========================================================================
 * decode.c -- telling formats apart, WAV, and bringing samples to the
 * card's rate.
 * ===========================================================================
 */
#include "decode.h"

static int is(const u8 *p, const char *s)
{
    while (*s) if (*p++ != (u8)*s++) return 0;
    return 1;
}

static int ctype_is(const char *c, const char *s)
{
    while (*s) {
        char a = *c++, b = *s++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

static int detect(AudioIn *in, Decoder *d, const char *ctype);

/* Too slow or too fast a rate is not music, and would not resample sanely. */
int audio_detect(AudioIn *in, Decoder *d, const char *ctype)
{
    int r = detect(in, d, ctype);
    if (r == 1 && (d->rate < 4000 || d->rate > 384000)) return 0;
    return r;
}

u32 audio_detect_limit(const AudioIn *in)
{
    const u8 *b = in->buf + in->pos;
    return in->len - in->pos >= 8 && is(b + 4, "ftyp") ? 0xFFFFFFFFu : 65536u;
}

static int detect(AudioIn *in, Decoder *d, const char *ctype)
{
    const u8 *b = in->buf + in->pos;
    u32 n = in->len - in->pos;

    if (n < 12 && !in->done) return AUDIO_MORE;
    if (n >= 12 && is(b, "RIFF") && is(b + 8, "WAVE")) return wav_open(in, d);
    if (n >= 4 && is(b, "OggS")) return vorbis_open(in, d);
    if (n >= 4 && is(b, "fLaC")) return flac_open(in, d);
    if (n >= 8 && is(b + 4, "ftyp")) return mp4_open(in, d);
    /* FLAC with an ID3 tag in front, which some programs put there */
    if (n >= 10 && is(b, "ID3")) {
        u32 t = 10 + ((u32)(b[6] & 127) << 21 | (u32)(b[7] & 127) << 14 | (u32)(b[8] & 127) << 7 | (b[9] & 127));
        if (t + 4 <= n && is(b + t, "fLaC")) { in->pos += t; return flac_open(in, d); }
        /* or AAC, which aac_open steps over the tag for */
        if (t + 2 <= n && b[t] == 0xFF && (b[t + 1] & 0xF6) == 0xF0) return aac_open(in, d);
    }
    /* a FLAC station, joined wherever it is (before mp3: its frames start
     * with bits an mp3 header would too) */
    if (ctype && (ctype_is(ctype, "audio/flac") || ctype_is(ctype, "audio/x-flac")))
        return flac_open(in, d);
    /* AAC in ADTS frames: a sync word like mp3's, but with "layer" 0 -- or
     * the server saying so (audio/aac, audio/aacp, audio/x-aac). */
    if ((n >= 2 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) ||
        (ctype && (ctype_is(ctype, "audio/aac") || ctype_is(ctype, "audio/x-aac"))))
        return aac_open(in, d);
    /* mp3: an ID3 tag in front, or a frame header straight away, or the
     * server saying so (radio starts wherever it likes). */
    if ((n >= 3 && is(b, "ID3")) || (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) ||
        (ctype && (ctype_is(ctype, "audio/mpeg") || ctype_is(ctype, "audio/mp3"))))
        return mp3_open(in, d);
    return 0;
}

/* ---- WAV ------------------------------------------------------------------
 * RIFF chunks: "fmt " says how the samples are laid out, "data" is them.
 * Plain PCM, 8 or 16 bits (and 24, 32), any number of channels, any rate,
 * mixed down to mono here. */
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }

static int s_wav_bytes, s_wav_frame;     /* bytes a sample, bytes a frame */
static u32 s_wav_end;                    /* where the samples stop         */

static int wav_decode(Decoder *d, AudioIn *in, s16 *out, int max)
{
    int n = 0, c;
    while (n < max) {
        s32 sum = 0;
        const u8 *p;
        if (in->pos >= s_wav_end) return n ? n : AUDIO_END;
        if (in->pos + (u32)s_wav_frame > in->len) {
            if (in->done) return n ? n : AUDIO_END;
            return n ? n : AUDIO_MORE;
        }
        p = in->buf + in->pos;
        for (c = 0; c < d->channels; c++, p += s_wav_bytes) {
            if (s_wav_bytes == 1) sum += ((s32)p[0] - 128) * 256;
            else                  sum += (s32)(short)le16(p + s_wav_bytes - 2);  /* the top 16 bits */
        }
        out[n++] = (s16)(sum / d->channels);
        in->pos += (u32)s_wav_frame;
    }
    return n;
}

int wav_open(AudioIn *in, Decoder *d)
{
    u32 at = in->pos + 12, fmt = 0;
    int bits = 0;

    for (;;) {
        u32 size;
        if (at + 8 > in->len) return in->done ? 0 : AUDIO_MORE;
        size = le32(in->buf + at + 4);
        if (is(in->buf + at, "fmt ")) {
            if (at + 8 + 16 > in->len) return in->done ? 0 : AUDIO_MORE;
            fmt = le16(in->buf + at + 8);
            d->channels = (int)le16(in->buf + at + 10);
            d->rate = (int)le32(in->buf + at + 12);
            bits = (int)le16(in->buf + at + 22);
            if (fmt == 0xFFFE && at + 8 + 26 <= in->len) fmt = le16(in->buf + at + 8 + 24);
        } else if (is(in->buf + at, "data")) {
            if (fmt != 1 || d->channels < 1 || d->channels > 8 || d->rate < 1000 ||
                (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return 0;
            s_wav_bytes = bits / 8;
            s_wav_frame = s_wav_bytes * d->channels;
            in->pos = at + 8;
            s_wav_end = size >= 0xFFFFFFF0u - in->pos ? 0xFFFFFFFFu : in->pos + size;
            d->kind = "WAV";
            d->kbps = d->rate * s_wav_frame * 8 / 1000;
            d->decode = wav_decode;
            return 1;
        }
        at += 8 + size + (size & 1);
        if (at > 64 * 1024 * 1024) return 0;
    }
}

/* ---- the card's rate ---------------------------------------------------------- */
void rs_init(Resampler *r, int from, int to)
{
    int i;
    r->step = (u32)(((double)from / (double)to) * 65536.0);
    r->phase = 0;
    r->prev = 0;
    r->taps = (from + to / 2) / to;
    if (r->taps < 1) r->taps = 1;
    if (r->taps > 8) r->taps = 8;
    r->h = 0; r->sum = 0; r->err = 0;
    r->recip = 65536 / r->taps;          /* a multiply a sample, not a division */
    for (i = 0; i < 8; i++) r->hist[i] = 0;
}

/* Straight lines between the samples it has: plenty, at these rates, for
 * an 8-bit speaker.
 *
 * Down to 8 bits by noise shaping rather than by dropping the low byte:
 * each sample is rounded with the error the last one left added in, so
 * the rounding error comes out as a faint hiss up in the treble instead of
 * as grit on every quiet passage -- which is how an 8-bit card makes
 * music sound crackly. */
static int lines(Resampler *r, const s16 *in, int n, u8 *out)
{
    int k = 0;
    while (n > 0) {
        u32 i = r->phase >> 16;
        s32 a, b, v;
        if ((int)i >= n) { r->phase -= (u32)n << 16; r->prev = in[n - 1]; break; }
        a = i == 0 ? r->prev : in[i - 1];
        b = in[i];
        v = a + (((b - a) * (s32)(r->phase & 0xFFFF)) >> 16) + r->err;
        {
            s32 q = (v + 128) >> 8;
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            r->err = v - (q << 8);
            if (r->err > 256) r->err = 256;          /* after clipping, let go */
            if (r->err < -256) r->err = -256;
            out[k++] = (u8)(q + 128);
        }
        r->phase += r->step;
    }
    return k;
}

int rs_run(Resampler *r, const s16 *in, int n, u8 *out)
{
    static s16 f[1024];
    int k = 0;
    if (r->taps < 2) return lines(r, in, n, out);
    while (n > 0) {                      /* the average of the last few     */
        int m = n < 1024 ? n : 1024, i;
        for (i = 0; i < m; i++) {
            r->sum += in[i] - r->hist[r->h];
            r->hist[r->h] = in[i];
            if (++r->h == r->taps) r->h = 0;
            f[i] = (s16)((r->sum * r->recip) >> 16);
        }
        k += lines(r, f, m, out + k);
        in += m; n -= m;
    }
    return k;
}

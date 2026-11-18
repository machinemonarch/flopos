/* decode.c -- a recording through FLOPOS's own decoders, on this PC.
 *
 *     gcc -O2 -Isrc/apps/sdk -Isrc/apps/lib/audio -Isrc/apps/lib/bits -Isrc/apps/lib/dsp \
 *         test/audiotest/decode.c src/apps/lib/audio/{aac,decode,flac,mp3,vorbis}.c \
 *         src/apps/lib/bits/prefix.c src/apps/lib/dsp/{dct,mdct}.c -lm -o build/audiotest/decode
 *     build/audiotest/decode FILE > OUT.raw
 *
 * Writes mono 16-bit samples at the recording's rate (as FLOPOS plays
 * them, before the card's resampling) to standard output, and what it
 * found on standard error: the kind, the rate, and how loud it came out.
 * Compare with a reference decoder (see compare.py and the notes there). */
#include "decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int main(int argc, char **argv)
{
    static s16 out[8192];
    AudioIn in;
    Decoder d;
    FILE *f;
    long n;
    double sum = 0;
    unsigned long total = 0;
    int k, i, peak = 0;

    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "decode FILE > OUT.raw\n"); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    in.buf = malloc((size_t)n + 64);
    in.len = (u32)fread(in.buf, 1, (size_t)n, f);
    memset(in.buf + in.len, 0, 64);
    in.pos = 0; in.done = 1;
    fclose(f);
    memset(&d, 0, sizeof d);
    if (audio_detect(&in, &d, "") != 1) { fprintf(stderr, "FAILED: not something FLOPOS plays\n"); return 1; }
    while ((k = d.decode(&d, &in, out, 8192)) > 0) {
        fwrite(out, sizeof *out, (size_t)k, stdout);
        for (i = 0; i < k; i++) { sum += (double)out[i] * out[i]; if (abs(out[i]) > peak) peak = abs(out[i]); }
        total += (unsigned long)k;
    }
    fprintf(stderr, "%s, %d Hz, %d channels: %lu samples, loudness %.0f, peak %d%s\n", d.kind, d.rate, d.channels,
            total, total ? sqrt(sum / total) : 0.0, peak, k == AUDIO_BAD ? " (stopped: could not be read)" : "");
    return k == AUDIO_BAD;
}

/* audio.c -- the recordings' decoders (src/apps/lib/audio), given garbage.
 * Ways in: whatever audio_detect makes of it, and each decoder told
 * outright that the file is its kind; each as a whole file and as a
 * stream that arrives in pieces. */
#include "decode.h"
#include <stdlib.h>
#include <string.h>

const char *fuzz_name = "audio";
const u32 fuzz_max = 96 * 1024;

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    static s16 pcm[1152];
    u8 *copy = malloc(len ? len : 1);    /* exactly as long: a read past the end is caught */
    AudioIn in;
    Decoder d;
    int ok, calls, stream = (mode >> 8) & 1;

    memcpy(copy, data, len);
    memset(&d, 0, sizeof d);
    in.buf = copy; in.pos = 0;
    in.len = stream ? (len < 3000 ? len : 3000) : len;
    in.done = !stream || in.len == len;

    for (;;) {
        switch (mode % 6) {
        case 0:  ok = audio_detect(&in, &d, (mode >> 9) & 1 ? "audio/mpeg" : 0); break;
        case 1:  ok = mp3_open(&in, &d); break;
        case 2:  ok = vorbis_open(&in, &d); break;
        case 3:  ok = flac_open(&in, &d); break;
        case 4:  ok = aac_open(&in, &d); break;
        default: ok = wav_open(&in, &d); break;
        }
        if (ok != AUDIO_MORE || in.done) break;
        in.len += 1 + (mode >> 12) % 4000;
        if (in.len >= len) { in.len = len; in.done = 1; }
    }
    if (ok == 1 && d.decode) {
        for (calls = 0; calls < 400; calls++) {
            int want = 1 + (int)((mode >> 16) + (u32)calls * 37) % 1152;
            int n = d.decode(&d, &in, pcm, (calls & 3) ? 1152 : want);
            if (n == AUDIO_END || n == AUDIO_BAD) break;
            if (n == AUDIO_MORE) {
                if (in.done) break;
                in.len += 1 + (mode >> 12) % 4000;
                if (in.len >= len) { in.len = len; in.done = 1; }
            }
        }
    }
    free(copy);
}

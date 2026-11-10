/* render.c -- play a tracker song on this PC, through FLOPOS's own tracker.
 *
 *   gcc -O1 -I apps -I apps/lib/audio -o /tmp/render tools/audiotest/render.c apps/lib/audio/tracker.c
 *   /tmp/render song.xm out.u8 [rate] [seconds]
 *
 * Writes 8-bit unsigned mono, as the Sound Blaster gets it.  compare.py
 * then holds it up against libopenmpt. */
#include "app.h"
#include "tracker.h"
#include <stdio.h>
#include <stdlib.h>

static u8 buf[4 << 20];                  /* the file, and room to unpack IT samples */

int main(int argc, char **argv)
{
    FILE *f, *o;
    u32 n, tot = 0;
    int rate = argc > 3 ? atoi(argv[3]) : 22050, secs = argc > 4 ? atoi(argv[4]) : 40;
    static u8 out[3400];
    if (argc < 3 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "render song out.u8 [rate] [seconds]\n"); return 2; }
    n = (u32)fread(buf, 1, sizeof buf / 2, f);
    fclose(f);
    if (!tracker_load(buf, n, sizeof buf, rate)) { printf("not a song it can play\n"); return 1; }
    printf("%s, %d channels, \"%s\"\n", tracker_kind(), tracker_channels(), tracker_title());
    if (!(o = fopen(argv[2], "wb"))) return 2;
    while (tot < (u32)rate * (u32)secs) {
        int k = tracker_render(out, (int)sizeof out);
        fwrite(out, 1, (size_t)k, o);
        tot += (u32)k;
        if (k < (int)sizeof out) { printf("ended after %.1f s\n", tot / (double)rate); break; }
    }
    fclose(o);
    return 0;
}

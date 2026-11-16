/* enginetest.c -- FLOPOS's own wizard engine (src/kernel/wizsay.c), built for
 * this PC.  Given the model and a line of piece numbers, it prints the
 * scores for the next piece after each one, for enginetest.py to hold up
 * against speak.py. */
#include <stdio.h>
#include <stdlib.h>
#define WIZ_HOST
#include "../../src/kernel/wizsay.c"

static void *take(u32 n) { return malloc(n); }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    static u8 buf[1 << 20];
    u32 n = (u32)fread(buf, 1, sizeof buf, f);
    int i, j;
    if (!wiz_load(buf, n, take)) { puts("load"); return 1; }
    wiz_begin();
    for (i = 2; i < argc; i++) {
        const float *z = wiz_next(atoi(argv[i]), 0);
        if (!z) { puts("full"); return 1; }
        for (j = 0; j < W.V; j++) printf("%.5f ", z[j]);
        printf("| %d %d\n", wiz_pick(z, 0.0f), wiz_pick(z, 0.999999f));
    }
    return 0;
}

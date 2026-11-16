/* tracker.c -- the tracker (MOD, XM, S3M, IT), given garbage songs. */
#include "app.h"
#include "tracker.h"
#include <stdlib.h>
#include <string.h>

const char *fuzz_name = "tracker";
const u32 fuzz_max = 512 * 1024;

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    static u8 out[3400];
    u32 room = len + ((mode >> 8) & 1 ? 0 : (mode >> 9) % (1024 * 1024));   /* room to unpack IT samples */
    u8 *copy = malloc(room ? room : 1);
    int calls;

    memcpy(copy, data, len);
    if (tracker_load(copy, len, room, (mode & 1) ? 22050 : 11025))
        for (calls = 0; calls < 60; calls++)
            if (tracker_render(out, (int)sizeof out) < (int)sizeof out) break;
    free(copy);
}

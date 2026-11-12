/* fstest.c -- FLOPOS's own file code (kernel/fs.c), built for this PC and
 * run against a floppy image in a file.
 *
 *   fstest IMAGE [--cut N] < commands
 *
 * Commands, one a line: write PATH LEN SEED, read PATH, mkdir PATH,
 * rm PATH, mv FROM TO, ls PATH, room.  Each answers one line: "ok ..." or
 * "no WHY".  --cut N stops dead after N sectors have been written, as a
 * power cut would, and leaves the image as it then is.
 * Driven by tools/fstest/run.py; not meant to be read by eye.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FS_HOST
#include "../../kernel/fs.c"

static u8 *s_img;
static long s_len, s_cut = -1;
static const char *s_path;

static void keep(void)
{
    FILE *f = fopen(s_path, "wb");
    if (!f || fwrite(s_img, 1, (size_t)s_len, f) != (size_t)s_len) { fprintf(stderr, "cannot write the image\n"); exit(2); }
    fclose(f);
}

int disk_read_run(u32 lba, u32 sectors, u8 *dst)
{
    if ((long)(lba + sectors) * 512 > s_len) return 0;
    memcpy(dst, s_img + (long)lba * 512, (size_t)sectors * 512);
    return 1;
}

int disk_write_run(u32 lba, u32 sectors, const u8 *src)
{
    u32 k;
    if ((long)(lba + sectors) * 512 > s_len) return 0;
    for (k = 0; k < sectors; k++) {
        if (s_cut == 0) { keep(); printf("cut\n"); exit(3); }
        if (s_cut > 0) s_cut--;
        memcpy(s_img + (long)(lba + k) * 512, src + (size_t)k * 512, 512);
    }
    return 1;
}

void *halloc(u32 bytes) { return calloc(1, bytes); }

int rtc_read(u8 *h, u8 *mi, u8 *se, u16 *yr, u8 *mo, u8 *da)
{
    *h = 12; *mi = 34; *se = 56; *yr = 2026; *mo = 11; *da = 12;
    return 1;
}

static void fill(u8 *p, u32 n, u32 seed)
{
    u32 x = seed, i;
    for (i = 0; i < n; i++) { x = x * 1103515245u + 12345u; p[i] = (u8)(x >> 16); }
}

static u32 sum(const u8 *p, u32 n)
{
    u32 h = 2166136261u, i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void one(const FsItem *it, void *ctx)
{
    (void)ctx;
    printf(" %s%s:%u", it->name, it->folder ? "/" : "", it->size);
}

int main(int argc, char **argv)
{
    static u8 buf[1 << 21];
    char line[512], a[200], b[200];
    FILE *f;
    u32 n, seed;
    int r;

    if (argc < 2) return 2;
    s_path = argv[1];
    if (argc >= 4 && !strcmp(argv[2], "--cut")) s_cut = atol(argv[3]);
    f = fopen(s_path, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); s_len = ftell(f); fseek(f, 0, SEEK_SET);
    s_img = malloc((size_t)s_len);
    if (fread(s_img, 1, (size_t)s_len, f) != (size_t)s_len) return 2;
    fclose(f);

    while (fgets(line, sizeof line, stdin)) {
        a[0] = b[0] = 0;
        if (sscanf(line, "write %199s %u %u", a, &n, &seed) == 3) {
            fill(buf, n, seed);
            r = inv_write(a, buf, n);
        } else if (sscanf(line, "read %199s", a) == 1) {
            r = inv_read(a, buf, sizeof buf);
            if (r >= 0) { printf("ok %d %u\n", r, sum(buf, (u32)r)); continue; }
            r = 0;
        } else if (sscanf(line, "mkdir %199s", a) == 1) r = inv_mkdir(a);
        else if (sscanf(line, "rm %199s", a) == 1) r = inv_remove(a);
        else if (sscanf(line, "mv %199s %199s", a, b) == 2) r = inv_move(a, b);
        else if (!strncmp(line, "ls", 2)) {
            sscanf(line, "ls %199s", a);
            printf("ok");
            r = inv_list(a, one, 0);
            if (r < 0) printf(" no %s", inv_why());
            printf("\n");
            continue;
        } else if (!strncmp(line, "room", 4)) { printf("ok %u\n", inv_room()); continue; }
        else { printf("no what?\n"); continue; }
        if (r) printf("ok\n"); else printf("no %s\n", inv_why());
    }
    keep();
    return 0;
}

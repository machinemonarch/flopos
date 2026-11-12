/* kshim.c -- what the kernel would have given its own code, for targets
 * built from kernel/ files (x509, tls): memory, ROOTS.DAT (the file named
 * in FUZZ_ROOTS, made by run.py from its own authority), a clock that
 * says a day after that file was made so undamaged certificates are in
 * date, and random numbers that are the same every time, so that a
 * recorded conversation can be played back. */
#include "flopos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static u8 *s_file; static u32 s_file_len;
static u32 s_rand = 1, s_now;

void kshim_reset(void) { s_rand = 1; s_now = 1000; }

void *halloc(usize bytes) { return calloc(1, bytes ? bytes : 1); }
int fs_mount(void) { return 1; }
int fs_find(const char *name, FsFile *f)
{
    if (!s_file) {
        const char *path = getenv("FUZZ_ROOTS");
        FILE *fp = path ? fopen(path, "rb") : 0;
        (void)name;
        if (!fp) return 0;
        s_file = malloc(400000);
        s_file_len = (u32)fread(s_file, 1, 400000, fp);
        fclose(fp);
    }
    f->first = 2; f->size = s_file_len;
    return 1;
}
int fs_read(const FsFile *f, u8 *buf, u32 max)
{
    u32 n = f->size < max ? f->size : max;
    memcpy(buf, s_file, n);
    return (int)n;
}
int rtc_read(u8 *hour, u8 *min, u8 *sec, u16 *year, u8 *month, u8 *day)
{
    static time_t made;
    struct tm *t;
    if (!made) {
        struct stat st;
        const char *path = getenv("FUZZ_ROOTS");
        made = (path && !stat(path, &st) ? st.st_mtime : time(0)) + 86400;
    }
    t = gmtime(&made);
    *hour = (u8)t->tm_hour; *min = (u8)t->tm_min; *sec = (u8)t->tm_sec;
    *year = (u16)(t->tm_year + 1900); *month = (u8)(t->tm_mon + 1); *day = (u8)t->tm_mday;
    return 1;
}
u32  ms_now(void) { return s_now += 7; }
void rand_add(u32 v) { (void)v; }
void rand_bytes(u8 *out, int n)
{
    int i;
    for (i = 0; i < n; i++) { s_rand ^= s_rand << 13; s_rand ^= s_rand >> 17; s_rand ^= s_rand << 5; out[i] = (u8)(s_rand >> 9); }
}
void serial_print(const char *s) { (void)s; }
void serial_printf(const char *fmt, ...) { (void)fmt; }
int  ui_key(void) { return -1; }
int  ip_parse(const char *s, u32 *out) { (void)s; (void)out; return 0; }

/* inflate.c -- the kernel's unpacker (src/kernel/inflate.c), given damaged
 * packed programs: it must refuse them or unpack them, and never read or
 * write past either buffer.  Undamaged ones must come back whole (their
 * Adler-32 is checked as they unpack). */
#include "flopos.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

const char *fuzz_name = "inflate";
const u32 fuzz_max = 64 * 1024;

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    u32 cap = mode & 1 ? 256 * 1024 : 4096;        /* room enough, or too little */
    u8 *in = malloc(len ? len : 1), *out = malloc(cap);   /* exact, so a step past is caught */
    memcpy(in, data, len);
    (void)inflate_zlib(in, len, out, cap);
    free(in); free(out);
}

int fs_read(const FsFile *f, u8 *b, u32 m) { (void)f; (void)b; (void)m; return -1; }   /* load_packed and load_whole are not fuzzed here */
void *halloc(usize n) { return malloc(n ? n : 1); }
void hgive_back(void *p, usize n) { (void)p; (void)n; }

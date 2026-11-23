/* inflate.h -- packed programs on the floppy (inflate.c). */
#ifndef INFLATE_H
#define INFLATE_H
#include "flopos.h"
int inflate_zlib(const u8 *in, u32 n, u8 *out, u32 cap);
u8 *load_whole(const FsFile *f, u32 max, u32 *len);
int load_packed(const FsFile *f, u8 *dest, u32 room, const char **why);
#endif

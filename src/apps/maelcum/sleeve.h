/* sleeve.h -- the paper that goes in a cassette's box, to print.  See sleeve.c. */
#ifndef SLEEVE_H
#define SLEEVE_H
#include "app.h"

typedef struct {
    u8  when[6];                         /* year less 2000, month, day, hour, minute, second */
    int have_when;                       /* the clock could be read        */
    const char *did;                     /* "Written" or "Read"            */
    const char *verdict;                 /* what was said of the tape, or "" */
    const char *version;                 /* FLOPOS's                       */
    const char *cipher;                  /* its name if the save is sealed, or 0 */
    u32 files, bytes;
    int fast, packed;
    /* The i-th file's name and size; 0 when there are no more. */
    int (*file)(int i, const char **name, u32 *size);
} Sleeve;

/* Draw both sides (the outside above the inside) as a PDF of one A4
 * page; gives the whole file and returns its length. */
u32 sleeve_pdf(const Api *api, const Sleeve *s, const u8 **pdf);
#endif

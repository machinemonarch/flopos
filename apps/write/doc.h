/* The editor's document: characters with their styling, paragraphs ended
 * by a '\n' cell that carries the paragraph's style.  There is always at
 * least one paragraph, so the document always ends in a '\n'. */
#ifndef DOC_H
#define DOC_H
#include "app.h"

#define DOC_MAX   12000                  /* characters, ends included      */
#define FILE_MAX  16384                  /* a document file on the floppy  */

enum { ST_BODY, ST_TITLE, ST_H1, ST_H2, ST_H3, ST_H4, ST_COUNT };

typedef struct { u8 ch, at; } DCell;     /* at: AT_* bits, or a style on '\n' */
typedef struct { DCell c[DOC_MAX]; int len; } Doc;

void doc_empty(Doc *d);
int  doc_to_text(const Doc *d, u8 *out, int cap);        /* bytes, or -1 */
void doc_from_text(Doc *d, const u8 *in, int len);

/* The same, following one place across the change: the cell at want comes
 * out as a byte offset in *got, or the byte at want as a cell. */
int  doc_to_text_at(const Doc *d, u8 *out, int cap, int want, int *got);
void doc_from_text_at(Doc *d, const u8 *in, int len, int want, int *got);

/* Plain: the text exactly as written, marks and all, one paragraph a line.
 * doc_from_plain returns 0 if it did not all fit. */
int  doc_to_plain(const Doc *d, u8 *out, int cap, int want, int *got);
int  doc_from_plain(Doc *d, const u8 *in, int len, int want, int *got);
#endif

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
#endif

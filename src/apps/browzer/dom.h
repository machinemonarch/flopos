/* ===========================================================================
 * dom.h -- a page as a tree (dom.c), its styles (css.c), and its scripts
 * (script.c), for browzer.
 * ===========================================================================
 */
#ifndef DOM_H
#define DOM_H
#include "web.h"

typedef signed char s8;
typedef signed short s16;

#define DOM_NODES  32768                 /* apps have 12 MB: big news pages fit */
#define DOM_ATTRS  65000                 /* under 65536: Attr.next and Node.attr are 16 bits */
#define DOM_POOL   (1536 * 1024)

enum { N_ELEM = 1, N_TEXT = 3, N_COMMENT = 8, N_DOC = 9 };

/* display, as browzer understands it */
enum { D_INLINE, D_BLOCK, D_NONE, D_ITEM, D_TABLE, D_ROW, D_CELL };

/* flags */
#define NF_CLICK   0x01                  /* a script listens for clicks     */
#define NF_SUBMIT  0x02
#define NF_CHANGE  0x04
#define NF_VALUE   0x08                  /* value holds a typed value       */
#define NF_SCRIPT_DONE 0x10              /* a <script> already run          */
#define NF_HIDDEN  0x20                  /* worked out: not drawn           */
#define NF_GAPL    0x40                  /* inline, with room before it (margin, padding) */
#define NF_GAPR    0x80                  /* and after it                    */

typedef struct {
    u16 parent, first, last, next, prev; /* 0: none                         */
    u8  type, tag;
    u32 name;                            /* pool: the tag's name; for text, the text */
    u32 len;                             /* a text node's length            */
    u16 attr;                            /* the first attribute, 0: none    */
    u8  flags;
    /* the styles that matter on a text screen, worked out by css.c */
    u8  disp, col, at, align, indent, xform, pre, nobullet;
    u32 value, vlen;                     /* a field's value as typed        */
} Node;

typedef struct { u16 next; u32 name, val; u32 vlen; } Attr;

extern Node  g_node[DOM_NODES];
extern u32   g_nodes;
extern char  g_pool[DOM_POOL];
extern u32   g_pool_used;
extern int   g_dom_full;                 /* the page did not fit            */

/* building, as the page streams in */
void dom_begin(void);
void dom_byte(u8 b);
void dom_end(void);

/* reading and changing */
const char *dom_tag(u32 n);              /* "div"; "" for text              */
const char *dom_attr(u32 n, const char *name);   /* 0 if it has none   */
int   dom_set_attr(u32 n, const char *name, const char *val, u32 vlen);
void  dom_del_attr(u32 n, const char *name);
u32   dom_create(const char *tag);
u32   dom_text_node(const char *t, u32 len);
int   dom_append(u32 parent, u32 child, u32 before);   /* before 0: at the end */
void  dom_remove(u32 n);
u32   dom_text(u32 n, char *out, u32 cap);              /* textContent      */
u32   dom_clone(u32 n, int deep);                       /* cloneNode, loose */
int   dom_set_text(u32 n, const char *t, u32 len);
int   dom_set_html(u32 n, const char *html, u32 len);  /* innerHTML =     */
u32   dom_html(u32 n, char *out, u32 cap, int outer);   /* innerHTML       */
int   dom_has_class(u32 n, const char *cls, u32 len);
u32   dom_root(void);                    /* the <html> element              */
u32   dom_find(const char *tag);         /* the first <body>, <head>...    */
u32   dom_by_id(const char *id);
int   dom_value(u32 n, char *out, u32 cap);   /* a field's value         */

/* selectors (shared by CSS and querySelector): compounds right to left */
#define SEL_PARTS 8
enum { C_NONE, C_DESC, C_CHILD, C_ADJ, C_SIB };
enum { PS_FIRST = 1, PS_LAST = 2, PS_ROOT = 8, PS_EMPTY = 16, PS_CHECKED = 32, PS_DISABLED = 64,
       PS_NEVER = 128, PS_NTH = 256, PS_NOT = 512, PS_ENABLED = 1024 };
typedef struct {
    u16 tag, id;                         /* g_selpool offsets, 0: none      */
    u16 cls[3]; u8 clen[3], ncls;
    u16 an[2], av[2]; u8 aop[2], alen[2], nat;
    u8  comb;                            /* how it joins the one to its left */
    u16 pseudo;
    s8  nth_a, nth_b;
    u16 notv; u8 notk, notl;             /* :not(.x) '.', (#x) '#', (tag) 't' */
} Part;
extern char g_selpool[65000];
extern u32  g_selpool_used;
int   sel_parse(const char *s, u32 len, Part *out, int max, int *specificity);   /* parts, 0: bad */
int   sel_match(const Part *s, int count, u32 n);
#define SEL_TRIES_ALL 30000000u          /* for one page's styles: past it, no more matches */
void  sel_budget(void);                  /* a page's styles, or one query, begin */
int   dom_query(u32 from, const char *sel, u32 len, u16 *out, int max);   /* -1: bad selector */

/* drawing: the tree as HTML the page reader understands */
void  dom_render(Page *pg);

/* styles (css.c) */
void  css_reset(void);
void  css_add(const char *text, u32 len);
void  css_apply(void);                   /* work out every node's styles   */
void  css_inline(u32 n, const char *decls, u32 len);   /* style="..."  */
int   css_count(void);

/* scripts (script.c) */
void  script_page(Page *pg, const Url *u);   /* the page has loaded       */
void  script_tick(Page *pg);             /* from the main loop: timers      */
int   script_click(Page *pg, int link);  /* 1 if a script took the click   */
int   script_submit(Page *pg, int form); /* 1 if a script stopped it       */
void  script_changed(Page *pg, int field);
int   script_site_on(const char *host);
void  script_site_toggle(Page *pg);
int   script_state(void);             /* 0 no scripts, 1 some not run, 2 this site's run */
const char *script_go(void);          /* where a script asked to go, once */
const char *script_note(void);           /* "3 scripts, none of them run." */
extern int g_dom_dirty;                  /* scripts changed the page        */

/* web.c, for the parts above */
u32   web_entity(const char *name);
int   web_get(const char *url, const char *method, const u8 *body, int blen, const char *btype,
              char *out, int cap, int *status, char *ctype, int ccap);
void  web_rerender(Page *pg);            /* the page again, from the tree   */
void  web_html_byte(u8 b);               /* the page reader, a byte at a time */
void  web_html_flush(void);

#endif

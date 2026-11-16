/* lex.h -- the words of a script (lex.c), for the compiler. */
#ifndef LEX_H
#define LEX_H

enum { TK_EOF, TK_NUM, TK_STR, TK_IDENT, TK_TEMPLATE, TK_REGEX, TK_PUNCT };

enum {
    KW_BREAK, KW_CASE, KW_CATCH, KW_CLASS, KW_CONST, KW_CONTINUE, KW_DEBUGGER, KW_DEFAULT,
    KW_DELETE, KW_DO, KW_ELSE, KW_EXPORT, KW_EXTENDS, KW_FINALLY, KW_FOR, KW_FUNCTION, KW_IF,
    KW_IMPORT, KW_IN, KW_INSTANCEOF, KW_NEW, KW_RETURN, KW_SUPER, KW_SWITCH, KW_THIS, KW_THROW,
    KW_TRY, KW_TYPEOF, KW_VAR, KW_VOID, KW_WHILE, KW_WITH, KW_YIELD, KW_LET, KW_STATIC, KW_ASYNC,
    KW_AWAIT, KW_OF, KW_GET, KW_SET, KW_NULL, KW_TRUE, KW_FALSE, KW_ARGUMENTS, KW_TARGET,
    KW_COUNT
};
/* The ones that can never be a variable's name. */
#define KW_RESERVED(k) ((k) >= 0 && (k) <= KW_WITH && (k) != KW_OF)

/* Punctuation as its characters packed in an int: P2('=','>') is "=>". */
#define P1(a)       (a)
#define P2(a,b)     (((a) << 8) | (b))
#define P3(a,b,c)   (((a) << 16) | ((b) << 8) | (c))
#define P4(a,b,c,d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))

typedef struct {
    int  t;                              /* TK_*                             */
    int  nl;                             /* a line ended before it           */
    u32  pos, end, line;
    f64  num;
    Str *s;                              /* a name, a string, a template piece, a pattern */
    Str *flags;                          /* a regular expression's           */
    int  p;                              /* punctuation                      */
    int  kw;                             /* a name that is a keyword, or -1  */
    int  tail;                           /* a template piece ends the template */
} Tok;

typedef struct {
    const u16 *src;
    u32 n, pos, line;
    int regex_ok;
    const char *err;
    u32 err_line;
    int depth_tpl;
} Lex;

extern Str *g_kw[KW_COUNT];
void lex_init_keywords(void);
int  kw_of(Str *s);
void lex_start(Lex *L, const u16 *src, u32 n);
void lex_next(Lex *L, Tok *t);
void lex_template_more(Lex *L, Tok *t, u32 at);

#endif

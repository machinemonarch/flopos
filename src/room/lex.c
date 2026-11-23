/* ===========================================================================
 * lex.c -- cutting a script into words.
 *
 * The words are numbers, strings, names, punctuation, regular expressions
 * and pieces of template strings.  Whether a / starts a regular
 * expression or divides depends on the word before it, the way every
 * JavaScript reader decides it.  The reader also notes whether a line
 * ended before each word, which the grammar needs for its missing
 * semicolons.
 * ===========================================================================
 */
#include "js.h"
#include "lex.h"

static const char *s_kwnames[KW_COUNT] = {
    "break", "case", "catch", "class", "const", "continue", "debugger", "default",
    "delete", "do", "else", "export", "extends", "finally", "for", "function", "if",
    "import", "in", "instanceof", "new", "return", "super", "switch", "this", "throw",
    "try", "typeof", "var", "void", "while", "with", "yield", "let", "static", "async",
    "await", "of", "get", "set", "null", "true", "false", "arguments", "target"
};
Str *g_kw[KW_COUNT];

void lex_init_keywords(void)
{
    int i;
    for (i = 0; i < KW_COUNT; i++) g_kw[i] = atom(s_kwnames[i]);
}

int kw_of(Str *s)
{
    int i;
    for (i = 0; i < KW_COUNT; i++) if (g_kw[i] == s) return i;
    return -1;
}

void lex_start(Lex *L, const u16 *src, u32 n)
{
    L->src = src; L->n = n; L->pos = 0; L->line = 1;
    L->regex_ok = 1;
    L->err = 0;
    L->depth_tpl = 0;
}

static int id_start(u32 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0xAA; }
static int id_part(u32 c) { return id_start(c) || (c >= '0' && c <= '9') || c == 0x200C || c == 0x200D; }
static int hexv(u32 c) { c |= 0; return c >= '0' && c <= '9' ? (int)(c - '0') : (c | 32) >= 'a' && (c | 32) <= 'f' ? (int)((c | 32) - 'a' + 10) : -1; }

static void fail(Lex *L, const char *why) { if (!L->err) { L->err = why; L->err_line = L->line; } }

/* Skip spaces and comments; note if a line ended. */
static int skip(Lex *L)
{
    int nl = 0;
    while (L->pos < L->n) {
        u32 c = L->src[L->pos];
        if (c == '\n' || c == 0x2028 || c == 0x2029) { nl = 1; L->line++; L->pos++; }
        else if (c == ' ' || c == '\t' || c == '\r' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF || (c >= 0x2000 && c <= 0x200A) || c == 0x3000) L->pos++;
        else if (c == '/' && L->pos + 1 < L->n && L->src[L->pos + 1] == '/') {
            while (L->pos < L->n && L->src[L->pos] != '\n') L->pos++;
        } else if (c == '/' && L->pos + 1 < L->n && L->src[L->pos + 1] == '*') {
            L->pos += 2;
            while (L->pos + 1 < L->n && !(L->src[L->pos] == '*' && L->src[L->pos + 1] == '/')) {
                if (L->src[L->pos] == '\n') { nl = 1; L->line++; }
                L->pos++;
            }
            L->pos += 2;
        } else if (c == '<' && L->pos + 3 < L->n && L->src[L->pos + 1] == '!' && L->src[L->pos + 2] == '-' && L->src[L->pos + 3] == '-') {
            while (L->pos < L->n && L->src[L->pos] != '\n') L->pos++;   /* <!-- in old pages */
        } else break;
    }
    if (L->pos > L->n) L->pos = L->n;
    return nl;
}

/* A string's characters, with its escapes worked out.  end: the closing
 * quote, or for a template ` and ${. */
static Str *string_body(Lex *L, u32 quote, int *tail)
{
    u32 start = L->pos, n = 0, i;
    u16 *buf;
    Str *s;
    /* count first: escapes only make it shorter */
    for (i = L->pos; i < L->n; i++) {
        u32 c = L->src[i];
        if (c == '\\') { i++; continue; }
        if (c == quote) break;
        if (quote == '`' && c == '$' && i + 1 < L->n && L->src[i + 1] == '{') break;
        if (quote != '`' && c == '\n') break;
    }
    if (L->skipping) {                    /* looking ahead: where it ends is enough */
        while (L->pos < i) { if (L->src[L->pos] == '\n') L->line++; L->pos++; }
        if (L->pos < L->n && L->src[L->pos] == quote) { L->pos++; if (tail) *tail = 1; }
        else if (quote == '`' && L->pos + 1 < L->n && L->src[L->pos] == '$') { L->pos += 2; if (tail) *tail = 0; }
        else if (quote != '`') fail(L, "a string has no end");
        return S_empty;
    }
    s = str_new(i - start);
    if (!s) { fail(L, "out of memory"); return 0; }
    buf = s->c;
    while (L->pos < L->n) {
        u32 c = L->src[L->pos];
        if (c == quote) { L->pos++; if (tail) *tail = 1; break; }
        if (quote == '`' && c == '$' && L->pos + 1 < L->n && L->src[L->pos + 1] == '{') { L->pos += 2; if (tail) *tail = 0; break; }
        if (quote != '`' && c == '\n') { fail(L, "a string has no end"); break; }
        if (c == '\n') L->line++;
        L->pos++;
        if (c != '\\') { buf[n++] = (u16)c; continue; }
        if (L->pos >= L->n) break;
        c = L->src[L->pos++];
        switch (c) {
        case 'n': buf[n++] = '\n'; break;
        case 't': buf[n++] = '\t'; break;
        case 'r': buf[n++] = '\r'; break;
        case 'b': buf[n++] = '\b'; break;
        case 'f': buf[n++] = '\f'; break;
        case 'v': buf[n++] = 0x0B; break;
        case '0':
            if (L->pos < L->n && L->src[L->pos] >= '0' && L->src[L->pos] <= '7') {
                u32 v = 0, k = 0;
                L->pos--;
                while (k < 3 && L->pos < L->n && L->src[L->pos] >= '0' && L->src[L->pos] <= '7') { v = v * 8 + (L->src[L->pos++] - '0'); k++; }
                buf[n++] = (u16)v;
            } else buf[n++] = 0;
            break;
        case 'x': {
            int a = L->pos + 1 < L->n ? hexv(L->src[L->pos]) : -1, b = L->pos + 1 < L->n ? hexv(L->src[L->pos + 1]) : -1;
            if (a < 0 || b < 0) { fail(L, "a bad \\x escape"); break; }
            buf[n++] = (u16)(a * 16 + b); L->pos += 2;
            break;
        }
        case 'u': {
            u32 v = 0;
            if (L->pos < L->n && L->src[L->pos] == '{') {
                L->pos++;
                while (L->pos < L->n && L->src[L->pos] != '}') { int h = hexv(L->src[L->pos++]); if (h < 0) { fail(L, "a bad \\u escape"); break; } v = v * 16 + (u32)h; }
                L->pos++;
            } else {
                int k;
                for (k = 0; k < 4; k++) { int h = L->pos < L->n ? hexv(L->src[L->pos++]) : -1; if (h < 0) { fail(L, "a bad \\u escape"); break; } v = v * 16 + (u32)h; }
            }
            if (v >= 0x10000 && v < 0x110000) { v -= 0x10000; buf[n++] = (u16)(0xD800 + (v >> 10)); buf[n++] = (u16)(0xDC00 + (v & 0x3FF)); }
            else buf[n++] = (u16)v;
            break;
        }
        case '\r': if (L->pos < L->n && L->src[L->pos] == '\n') L->pos++; L->line++; break;   /* a continued line */
        case '\n': case 0x2028: case 0x2029: L->line++; break;
        default: buf[n++] = (u16)c;
        }
        if (n > s->len) n = s->len;
    }
    s->len = n;
    return s;
}

static int punct(Lex *L, Tok *t)
{
    static const char *ps[] = {
        ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
        "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=",
        "%=", "&=", "|=", "^=", "<<", ">>", "**",
        "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%", "&", "|",
        "^", "!", "~", "?", ":", "=", ".", "@", "#", 0 };
    int i;
    for (i = 0; ps[i]; i++) {
        const char *p = ps[i];
        u32 k = 0;
        while (p[k] && L->pos + k < L->n && L->src[L->pos + k] == (u16)p[k]) k++;
        if (!p[k]) {
            /* ?. followed by a digit is ? and a number: a ? .5 : 1 */
            if (p[0] == '?' && p[1] == '.' && L->pos + 2 < L->n && L->src[L->pos + 2] >= '0' && L->src[L->pos + 2] <= '9') continue;
            t->t = TK_PUNCT;
            t->p = 0;
            for (k = 0; p[k]; k++) t->p = (t->p << 8) | (u8)p[k];
            L->pos += k;
            return 1;
        }
    }
    return 0;
}

static void regex_word(Lex *L, Tok *t)
{
    u32 start = L->pos, fstart;
    int cls = 0;
    while (L->pos < L->n) {
        u32 c = L->src[L->pos];
        if (c == '\n') { fail(L, "a regular expression has no end"); return; }
        if (c == '\\') { L->pos += 2; continue; }
        if (c == '[') cls = 1;
        else if (c == ']') cls = 0;
        else if (c == '/' && !cls) break;
        L->pos++;
    }
    t->t = TK_REGEX;
    t->s = L->skipping ? S_empty : str_from16(L->src + start, L->pos - start);
    L->pos++;
    fstart = L->pos;
    while (L->pos < L->n && id_part(L->src[L->pos])) L->pos++;
    t->flags = L->skipping ? S_empty : str_from16(L->src + fstart, L->pos - fstart);
}

void lex_next(Lex *L, Tok *t)
{
    u32 c;
    t->nl = skip(L);
    t->pos = L->pos;
    t->line = L->line;
    t->s = 0; t->flags = 0; t->p = 0; t->tail = 1; t->kw = -1;
    if (L->pos >= L->n) { t->t = TK_EOF; t->end = L->pos; return; }
    c = L->src[L->pos];
    if (id_start(c) || c == '\\') {
        u32 start = L->pos;
        int esc = 0;
        while (L->pos < L->n && (id_part(L->src[L->pos]) || L->src[L->pos] == '\\')) { if (L->src[L->pos] == '\\') esc = 1; L->pos++; }
        if (esc) {                          /* \u in a name: rare; decoded plainly */
            u16 b[256]; u32 n = 0, i = start;
            while (i < L->pos && n < 256) {
                if (L->src[i] == '\\' && i + 5 < L->pos + 1 && L->src[i + 1] == 'u') {
                    u32 v = 0, k;
                    for (k = 0; k < 4; k++) { int h = hexv(L->src[i + 2 + k]); v = v * 16 + (u32)(h < 0 ? 0 : h); }
                    b[n++] = (u16)v; i += 6;
                } else b[n++] = L->src[i++];
            }
            t->s = str_intern(str_from16(b, n));
        } else t->s = str_intern16(L->src + start, L->pos - start);
        t->t = TK_IDENT;
        t->kw = kw_of(t->s);
        L->regex_ok = t->kw == KW_RETURN || t->kw == KW_TYPEOF || t->kw == KW_INSTANCEOF || t->kw == KW_IN ||
                      t->kw == KW_OF || t->kw == KW_NEW || t->kw == KW_DELETE || t->kw == KW_VOID ||
                      t->kw == KW_THROW || t->kw == KW_CASE || t->kw == KW_DO || t->kw == KW_ELSE || t->kw == KW_YIELD || t->kw == KW_AWAIT;
        t->end = L->pos;
        return;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && L->pos + 1 < L->n && L->src[L->pos + 1] >= '0' && L->src[L->pos + 1] <= '9')) {
        u32 start = L->pos;
        t->t = TK_NUM;
        if (c == '0' && L->pos + 1 < L->n && ((L->src[L->pos + 1] | 32) == 'x' || (L->src[L->pos + 1] | 32) == 'o' || (L->src[L->pos + 1] | 32) == 'b')) {
            int base = (L->src[L->pos + 1] | 32) == 'x' ? 16 : (L->src[L->pos + 1] | 32) == 'o' ? 8 : 2;
            f64 v = 0;
            L->pos += 2;
            while (L->pos < L->n) {
                u32 d = L->src[L->pos];
                int h = d == '_' ? -2 : hexv(d);
                if (h == -2) { L->pos++; continue; }
                if (h < 0 || h >= base) break;
                v = v * base + h; L->pos++;
            }
            t->num = v;
        } else if (c == '0' && L->pos + 1 < L->n && L->src[L->pos + 1] >= '0' && L->src[L->pos + 1] <= '7') {
            f64 v = 0;                     /* an old octal: 017 */
            L->pos++;
            while (L->pos < L->n && L->src[L->pos] >= '0' && L->src[L->pos] <= '7') v = v * 8 + (L->src[L->pos++] - '0');
            t->num = v;
        } else {
            /* One point at most, and only before the exponent: 1..toString
             * is the number 1. and then .toString. */
            u16 b[400]; u32 n = 0, used;
            int dot = 0, ex = 0;
            while (L->pos < L->n && n < 398) {
                u32 d = L->src[L->pos];
                if (d == '_') { L->pos++; continue; }
                if (d >= '0' && d <= '9') { b[n++] = (u16)d; L->pos++; continue; }
                if (d == '.' && !dot && !ex) { dot = 1; b[n++] = (u16)d; L->pos++; continue; }
                if ((d | 32) == 'e' && !ex) {
                    u32 k = L->pos + 1;
                    if (k < L->n && (L->src[k] == '+' || L->src[k] == '-')) k++;
                    if (k >= L->n || L->src[k] < '0' || L->src[k] > '9') break;   /* not an exponent */
                    ex = 1;
                    b[n++] = (u16)d; L->pos++;
                    if (L->src[L->pos] == '+' || L->src[L->pos] == '-') b[n++] = L->src[L->pos++];
                    continue;
                }
                break;
            }
            t->num = num_parse_prefix(b, n, &used);
            (void)start;
        }
        if (L->pos < L->n && L->src[L->pos] == 'n') L->pos++;   /* a BigInt: kept as a number */
        L->regex_ok = 0;
        t->end = L->pos;
        return;
    }
    if (c == '"' || c == '\'') {
        L->pos++;
        t->t = TK_STR;
        t->s = string_body(L, c, 0);
        L->regex_ok = 0;
        t->end = L->pos;
        return;
    }
    if (c == '`') {
        L->pos++;
        t->t = TK_TEMPLATE;
        t->s = string_body(L, '`', &t->tail);
        L->regex_ok = 0;
        t->end = L->pos;
        return;
    }
    if (c == '/' && L->regex_ok) {
        L->pos++;
        regex_word(L, t);
        L->regex_ok = 0;
        t->end = L->pos;
        return;
    }
    if (punct(L, t)) {
        L->regex_ok = !(t->p == ')' || t->p == ']' || t->p == '}');
        t->end = L->pos;
        return;
    }
    fail(L, "a character that is not JavaScript");
    t->t = TK_EOF;
    t->end = L->pos;
}

/* After the } that closes a ${...} in a template: the next piece. */
void lex_template_more(Lex *L, Tok *t, u32 at)
{
    L->pos = at;
    t->nl = 0;
    t->pos = at;
    t->t = TK_TEMPLATE;
    t->s = string_body(L, '`', &t->tail);
    L->regex_ok = 0;
    t->end = L->pos;
}

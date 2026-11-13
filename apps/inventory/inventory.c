/* ===========================================================================
 * inventory.c -- what you carry: your files, and the folders they are in.
 *
 * INVENTORY.APP on the floppy; typed as `inventory`.  It shows the one
 * folder on the floppy that is yours (INVENTORY, as any other computer
 * sees it) and lets you walk through it, open a document, make a folder,
 * rename, move and drop things.  All of the work is the system's (the
 * inv_ calls in apps/app.h); this is only the list and the keys.
 *
 *   inventory            starts at the top
 *   inventory NOTES      starts in that folder
 *   inventory NOTES/A.MD starts in its folder, with it picked out
 *
 * Moving is picking up and putting down: M picks the thing up, you walk to
 * where it should be, P puts it down there.
 * ===========================================================================
 */
#include "app.h"

#define MAXI 400

static const Api *A;
static int  W, H, ROWS;
static char s_dir[INV_PATH];             /* where we are; "" is the top     */
static InvItem s_it[MAXI];
static int  s_n, s_sel, s_top;
static char s_carry[INV_PATH];           /* what is picked up, by its whole path */
static char s_carry_name[13];
static char s_say[80];
static u8   s_say_c;

/* ---- small things ----------------------------------------------------------- */
static int len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void copy(char *d, const char *s, int cap) { int i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }
static int same(const char *a, const char *b) { for (; *a && *a == *b; a++, b++) { } return *a == *b; }
static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static void say(const char *t, u8 c) { copy(s_say, t, (int)sizeof s_say); s_say_c = c; }

/* dir + "/" + name; 0 if it will not fit. */
static int join(char *out, const char *dir, const char *name)
{
    if (len(dir) + 1 + len(name) >= INV_PATH) return 0;
    if (*dir) A->format(out, INV_PATH, "%s/%s", dir, name);
    else copy(out, name, INV_PATH);
    return 1;
}

static const char *ending(const char *name)
{
    int i, dot = -1;
    for (i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    return dot < 0 ? "" : name + dot + 1;
}

static const char *kind(const InvItem *it)
{
    static const char *const k[][2] = {
        { "MD", "document" }, { "TXT", "text" }, { "MOD", "song" }, { "XM", "song" }, { "S3M", "song" },
        { "IT", "song" }, { "MP3", "sound" }, { "OGG", "sound" }, { "FLA", "sound" }, { "WAV", "sound" },
        { "HTM", "web page" }, { "APP", "program" },
    };
    const char *e = ending(it->name);
    u32 i;
    if (it->folder) return "folder";
    for (i = 0; i < sizeof k / sizeof k[0]; i++) if (same(e, k[i][0])) return k[i][1];
    return "file";
}

static int is_doc(const InvItem *it)
{
    const char *e = ending(it->name);
    return !it->folder && (same(e, "MD") || same(e, "TXT"));
}

/* ---- the list --------------------------------------------------------------- */
static void take(const InvItem *it, void *ctx)
{
    (void)ctx;
    if (s_n < MAXI) s_it[s_n++] = *it;
}

static int before(const InvItem *a, const InvItem *b)
{
    int i;
    if (a->folder != b->folder) return a->folder;
    for (i = 0; a->name[i] && a->name[i] == b->name[i]; i++) { }
    return (u8)a->name[i] < (u8)b->name[i];
}

/* Read the folder again; keep the bar on `pick` if it is there. */
static void look(const char *pick)
{
    int i, k;
    char m[100];
    s_n = 0;
    if (A->inv_list(s_dir, take, 0) < 0) {
        say(A->inv_why(), A_ACCENT);
        if (*s_dir) { s_dir[0] = 0; s_n = 0; A->inv_list(s_dir, take, 0); }
    }
    for (i = 1; i < s_n; i++) {                  /* folders first, then by name */
        InvItem t = s_it[i];
        for (k = i; k > 0 && before(&t, &s_it[k - 1]); k--) s_it[k] = s_it[k - 1];
        s_it[k] = t;
    }
    if (pick) for (i = 0; i < s_n; i++) if (same(s_it[i].name, pick)) s_sel = i;
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
    A->format(m, sizeof m, "[inventory] /%s %u things\n", s_dir, (u32)s_n);
    A->wire(m);
}

static void draw(void)
{
    char m[100];
    int i, x;
    u32 room = A->inv_room();

    A->win_open(A->screen_cols(), A->screen_rows());
    A->win_frame(0, A_GREY);
    x = A->win_text(2, 0, " inventory ", A_BRIGHT, A_PAPER);
    x = A->win_text(x, 0, A->os_version(), A_DIM, A_PAPER);   /* which FLOPOS it came with */
    x = A->win_text(x, 0, " ", A_GREY, A_PAPER);
    A->win_put(x++, 0, A_FR_H, A_GREY, A_PAPER, 0);
    A->format(m, sizeof m, " /%s ", s_dir);
    A->win_text(x, 0, m, A_TEXT, A_PAPER);
    A->format(m, sizeof m, " %u KB free ", room / 1024);
    A->win_text(W - 3 - len(m), H - 1, m, A_DIM, A_PAPER);

    A->win_text(2, 1, "name", A_DIM, A_PAPER);
    A->win_text(16, 1, "kind", A_DIM, A_PAPER);
    A->win_text(32, 1, "size", A_DIM, A_PAPER);
    A->win_text(38, 1, "changed", A_DIM, A_PAPER);

    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + ROWS) s_top = s_sel - ROWS + 1;
    if (!s_n) A->win_text(2, 3, "Nothing here yet.", A_DIM, A_PAPER);
    for (i = 0; i < ROWS && s_top + i < s_n; i++) {
        const InvItem *it = &s_it[s_top + i];
        int y = 2 + i, on = s_top + i == s_sel;
        u8 bg = on ? A_HILITE : A_PAPER;
        if (on) A->win_band(y, 1, W - 2, bg);
        A->win_text(2, y, it->name, it->folder ? A_BRIGHT : A_TEXT, bg);
        A->win_text(16, y, kind(it), A_DIM, bg);
        if (!it->folder) {
            if (it->size < 10000) A->format(m, sizeof m, "%u", it->size);
            else A->format(m, sizeof m, "%u KB", (it->size + 512) / 1024);
            A->win_text(36 - len(m), y, m, A_TEXT, bg);
        }
        if (it->date) {
            A->format(m, sizeof m, "%u-%02u-%02u", 1980u + (it->date >> 9), (u32)(it->date >> 5) & 15, (u32)it->date & 31);
            A->win_text(38, y, m, A_DIM, bg);
        }
    }
    if (s_carry[0]) {
        A->format(m, sizeof m, "Carrying %s. \1P\1 puts it down here.", s_carry_name);
        A->win_text(2, H - 5, m, A_BRIGHT, A_PAPER);
    } else if (s_say[0]) A->win_text(2, H - 5, s_say, s_say_c, A_PAPER);
    A->win_text(2, H - 3, "\1Enter\1 open  \1W\1 write new  \1N\1 new folder  \1R\1 rename", A_DIM, A_PAPER);
    A->win_text(2, H - 2, "\1M\1 move  \1D\1 drop  \1Backspace\1 up a folder  \1Esc\1 leave", A_DIM, A_PAPER);
}

static int wait_key(void)
{
    int key, mods;
    do { key = A->key(&mods); if (key < 0) A->pump(0); } while (key < 0);
    return key;
}

/* A name, typed on the message line.  0 if esc. */
static int ask(const char *what, char *out, int cap, const char *start)
{
    int n, key, x;
    copy(out, start, cap);
    n = len(out);
    for (;;) {
        draw();
        A->win_band(H - 5, 1, W - 2, A_PAPER);
        x = A->win_text(2, H - 5, what, A_BRIGHT, A_PAPER);
        x = A->win_text(x + 1, H - 5, out, A_TEXT, A_HILITE);
        A->win_put(x, H - 5, '_', A_BRIGHT, A_HILITE, 0);
        key = wait_key();
        if (key == K_ESC) return 0;
        if (key == K_ENTER) return n > 0;
        if (key == K_BKSP) { if (n) out[--n] = 0; continue; }
        key = up(key);
        if (n < cap - 1 && ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == '.' || key == '_' || key == '-')) {
            out[n++] = (char)key;
            out[n] = 0;
        }
    }
}

static void up_a_folder(void)
{
    char was[13];
    int i, cut = -1;
    if (!*s_dir) return;
    for (i = 0; s_dir[i]; i++) if (s_dir[i] == '/') cut = i;
    copy(was, s_dir + cut + 1, (int)sizeof was);
    s_dir[cut < 0 ? 0 : cut] = 0;
    s_sel = s_top = 0;
    look(was);
}

static void told(int ok, const char *done)
{
    if (ok) say(done, A_TEXT); else say(A->inv_why(), A_ACCENT);
    A->wire(ok ? "[inventory] ok\n" : "[inventory] no\n");
}

/* ---- the app ---------------------------------------------------------------- */
int app_main(const Api *api, int argc, char **argv)
{
    char path[INV_PATH], to[INV_PATH], name[13], m[100];
    const char *pick = 0;
    InvItem st;
    int key, i;

    A = api;
    A->win_open(A->screen_cols(), A->screen_rows());
    W = A->win_width(); H = A->win_height();
    ROWS = H - 8;

    /* Where to start: a folder, or a file's folder with the file picked out. */
    if (argc >= 2 && argv[1][0] && A->inv_stat(argv[1], &st)) {
        copy(s_dir, argv[1], INV_PATH);
        for (i = 0; s_dir[i]; i++) s_dir[i] = (char)up(s_dir[i]);
        while (len(s_dir) && s_dir[len(s_dir) - 1] == '/') s_dir[len(s_dir) - 1] = 0;
        if (!st.folder) {
            int cut = -1;
            for (i = 0; s_dir[i]; i++) if (s_dir[i] == '/') cut = i;
            copy(name, s_dir + cut + 1, (int)sizeof name);
            s_dir[cut < 0 ? 0 : cut] = 0;
            pick = name;
        }
    }
    look(pick);

    for (;;) {
        const InvItem *it = s_n ? &s_it[s_sel] : 0;
        draw();
        key = wait_key();
        if (key != 'p' && key != 'P') s_say[0] = 0;
        switch (key) {
        case K_UP:   if (s_sel > 0) s_sel--; break;
        case K_DOWN: if (s_sel < s_n - 1) s_sel++; break;
        case K_PGUP: s_sel = s_sel > ROWS ? s_sel - ROWS : 0; break;
        case K_PGDN: s_sel = s_sel + ROWS < s_n ? s_sel + ROWS : (s_n ? s_n - 1 : 0); break;
        case K_HOME: s_sel = 0; break;
        case K_END:  s_sel = s_n ? s_n - 1 : 0; break;
        case K_BKSP: case K_LEFT: up_a_folder(); break;
        case K_ESC:
            if (s_carry[0]) { s_carry[0] = 0; say("Put back where it was.", A_TEXT); break; }
            A->win_close();
            return 0;
        case K_ENTER: case K_RIGHT:
            if (!it) break;
            if (!join(path, s_dir, it->name)) { say("That is too many folders deep.", A_ACCENT); break; }
            if (it->folder) { copy(s_dir, path, INV_PATH); s_sel = s_top = 0; look(0); }
            else if (is_doc(it) && key == K_ENTER) {
                A->format(m, sizeof m, "scribe %s back", path);
                A->then(m);
                A->win_close();
                return 0;
            } else if (key == K_ENTER) say("Nothing here opens that kind of thing yet.", A_TEXT);
            break;
        case 'w': case 'W':                      /* a new document, to be kept in this folder */
            A->format(m, sizeof m, "scribe %s/ back", s_dir);
            A->then(m);
            A->win_close();
            return 0;
        case 'n': case 'N':
            if (!ask("New folder:", name, 9, "")) break;
            if (!join(path, s_dir, name)) { say("That is too many folders deep.", A_ACCENT); break; }
            told(A->inv_mkdir(path), "Made.");
            look(name);
            break;
        case 'r': case 'R':
            if (!it) break;
            copy(to, it->name, (int)sizeof to);
            if (!ask("New name:", name, 13, to) || same(name, it->name)) break;
            if (!join(path, s_dir, it->name) || !join(to, s_dir, name)) break;
            told(A->inv_move(path, to), "Renamed.");
            look(name);
            break;
        case 'm': case 'M':
            if (!it || !join(s_carry, s_dir, it->name)) break;
            copy(s_carry_name, it->name, (int)sizeof s_carry_name);
            break;
        case 'p': case 'P':
            if (!s_carry[0]) break;
            if (!join(to, s_dir, s_carry_name)) { say("That is too many folders deep.", A_ACCENT); break; }
            if (same(to, s_carry)) { s_carry[0] = 0; say("It is already here.", A_TEXT); break; }
            told(A->inv_move(s_carry, to), "Put down here.");
            s_carry[0] = 0;
            look(s_carry_name);
            break;
        case 'd': case 'D': case K_DEL:
            if (!it || !join(path, s_dir, it->name)) break;
            A->format(m, sizeof m, "Drop %s for good?  \1Y\1 yes, any other key no.", it->name);
            say(m, A_ACCENT);
            draw();
            key = wait_key();
            s_say[0] = 0;
            if (key != 'y' && key != 'Y') break;
            if (same(path, s_carry)) s_carry[0] = 0;
            told(A->inv_remove(path), "Dropped.");
            look(0);
            break;
        default: break;
        }
    }
}

/* ===========================================================================
 * store.c -- what FLOPOS remembers.
 *
 * Two files are laid down on the floppy at build time, each a fixed size
 * and in one piece: JOURNAL.TXT, which is plain text anybody can read on
 * any machine, and STATE.BIN, which holds the settings and the count of boots.
 *
 * Because both files never change length, FLOPOS overwrites their data
 * sectors in place and never touches the FAT or the directory.  Where they
 * are is looked up by name at boot (fs_one_piece), not taken from the
 * build: deleted and copied back on another computer, they move.
 *
 * Nothing is written unless something asks.  A command that saves says so.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"
#include "gen/version.h"

#define SECTOR 512

static u8 *s_journal;                    /* the text file, in RAM          */
static u32 s_journal_len;                /* bytes used                     */
static u32 s_journal_cap;
static int s_journal_dirty;

static u8 *s_state;                      /* STATE.BIN, in RAM              */
static int s_state_dirty;

static int s_ready;
static u32 s_jlba, s_jsec, s_slba, s_ssec;  /* where the two files really are */

int store_ready(void) { return s_ready; }
u32 store_journal_used(void) { return s_journal_len; }
u32 store_journal_size(void) { return s_journal_cap; }

/* ---- sectors ----------------------------------------------------------
 * As many sectors per BIOS call as the geometry allows, which is up to a
 * whole track.  Every call is a round trip out of protected mode and back
 * with the interrupt controller handed over and returned, so doing this a
 * sector at a time turns sixteen kilobytes into thirty-two of those --
 * slow enough on a real drive to look like a hang.
 */
/* ---- the journal ------------------------------------------------------
 * Plain text, terminated by the DOS end-of-file character so that TYPE,
 * Notepad and cat all stop in the right place, then padded with zeroes.
 */
#define EOF_MARK 0x1A

static void journal_find_end(void)
{
    u32 i;
    s_journal_len = 0;
    for (i = 0; i + 1 < s_journal_cap; i++) {         /* room kept for the end mark */
        if (s_journal[i] == EOF_MARK || s_journal[i] == 0) break;
    }
    s_journal_len = i;
}

void store_journal_line(const char *s)
{
    u32 n;

    if (!s_ready) return;
    for (n = 0; s[n]; n++) { }

    /* Full is full.  Overwriting the oldest entries would lose exactly the
     * part that is worth keeping, so it stops instead and says so. */
    if (s_journal_len + n + 2 >= s_journal_cap) return;

    memcpy8(s_journal + s_journal_len, s, n);
    s_journal_len += n;
    s_journal[s_journal_len++] = '\r';   /* CRLF: it will be read on a PC  */
    s_journal[s_journal_len++] = '\n';
    s_journal_dirty = 1;
}

void store_journal_printf(const char *fmt, ...)
{
    char buf[160];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    fmt_vstr(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    store_journal_line(buf);
}

const char *store_journal_text(void) { return (const char *)s_journal; }

/* ---- load and save ---------------------------------------------------- */
typedef struct {
    char magic[8];
    u32  version;
    u32  count;
    u32  checksum;
    u32  boots;
} StateHdr;

static u32 s_boots;
u32 store_boots(void) { return s_boots; }

/* Settings live at the far end of STATE.BIN, each with its own mark and
 * checksum, so a file of any length from 512 bytes up keeps them (an
 * 8 KB one from before 1.49 too).  An older floppy has zeros there and so
 * simply has no settings. */
typedef struct {
    char magic[4];                       /* "SETS"                         */
    u32  net_how;                        /* 0 automatic, 1 by hand         */
    u32  ip, mask, gw, dns;
    u32  sum;
} Settings;

static Settings *s_set;

/* The clock's reading at the last goodbye, just before the settings. */
typedef struct {
    char magic[4];                       /* "TIME"                         */
    u32  when;                           /* seconds since 2000             */
    u32  sum;
} SavedClock;

static SavedClock *s_clk;

u32 store_clock_get(void)
{
    if (!s_ready || !s_clk) return 0;
    if (s_clk->magic[0] != 'T' || s_clk->magic[1] != 'I' || s_clk->magic[2] != 'M' ||
        s_clk->magic[3] != 'E' || s_clk->sum != (s_clk->when ^ 0x7153C10Cu)) return 0;
    return s_clk->when;
}

void store_clock_set(u32 s)
{
    if (!s_ready || !s_clk) return;
    s_clk->magic[0] = 'T'; s_clk->magic[1] = 'I'; s_clk->magic[2] = 'M'; s_clk->magic[3] = 'E';
    s_clk->when = s;
    s_clk->sum = s ^ 0x7153C10Cu;
    s_state_dirty = 1;
}

/* The keyboard, before the clock.  It carries the version it was chosen
 * under: a floppy written with a new release starts on Swedish again,
 * even though writefloppy carries this file over. */
typedef struct {
    char magic[4];                       /* "KEYS"                         */
    char code[4];                        /* "de", "fr"...                  */
    char version[16];                    /* FLOPOS_VERSION when chosen     */
    u32  sum;
} SavedKeys;

static SavedKeys *s_keys;

static u32 keys_sum(const SavedKeys *k)
{
    u32 sum = 0x4B455953u;
    const u8 *p = (const u8 *)k;
    u32 i;
    for (i = 0; i < (u32)__builtin_offsetof(SavedKeys, sum); i++) sum = (sum << 3 | sum >> 29) ^ p[i];
    return sum;
}

const char *store_keys_get(void)
{
    if (!s_ready || !s_keys) return 0;
    if (s_keys->magic[0] != 'K' || s_keys->magic[1] != 'E' || s_keys->magic[2] != 'Y' ||
        s_keys->magic[3] != 'S' || s_keys->sum != keys_sum(s_keys)) return 0;
    {
        const char *a = s_keys->version, *b = FLOPOS_VERSION;
        while (*a && *a == *b) { a++; b++; }
        if (*a || *b) return 0;          /* another release chose it      */
    }
    s_keys->code[3] = 0;
    return s_keys->code;
}

int store_keys_set(const char *code)
{
    int i;
    if (!s_ready || !s_keys) return 0;
    memset8(s_keys, 0, sizeof *s_keys);
    s_keys->magic[0] = 'K'; s_keys->magic[1] = 'E'; s_keys->magic[2] = 'Y'; s_keys->magic[3] = 'S';
    for (i = 0; i < 3 && code[i]; i++) s_keys->code[i] = code[i];
    for (i = 0; i < 15 && FLOPOS_VERSION[i]; i++) s_keys->version[i] = FLOPOS_VERSION[i];
    s_keys->sum = keys_sum(s_keys);
    s_state_dirty = 1;
    return store_save();
}

/* The clock's settings (nts.c, clockui.c): how far to trust the network's
 * time, and the signed servers to ask, in order.  Its own mark and sum. */
typedef struct {
    char magic[4];                       /* "CLOK"                         */
    u32  trust;
    char server[3][64];
    u32  sum;
} ClockSet;

static ClockSet *s_clkset;

static u32 clockset_sum(const ClockSet *c)
{
    u32 sum = 0x434C4F4Bu, i;
    const u8 *p = (const u8 *)c;
    for (i = 0; i < (u32)__builtin_offsetof(ClockSet, sum); i++) sum = (sum << 5 | sum >> 27) ^ p[i];
    return sum;
}

int store_clockset_get(u32 *trust, char server[3][64])
{
    int i, k;
    if (!s_ready || !s_clkset) return 0;
    if (s_clkset->magic[0] != 'C' || s_clkset->magic[1] != 'L' || s_clkset->magic[2] != 'O' ||
        s_clkset->magic[3] != 'K' || s_clkset->sum != clockset_sum(s_clkset)) return 0;
    *trust = s_clkset->trust;
    for (i = 0; i < 3; i++) {
        for (k = 0; k < 63 && s_clkset->server[i][k]; k++) server[i][k] = s_clkset->server[i][k];
        server[i][k] = 0;
    }
    return 1;
}

int store_clockset_set(u32 trust, const char server[3][64])
{
    int i, k;
    if (!s_ready || !s_clkset) return 0;
    memset8(s_clkset, 0, sizeof *s_clkset);
    s_clkset->magic[0] = 'C'; s_clkset->magic[1] = 'L'; s_clkset->magic[2] = 'O'; s_clkset->magic[3] = 'K';
    s_clkset->trust = trust;
    for (i = 0; i < 3; i++)
        for (k = 0; k < 63 && server[i][k]; k++) s_clkset->server[i][k] = server[i][k];
    s_clkset->sum = clockset_sum(s_clkset);
    s_state_dirty = 1;
    return store_save();
}

static u32 settings_sum(const Settings *t)
{
    return 0x5E7u ^ t->net_how ^ t->ip ^ (t->mask * 3) ^ (t->gw * 5) ^ (t->dns * 7);
}

int store_net_get(u32 *how, u32 *ip, u32 *mask, u32 *gw, u32 *dns)
{
    if (!s_ready || !s_set) return 0;
    if (s_set->magic[0] != 'S' || s_set->magic[1] != 'E' ||
        s_set->magic[2] != 'T' || s_set->magic[3] != 'S') return 0;
    if (settings_sum(s_set) != s_set->sum) return 0;
    *how = s_set->net_how; *ip = s_set->ip; *mask = s_set->mask;
    *gw = s_set->gw; *dns = s_set->dns;
    return 1;
}

int store_net_set(u32 how, u32 ip, u32 mask, u32 gw, u32 dns)
{
    if (!s_ready || !s_set) return 0;
    s_set->magic[0] = 'S'; s_set->magic[1] = 'E';
    s_set->magic[2] = 'T'; s_set->magic[3] = 'S';
    s_set->net_how = how; s_set->ip = ip; s_set->mask = mask;
    s_set->gw = gw; s_set->dns = dns;
    s_set->sum = settings_sum(s_set);
    s_state_dirty = 1;
    return store_save();
}

/* A file's sectors into memory.  A sector a power cut left unreadable
 * comes back as zeros and the file is marked for writing, so the next
 * save mends it; otherwise it would stay unreadable, and nothing would
 * ever be saved again.  Only the first sector must read: it says what the
 * file is. */
static int read_mend(u32 lba, u32 n, u8 *dst, int *dirty)
{
    u32 i;
    if (disk_read_run(lba, n, dst)) return 1;
    for (i = 0; i < n; i++) {
        if (disk_read_run(lba + i, 1, dst + i * SECTOR)) continue;
        if (i == 0) return 0;
        memset8(dst + i * SECTOR, 0, SECTOR);
        *dirty = 1;
    }
    return 1;
}

/* Both files, from the floppy into memory, and checked.  0 leaves the
 * store read-only. */
static int read_in(void)
{
    StateHdr *h = (StateHdr *)s_state;

    if (!read_mend(s_jlba, s_jsec, s_journal, &s_journal_dirty)) return 0;
    if (!read_mend(s_slba, s_ssec, s_state, &s_state_dirty)) return 0;

    /* A JOURNAL.TXT that is not FLOPOS's journal is left alone: read-only. */
    {
        static const char sig[] = "FLOPOS journal";
        int k;
        for (k = 0; sig[k]; k++)
            if (s_journal[k] != (u8)sig[k]) return 0;
    }

    journal_find_end();

    s_boots = 0;
    if (h->magic[0] == 'F' && h->magic[1] == 'L' && h->version == 1) s_boots = h->boots;
    return 1;
}

void store_init(void)
{

    s_ready = 0;
    /* Where the files are now, not where the build put them: deleted
     * and copied back on another computer, they move, and the old place
     * may by now hold a file of the inventory.  Changed in size or no
     * longer in one piece, they are left alone: read-only. */
    if (!fs_one_piece("JOURNAL.TXT", &s_jlba, &s_jsec) || !fs_one_piece("STATE.BIN", &s_slba, &s_ssec)) return;
    if (s_ssec * SECTOR < 512) return;

    s_journal_cap = s_jsec * SECTOR;
    s_journal = (u8 *)halloc(s_journal_cap);
    s_state   = (u8 *)halloc(s_ssec * SECTOR);
    if (!s_journal || !s_state) { s_journal = 0; return; }
    s_set     = (Settings *)(s_state + s_ssec * SECTOR - 64);
    s_clk     = (SavedClock *)(s_state + s_ssec * SECTOR - 128);
    s_keys    = (SavedKeys *)(s_state + s_ssec * SECTOR - 192);
    s_clkset  = (ClockSet *)(s_state + s_ssec * SECTOR - 448);

    if (!read_in()) return;
    s_boots++;
    s_state_dirty = 1;                  /* the boot count changed         */
    s_ready = 1;
}

/* The two files were written from outside (an app put back a copy from a
 * tape): believe the floppy over memory.  Nothing here is marked for
 * writing, since memory and the floppy now agree. */
void store_reload(void)
{
    u32 jl, js, sl, ss;
    if (!s_journal) return;
    s_ready = 0;
    if (!fs_one_piece("JOURNAL.TXT", &jl, &js) || !fs_one_piece("STATE.BIN", &sl, &ss) ||
        js != s_jsec || ss != s_ssec) return;
    s_jlba = jl; s_slba = sl;
    if (!read_in()) return;
    s_journal_dirty = 0;
    s_state_dirty = 0;
    s_ready = 1;
}

int store_save(void)
{
    StateHdr *h = (StateHdr *)s_state;
    int ok = 1;

    if (!s_ready) return 0;

    if (s_journal_dirty) {
        /* Terminate, then clear everything after so a shorter journal
         * never leaves the tail of a longer one behind it. */
        u32 i;
        s_journal[s_journal_len] = EOF_MARK;
        for (i = s_journal_len + 1; i < s_journal_cap; i++) s_journal[i] = 0;
        if (!disk_write_run(s_jlba, s_jsec, s_journal))
            ok = 0;
        else
            s_journal_dirty = 0;
    }

    if (s_state_dirty) {
        h->magic[0] = 'F'; h->magic[1] = 'L'; h->magic[2] = 'O';
        h->magic[3] = 'P'; h->magic[4] = 'S'; h->magic[5] = 'T';
        h->magic[6] = 0;   h->magic[7] = 0;
        h->version  = 1;
        h->count    = 0;                 /* the taught words, gone since 1.32.0 */
        h->boots    = s_boots;
        h->checksum = 0x9E3779B9u;       /* their checksum, of nothing     */
        if (!disk_write_run(s_slba, s_ssec, s_state))
            ok = 0;
        else
            s_state_dirty = 0;
    }

    return ok;
}

int store_unsaved(void) { return s_journal_dirty || s_state_dirty; }

/* ===========================================================================
 * store.c -- what FLOPOS remembers.
 *
 * Two files are laid down on the floppy at build time, each a fixed size
 * and in one piece: JOURNAL.TXT, which is plain text anybody can read on
 * any machine, and STATE.BIN, which holds the settings and the count of boots.
 *
 * Because both files never change length, FLOPOS can overwrite their data
 * sectors in place and never touch the FAT or the directory.  That is the
 * whole reason there is no filesystem driver here: the build already did
 * the filesystem's job, and left behind the two numbers -- a starting
 * sector and a length -- that the kernel needs.  tools/mkimage.py patches
 * those into the signature block below.
 *
 * Nothing is written unless something asks.  A command that saves says so.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

/* mkimage.py finds this by its signature and fills in the numbers.  The
 * volatile stops the compiler deciding the zeroes are the final word. */
volatile DiskMap g_diskmap = {
    { 'F','L','O','P','O','S','-','D','I','S','K','M','A','P','!',0 },
    0, 0, 0, 0
};

#define SECTOR 512

static u8 *s_journal;                    /* the text file, in RAM          */
static u32 s_journal_len;                /* bytes used                     */
static u32 s_journal_cap;
static int s_journal_dirty;

static Taught *s_taught;
static int     s_taught_n;
static int     s_taught_dirty;

static int s_ready;

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
    for (i = 0; i < s_journal_cap; i++) {
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

/* ---- taught words -------------------------------------------------------
 * FLOPOS could once be taught words of its own.  That is gone (1.32.0),
 * but the table keeps its place in STATE.BIN, so a floppy from before
 * still reads, and what follows it in the file stays where it was. */
static u32 taught_checksum(void)
{
    u32 sum = 0x9E3779B9u;
    const u8 *p = (const u8 *)s_taught;
    u32 i, n = (u32)(s_taught_n * (int)sizeof(Taught));
    for (i = 0; i < n; i++) sum = (sum << 1 | sum >> 31) ^ p[i];
    return sum;
}

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

/* Settings live at the far end of STATE.BIN, well clear of the taught
 * words, with their own mark and checksum.  An older floppy has zeros
 * there and so simply has no settings; nothing about the words changes. */
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
    s_taught_dirty = 1;
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
    s_taught_dirty = 1;
    return store_save();
}

/* Both files, from the floppy into memory, and checked.  0 leaves the
 * store read-only. */
static int read_in(void)
{
    u8 *statebuf = (u8 *)s_taught - sizeof(StateHdr);
    StateHdr *h = (StateHdr *)statebuf;
    int i;

    if (!disk_read_run(g_diskmap.journal_lba, g_diskmap.journal_sectors, s_journal))
        return 0;
    if (!disk_read_run(g_diskmap.state_lba, g_diskmap.state_sectors, statebuf))
        return 0;

    /* Before trusting any of this enough to write to it, check that the
     * sectors the build pointed us at really are the journal.  If the disk
     * map were ever wrong, a write would land somewhere else on the floppy
     * -- and the only copy of the kernel is on that same floppy.  A wrong
     * address now fails safe and read-only instead of eating the disk. */
    {
        static const char sig[] = "FLOPOS journal";
        int k;
        for (k = 0; sig[k]; k++)
            if (s_journal[k] != (u8)sig[k]) return 0;
    }

    journal_find_end();

    s_taught_n = 0;
    s_boots = 0;
    if (h->magic[0] == 'F' && h->magic[1] == 'L' && h->version == 1) {
        if (h->count <= TAUGHT_MAX) {
            s_taught_n = (int)h->count;
            s_boots = h->boots;
            if (taught_checksum() != h->checksum) s_taught_n = 0;
        }
    }
    for (i = s_taught_n; i < TAUGHT_MAX; i++) s_taught[i].word[0] = 0;
    /* Every word and answer ends, whatever the file said. */
    for (i = 0; i < s_taught_n; i++) {
        s_taught[i].word[WORD_MAX - 1] = 0;
        s_taught[i].resp[RESP_MAX - 1] = 0;
    }
    return 1;
}

void store_init(void)
{
    u8 *statebuf;

    s_ready = 0;
    if (g_diskmap.journal_sectors == 0 || g_diskmap.state_sectors == 0) return;

    s_journal_cap = g_diskmap.journal_sectors * SECTOR;
    s_journal = (u8 *)halloc(s_journal_cap);
    statebuf  = (u8 *)halloc(g_diskmap.state_sectors * SECTOR);
    s_taught  = (Taught *)(statebuf + sizeof(StateHdr));
    s_set     = (Settings *)(statebuf + g_diskmap.state_sectors * SECTOR - 64);
    s_clk     = (SavedClock *)(statebuf + g_diskmap.state_sectors * SECTOR - 128);

    if (!read_in()) return;
    s_boots++;
    s_taught_dirty = 1;                  /* the boot count changed         */
    s_ready = 1;
}

/* The two files were written from outside (an app put back a copy from a
 * tape): believe the floppy over memory.  Nothing here is marked for
 * writing, since memory and the floppy now agree. */
void store_reload(void)
{
    if (!s_journal) return;
    s_ready = 0;
    if (!read_in()) return;
    s_journal_dirty = 0;
    s_taught_dirty = 0;
    s_ready = 1;
}

int store_save(void)
{
    u8 *statebuf = (u8 *)s_taught - sizeof(StateHdr);
    StateHdr *h = (StateHdr *)statebuf;
    int ok = 1;

    if (!s_ready) return 0;

    if (s_journal_dirty) {
        /* Terminate, then clear everything after so a shorter journal
         * never leaves the tail of a longer one behind it. */
        u32 i;
        s_journal[s_journal_len] = EOF_MARK;
        for (i = s_journal_len + 1; i < s_journal_cap; i++) s_journal[i] = 0;
        if (!disk_write_run(g_diskmap.journal_lba, g_diskmap.journal_sectors,
                       s_journal))
            ok = 0;
        else
            s_journal_dirty = 0;
    }

    if (s_taught_dirty) {
        h->magic[0] = 'F'; h->magic[1] = 'L'; h->magic[2] = 'O';
        h->magic[3] = 'P'; h->magic[4] = 'S'; h->magic[5] = 'T';
        h->magic[6] = 0;   h->magic[7] = 0;
        h->version  = 1;
        h->count    = (u32)s_taught_n;
        h->boots    = s_boots;
        h->checksum = taught_checksum();
        if (!disk_write_run(g_diskmap.state_lba, g_diskmap.state_sectors, statebuf))
            ok = 0;
        else
            s_taught_dirty = 0;
    }

    return ok;
}

int store_unsaved(void) { return s_journal_dirty || s_taught_dirty; }

/* ===========================================================================
 * fs.c -- files on the floppy, by name.
 *
 * Until now FLOPOS only knew where its own few files were because the
 * build wrote their addresses into the kernel.  Programs that are not
 * part of the kernel need finding the ordinary way: read the floppy's
 * directory, find the name, follow the file allocation table from cluster
 * to cluster.  FAT12, the format every 1.44MB floppy has used since DOS,
 * so a file copied onto this disk from any other computer is found too.
 *
 * The floppy has two parts, and they are treated differently.
 *
 * The system's own files sit in the top folder, where the build put them.
 * They are only read, or rewritten in place inside their own clusters
 * (fs_write_inplace): nothing about their size or where they live ever
 * changes, and the top folder itself is never written.
 *
 * Everything that is yours lives in one folder, INVENTORY, and below it.
 * There FLOPOS does the whole of FAT12: makes files and folders, grows
 * and shrinks them, renames, moves and removes them (the inv_ calls).
 * Every path they take is followed down from that folder and cannot leave
 * it.  Any other computer sees an ordinary folder of ordinary files.
 *
 * Writing goes in an order that a power cut cannot turn into a damaged
 * disk: a file's new contents first, into clusters nothing uses; then the
 * allocation table, its spare copy before the one in use; then the
 * folder's entry, which is the moment the change becomes true; and only
 * then are the old contents let go.  Cut anywhere, a file is whole, old
 * or new.  The worst left behind is a few clusters marked used that
 * nothing owns, which any disk checker returns.  (Two exceptions, both
 * told plainly in docs/inventory.md: with less room free than the new
 * copy needs, the old one has to give way first; and a move between
 * folders cut halfway leaves the thing listed in both.)
 * ===========================================================================
 */
#ifdef FS_HOST                           /* built for this PC, by tools/fstest */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef struct { u32 first, size; } FsFile;
typedef struct { char name[13]; u8 folder; u16 date, time; u32 size; } FsItem;
int   disk_read_run(u32 lba, u32 sectors, u8 *dst);
int   disk_write_run(u32 lba, u32 sectors, const u8 *src);
void *halloc(u32 bytes);
int   rtc_read(u8 *hour, u8 *min, u8 *sec, u16 *year, u8 *month, u8 *day);
static void memcpy8(void *d, const void *s, u32 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
#else
#include "flopos.h"
#include "io.h"
#endif

static int s_mounted;
static u32 s_spc, s_fat_lba, s_root_lba, s_root_n, s_data_lba, s_clusters;
static u8 *s_fat;                        /* the whole first FAT            */
static u8 *s_root;                       /* the whole root directory       */
static u8 s_boot[512];
static u32 s_fats, s_spf;                /* how many copies of the FAT, and how long each */
static u8 *s_spare;                      /* room for the second copy, when it is checked */
#define FAT_SECS 16                      /* a floppy's FAT is 9 sectors    */
static u8  s_fat_dirty[FAT_SECS];        /* changed in memory, not yet on the disk */
static u8  s_fat_mend[FAT_SECS];         /* unreadable in the first copy; taken from the spare */
static int s_checked;                    /* the two copies have been made to agree */
static u32 s_inv;                        /* the inventory folder's first cluster, 0: none */
static const char *s_why = "";

static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/* The layout comes from the floppy's own boot sector, not from what a
 * 1.44MB disk is supposed to look like. */
int fs_mount(void)
{
    u32 reserved, fats, spf, total, i;

    if (s_mounted) return 1;
    if (!disk_read_run(0, 1, s_boot)) return 0;
    if (get16(s_boot + 11) != 512) return 0;
    s_spc = s_boot[13];
    reserved = get16(s_boot + 14);
    fats = s_boot[16];
    s_root_n = get16(s_boot + 17);
    total = get16(s_boot + 19);
    spf = get16(s_boot + 22);
    if (!s_spc || !fats || !spf || !s_root_n || !total) return 0;

    s_fat_lba = reserved;
    s_root_lba = reserved + fats * spf;
    s_data_lba = s_root_lba + (s_root_n * 32 + 511) / 512;
    s_clusters = (total - s_data_lba) / s_spc;

    s_fats = fats; s_spf = spf;
    if (!s_fat) {
        s_fat = (u8 *)halloc(spf * 512);
        s_spare = (u8 *)halloc(spf * 512);
        s_root = (u8 *)halloc(((s_root_n * 32 + 511) / 512) * 512);
    }
    for (i = 0; i < FAT_SECS; i++) s_fat_dirty[i] = 0;
    if (!disk_read_run(s_fat_lba, spf, s_fat)) {
        /* A sector of the table will not read: a write to it was cut
         * short.  The spare copy is written first, so it has that sector
         * whole. */
        if (fats < 2 || spf > FAT_SECS) return 0;
        for (i = 0; i < spf; i++) {
            if (disk_read_run(s_fat_lba + i, 1, s_fat + i * 512)) continue;
            if (!disk_read_run(s_fat_lba + spf + i, 1, s_fat + i * 512)) return 0;
            s_fat_mend[i] = 1;
            s_checked = 0;
        }
    }
    if (!disk_read_run(s_root_lba, (s_root_n * 32 + 511) / 512, s_root)) return 0;

    /* The folder that is yours: INVENTORY, under the short name every
     * FAT12 reader sees it by. */
    s_inv = 0;
    for (i = 0; i < s_root_n; i++) {
        const u8 *e = s_root + i * 32;
        static const char inv[] = "INVENT";
        u32 k;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || (e[11] & 0x18) != 0x10) continue;
        for (k = 0; k < 6 && e[k] == (u8)inv[k]; k++) { }
        if (k == 6) { s_inv = get16(e + 26); break; }
    }
    if (s_inv < 2 || s_inv >= s_clusters + 2 || spf > FAT_SECS) s_inv = 0;
    s_mounted = 1;
    return 1;
}

static u32 next_cluster(u32 c)
{
    u32 off = c + c / 2, v = get16(s_fat + off);
    return (c & 1) ? v >> 4 : v & 0xFFF;
}

/* "write.app" -> "WRITE   APP" */
static int to83(const char *name, char out[11])
{
    int i = 0, n;
    for (n = 0; n < 11; n++) out[n] = ' ';
    for (n = 0; name[i] && name[i] != '.' && n < 8; i++, n++)
        out[n] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
    if (name[i] && name[i] != '.') return 0;            /* name too long  */
    if (name[i] == '.') i++;
    for (n = 8; name[i] && n < 11; i++, n++)
        out[n] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
    return name[i] == 0;
}

int fs_find(const char *name, FsFile *f)
{
    char want[11];
    u32 i;
    int k;

    if (!fs_mount() || !to83(name, want)) return 0;
    for (i = 0; i < s_root_n; i++) {
        const u8 *e = s_root + i * 32;
        if (e[0] == 0x00) break;                         /* end of the list */
        if (e[0] == 0xE5) continue;                      /* deleted         */
        if (e[11] & 0x18) continue;                      /* label or folder */
        for (k = 0; k < 11 && e[k] == (u8)want[k]; k++) { }
        if (k < 11) continue;
        f->first = get16(e + 26);
        f->size = get32(e + 28);
        return 1;
    }
    return 0;
}

/* Visit every file, for listings. */
void fs_each(void (*fn)(const char *name, u32 size, void *ctx), void *ctx)
{
    u32 i;
    if (!fs_mount()) return;
    for (i = 0; i < s_root_n; i++) {
        const u8 *e = s_root + i * 32;
        char name[13];
        int k, n = 0;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || (e[11] & 0x18)) continue;
        for (k = 0; k < 8 && e[k] != ' '; k++) name[n++] = (char)e[k];
        if (e[8] != ' ') {
            name[n++] = '.';
            for (k = 8; k < 11 && e[k] != ' '; k++) name[n++] = (char)e[k];
        }
        name[n] = 0;
        fn(name, get32(e + 28), ctx);
    }
}

/* Walk the file's chain a run of consecutive clusters at a time, so a file
 * that is all in one piece -- every file on a disk this build made -- is
 * a handful of track reads.  `write` chooses which way the data goes. */
static int walk(const FsFile *f, u8 *buf, u32 bytes, int write)
{
    u32 c = f->first, done = 0, guard = 0, per = s_spc * 512;

    while (done < bytes) {
        u32 start = c, run = 1, lba, sectors, take;
        if (c < 2 || c >= s_clusters + 2) return 0;
        while (done + run * per < bytes && next_cluster(c) == c + 1 && run < 64) {
            c++;
            run++;
        }
        lba = s_data_lba + (start - 2) * s_spc;
        sectors = run * s_spc;
        take = sectors * 512;
        if (take > bytes - done) {
            /* The last piece: a whole number of sectors through a bounce
             * buffer, so nothing past the end of buf is read or written. */
            static u8 tail[512];
            u32 full = (bytes - done) / 512, rest = (bytes - done) % 512;
            if (full) {
                if (write ? !disk_write_run(lba, full, buf + done)
                          : !disk_read_run(lba, full, buf + done)) return 0;
                done += full * 512;
                lba += full;
            }
            if (rest) {
                if (write) {
                    u32 k;
                    for (k = 0; k < 512; k++) tail[k] = k < rest ? buf[done + k] : 0;
                    if (!disk_write_run(lba, 1, tail)) return 0;
                } else {
                    if (!disk_read_run(lba, 1, tail)) return 0;
                    memcpy8(buf + done, tail, rest);
                }
                done += rest;
            }
            break;
        }
        if (write ? !disk_write_run(lba, sectors, buf + done)
                  : !disk_read_run(lba, sectors, buf + done)) return 0;
        done += take;
        c = next_cluster(c);
        if (++guard > s_clusters) return 0;
        if (c >= 0xFF8 && done < bytes) return 0;        /* chain too short */
    }
    return 1;
}

int fs_read(const FsFile *f, u8 *buf, u32 max)
{
    u32 n = f->size < max ? f->size : max;
    if (!fs_mount()) return -1;
    if (n == 0) return 0;
    return walk(f, buf, n, 0) ? (int)n : -1;
}

/* Exactly the file's size, into exactly the file's clusters.  Callers pad
 * their data to f->size first. */
int fs_write_inplace(const FsFile *f, const u8 *buf)
{
    if (!fs_mount() || f->size == 0) return 0;
    return walk(f, (u8 *)buf, f->size, 1);
}

/* ===========================================================================
 * The inventory: the whole of FAT12, inside one folder.
 * ===========================================================================
 */
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

const char *inv_why(void) { return s_why; }
static int no(const char *why) { s_why = why; return 0; }

static u8  s_sec[512];                   /* one sector of a folder, in hand */
static u32 s_sec_lba = ~0u;
static int s_io;                         /* the floppy failed to read       */

/* Something went wrong halfway: forget what was changed in memory and
 * believe the floppy again. */
static int bail(const char *why)
{
    s_mounted = 0;
    s_sec_lba = ~0u;
    return no(why);
}

static int sec_load(u32 lba)
{
    if (s_sec_lba == lba) return 1;
    s_sec_lba = ~0u;
    if (!disk_read_run(lba, 1, s_sec)) { s_io = 1; return 0; }
    s_sec_lba = lba;
    return 1;
}

static int sec_save(void) { return disk_write_run(s_sec_lba, 1, s_sec); }

/* ---- the allocation table -------------------------------------------------- */
static int  cl_ok(u32 c) { return c >= 2 && c < s_clusters + 2; }
static u32  cl_lba(u32 c) { return s_data_lba + (c - 2) * s_spc; }

static void fat_set(u32 c, u32 v)
{
    u32 off = c + c / 2;
    if (c & 1) { s_fat[off] = (u8)((s_fat[off] & 0x0F) | (v << 4)); s_fat[off + 1] = (u8)(v >> 4); }
    else       { s_fat[off] = (u8)v; s_fat[off + 1] = (u8)((s_fat[off + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
    s_fat_dirty[off / 512] = 1;
    s_fat_dirty[(off + 1) / 512] = 1;
}

/* What changed, onto the floppy: the spare copy first, the one in use last. */
static int fat_flush(void)
{
    u32 copy, i;
    for (copy = s_fats; copy-- > 0; )
        for (i = 0; i < s_spf; i++)
            if (s_fat_dirty[i] && !disk_write_run(s_fat_lba + copy * s_spf + i, 1, s_fat + i * 512))
                return 0;
    for (i = 0; i < FAT_SECS; i++) s_fat_dirty[i] = 0;
    return 1;
}

/* The very last cluster is never given out.  It is a good cluster, but
 * some readers (mtools among them) take a chain that reaches it for a
 * damaged disk, and the floppy has to read anywhere. */
static u32 fat_free(void)
{
    u32 c, n = 0;
    for (c = 2; c <= s_clusters; c++) n += next_cluster(c) == 0;
    return n;
}

static u32 chain_len(u32 c)
{
    u32 n = 0;
    while (cl_ok(c) && n <= s_clusters) { n++; c = next_cluster(c); }
    return n;
}

static void chain_free(u32 c)
{
    u32 n = 0;
    while (cl_ok(c) && n++ <= s_clusters) {
        u32 next = next_cluster(c);
        fat_set(c, 0);
        c = next;
    }
}

/* n free clusters, linked into a chain: the lowest there are, so a file
 * on a fresh disk is in one piece.  The caller has counted that there are
 * enough. */
static u32 chain_alloc(u32 n)
{
    u32 c, first = 0, prev = 0;
    for (c = 2; c <= s_clusters && n; c++) {
        if (next_cluster(c)) continue;
        if (prev) fat_set(prev, c); else first = c;
        fat_set(c, 0xFFF);
        prev = c;
        n--;
    }
    return first;
}

/* Before the first change of a session: is there an inventory, and do the
 * two copies of the table agree?  If a write was once cut short between
 * them, the one in use wins: it matches the folders, which are written
 * after it. */
static int ready(void)
{
    u32 i, k;
    s_io = 0;
    s_sec_lba = ~0u;
    if (!fs_mount()) return no("The floppy will not read.");
    if (!s_inv) return no("This floppy has no inventory folder.");
    if (s_checked) return 1;
    if (s_fats > 1) {
        int have = disk_read_run(s_fat_lba + s_spf, s_spf, s_spare);
        for (i = 0; i < s_spf; i++) {
            for (k = 0; have && k < 512 && s_spare[i * 512 + k] == s_fat[i * 512 + k]; k++) { }
            if (have && k == 512) continue;
            if (!disk_write_run(s_fat_lba + s_spf + i, 1, s_fat + i * 512)) return no("The floppy would not take it.");
        }
    }
    for (i = 0; i < s_spf; i++)
        if (s_fat_mend[i]) {
            if (!disk_write_run(s_fat_lba + i, 1, s_fat + i * 512)) return no("The floppy would not take it.");
            s_fat_mend[i] = 0;
        }
    s_checked = 1;
    return 1;
}

/* ---- folders --------------------------------------------------------------- */
typedef struct { u32 c, s, i, ord, guard; int going; } DirIt;

/* The next entry of a folder, in s_sec; 0 at the end of its clusters, or
 * if the floppy fails (s_io). */
static u8 *dir_next(DirIt *it)
{
    if (it->going) {
        it->ord++;
        if (++it->i == 16) {
            it->i = 0;
            if (++it->s == s_spc) {
                it->s = 0;
                it->c = next_cluster(it->c);
                if (++it->guard > s_clusters) return 0;
            }
        }
    }
    it->going = 1;
    if (!cl_ok(it->c) || !sec_load(cl_lba(it->c) + it->s)) return 0;
    return s_sec + it->i * 32;
}

static void dir_begin(DirIt *it, u32 c)
{
    it->c = c; it->s = it->i = it->ord = it->guard = 0; it->going = 0;
}

/* Where an entry is, and the entry.  lfn is where the long name some
 * other computer gave it begins, just before it. */
typedef struct { u32 lba, idx, ord, lfn; u8 e[32]; } Slot;

static int dir_find(u32 dirc, const char want[11], Slot *out)
{
    DirIt it;
    u8 *e;
    u32 run = ~0u;
    int k;
    dir_begin(&it, dirc);
    while ((e = dir_next(&it))) {
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { run = ~0u; continue; }
        if ((e[11] & 0x3F) == 0x0F) { if (run == ~0u) run = it.ord; continue; }
        for (k = 0; k < 11 && e[k] == (u8)want[k]; k++) { }
        if (k == 11 && !(e[11] & 0x08)) {
            out->lba = s_sec_lba; out->idx = it.i; out->ord = it.ord;
            out->lfn = run != ~0u ? run : it.ord;
            memcpy8(out->e, e, 32);
            return 1;
        }
        run = ~0u;
    }
    return 0;
}

/* A free entry in a folder; the folder grows by a cluster if it is full. */
static int dir_room(u32 dirc, Slot *out)
{
    DirIt it;
    u8 *e;
    u32 last = dirc, c, k;
    dir_begin(&it, dirc);
    while ((e = dir_next(&it))) {
        last = it.c;
        if (e[0] == 0x00 || e[0] == 0xE5) { out->lba = s_sec_lba; out->idx = it.i; return 1; }
    }
    if (s_io) return bail("The floppy will not read.");
    if (!fat_free()) return no("There is no room left on the floppy.");
    c = chain_alloc(1);
    for (k = 0; k < 512; k++) s_sec[k] = 0;
    for (k = 0; k < s_spc; k++) {
        s_sec_lba = cl_lba(c) + k;
        if (!sec_save()) return bail("The floppy would not take it.");
    }
    fat_set(last, c);
    if (!fat_flush()) return bail("The floppy would not take it.");
    out->lba = cl_lba(c); out->idx = 0;
    return 1;
}

static int slot_put(const Slot *s)
{
    if (!sec_load(s->lba)) return 0;
    memcpy8(s_sec + s->idx * 32, s->e, 32);
    return sec_save();
}

/* Strike out entries from..to of a folder: the entry and any long name. */
static int dir_strike(u32 dirc, u32 from, u32 to)
{
    DirIt it;
    u8 *e;
    dir_begin(&it, dirc);
    while ((e = dir_next(&it))) {
        if (it.ord > to) break;
        if (it.ord < from) continue;
        e[0] = 0xE5;
        if (it.i == 15 || it.ord == to) { if (!sec_save()) return 0; }
    }
    return !s_io;
}

/* The date and time, the way FAT12 keeps them. */
static void stamp(u8 *e, int made)
{
    u8 h = 0, mi = 0, se = 0, mo = 1, da = 1;
    u16 yr = 1980;
    u32 d, t;
    if (!rtc_read(&h, &mi, &se, &yr, &mo, &da) || yr < 1980 || yr > 2107) { yr = 1980; mo = 1; da = 1; h = mi = se = 0; }
    d = ((u32)(yr - 1980) << 9) | ((u32)mo << 5) | da;
    t = ((u32)h << 11) | ((u32)mi << 5) | (se / 2);
    if (made) { put16(e + 14, t); put16(e + 16, d); }
    put16(e + 18, d);
    put16(e + 22, t); put16(e + 24, d);
}

/* ---- names and paths ---------------------------------------------------------- */
/* One name of a path, as the eleven letters FAT12 keeps: up to eight, a
 * dot, up to three.  Letters, digits and a few signs; no spaces, and
 * nothing that begins with a dot, so no path can climb out. */
static int name83(const char *s, int len, char out[11])
{
    int i = 0, n;
    for (n = 0; n < 11; n++) out[n] = ' ';
    for (n = 0; i < len && s[i] != '.'; i++, n++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (n >= 8) return 0;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~' || c == '!')) return 0;
        out[n] = c;
    }
    if (!n) return 0;
    if (i < len) {
        i++;                                     /* the dot                */
        if (i == len) return 0;
        for (n = 8; i < len; i++, n++) {
            char c = s[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (n >= 11) return 0;
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~' || c == '!')) return 0;
            out[n] = c;
        }
    }
    return 1;
}

/* Follow a path down from the inventory.  1: it is there (slot); 0: it is
 * not, but the folder it would be in is (parent, last); 2: the path is the
 * inventory itself; -1: no such folder on the way, or not a name. */
static u32 s_depth;                      /* how many names the last path had */
static int follow(const char *path, u32 *parent, char last[11], Slot *slot)
{
    u32 dir = s_inv, depth = 0;
    const char *p = path;
    for (;;) {
        int len = 0, more;
        while (*p == '/' || *p == '\\') p++;
        if (!*p) { *parent = dir; return depth ? -1 : 2; }
        while (p[len] && p[len] != '/' && p[len] != '\\') len++;
        if (!name83(p, len, last)) { no("That is not a name the floppy can keep: eight letters, a dot, three."); return -1; }
        p += len;
        while (*p == '/' || *p == '\\') p++;
        more = *p != 0;
        *parent = dir;
        if (++depth > 8) { no("That is too many folders deep."); return -1; }
        s_depth = depth;
        if (!dir_find(dir, last, slot)) {
            if (s_io) { bail("The floppy will not read."); return -1; }
            if (more) { no("There is no such folder."); return -1; }
            return 0;
        }
        if (!more) return 1;
        if (!(slot->e[11] & 0x10)) { no("That is a file, not a folder."); return -1; }
        dir = get16(slot->e + 26);
        if (!cl_ok(dir)) { no("That folder is damaged."); return -1; }
    }
}

static void item_of(const u8 *e, FsItem *it)
{
    int k, n = 0;
    for (k = 0; k < 8 && e[k] != ' '; k++) it->name[n++] = (char)e[k];
    if (e[8] != ' ') {
        it->name[n++] = '.';
        for (k = 8; k < 11 && e[k] != ' '; k++) it->name[n++] = (char)e[k];
    }
    it->name[n] = 0;
    it->folder = (e[11] & 0x10) != 0;
    it->size = it->folder ? 0 : get32(e + 28);
    it->time = get16(e + 22);
    it->date = get16(e + 24);
}

/* How many levels of folders there are inside this one: 0 if it holds
 * only files.  Paths go eight names deep and no further, so that nothing
 * can be put where it could not be reached again. */
static u32 levels_below(u32 c, u32 level)
{
    DirIt it;
    u8 *e;
    u32 most = 0;
    if (level > 8) return 99;
    dir_begin(&it, c);
    while ((e = dir_next(&it))) {
        u32 sub, n;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[0] == '.' || (e[11] & 0x18) != 0x10) continue;
        sub = get16(e + 26);
        n = cl_ok(sub) ? 1 + levels_below(sub, level + 1) : 1;
        if (n > most) most = n;
    }
    return most;
}

/* ---- what apps and the shell call ------------------------------------------- */
int inv_stat(const char *path, FsItem *it)
{
    Slot s;
    u32 parent;
    char last[11];
    s_io = 0; s_sec_lba = ~0u; s_why = "";
    if (!fs_mount() || !s_inv) return no("This floppy has no inventory folder.");
    if (follow(path, &parent, last, &s) != 1) return 0;
    item_of(s.e, it);
    return 1;
}

/* Everything in a folder ("" is the inventory itself), in the order the
 * floppy has it.  How many, or -1 if there is no such folder. */
int inv_list(const char *dir, void (*fn)(const FsItem *it, void *ctx), void *ctx)
{
    Slot s;
    DirIt it;
    FsItem item;
    u32 parent, c;
    char last[11];
    u8 *e;
    int r, n = 0;

    s_io = 0; s_sec_lba = ~0u; s_why = "";
    if (!fs_mount() || !s_inv) { no("This floppy has no inventory folder."); return -1; }
    r = follow(dir, &parent, last, &s);
    if (r == 2) c = s_inv;
    else if (r == 1 && (s.e[11] & 0x10)) c = get16(s.e + 26);
    else { if (r >= 0) no("There is no such folder."); return -1; }
    dir_begin(&it, c);
    while ((e = dir_next(&it))) {
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[0] == '.' || (e[11] & 0x08)) continue;
        item_of(e, &item);
        fn(&item, ctx);
        n++;
    }
    return n;
}

/* A file's bytes, up to max.  How many, or -1. */
int inv_read(const char *path, u8 *buf, u32 max)
{
    Slot s;
    FsFile f;
    u32 parent;
    char last[11];
    s_io = 0; s_sec_lba = ~0u; s_why = "";
    if (!fs_mount() || !s_inv) { no("This floppy has no inventory folder."); return -1; }
    if (follow(path, &parent, last, &s) != 1) { if (!*s_why) no("There is nothing by that name."); return -1; }
    if (s.e[11] & 0x10) { no("That is a folder."); return -1; }
    f.first = get16(s.e + 26);
    f.size = get32(s.e + 28);
    if (fs_read(&f, buf, max) < 0) { no("The floppy will not read it."); return -1; }
    return (int)(f.size < max ? f.size : max);
}

u32 inv_room(void)
{
    if (!fs_mount() || !s_inv) return 0;
    return fat_free() * s_spc * 512;
}

/* Make the file, or replace it, with these n bytes. */
int inv_write(const char *path, const u8 *buf, u32 n)
{
    Slot s;
    FsFile f;
    u32 parent, per, need, room, oldc = 0, oldn = 0;
    char last[11];
    int r, k;

    if (!ready()) return 0;
    s_why = "";
    r = follow(path, &parent, last, &s);
    if (r < 0) return 0;
    if (r == 2) return no("That is the inventory itself.");
    if (r == 1 && (s.e[11] & 0x10)) return no("There is a folder by that name.");
    if (r == 1 && (s.e[11] & 0x01)) return no("That file is marked not to be changed.");
    per = s_spc * 512;
    need = (n + per - 1) / per;
    room = fat_free();
    if (r == 1) { oldc = get16(s.e + 26); oldn = chain_len(oldc); }
    if (need > room) {
        if (need > room + oldn) return no("There is not room on the floppy for that.");
        chain_free(oldc);                        /* so tight that the old copy has to give way first */
        oldc = 0;
    }
    f.first = chain_alloc(need);
    f.size = n;
    if (n && !walk(&f, (u8 *)buf, n, 1)) return bail("The floppy would not take it.");
    if (!fat_flush()) return bail("The floppy would not take it.");
    if (r == 0) {
        if (!dir_room(parent, &s)) {
            const char *why = s_why;
            chain_free(f.first);
            if (s_mounted) fat_flush();
            return no(why);
        }
        for (k = 0; k < 32; k++) s.e[k] = 0;
        memcpy8(s.e, last, 11);
        s.e[11] = 0x20;
        stamp(s.e, 1);
    } else stamp(s.e, 0);
    put16(s.e + 26, f.first);
    put16(s.e + 20, 0);
    put32(s.e + 28, n);
    if (!slot_put(&s)) return bail("The floppy would not take it.");
    if (oldc) {
        chain_free(oldc);
        if (!fat_flush()) return bail("The floppy would not take it.");
    }
    return 1;
}

int inv_mkdir(const char *path)
{
    Slot s;
    u32 parent, c, k;
    char last[11];
    int r;

    if (!ready()) return 0;
    s_why = "";
    r = follow(path, &parent, last, &s);
    if (r < 0) return 0;
    if (r) return no("There is already something by that name.");
    if (last[8] != ' ') return no("A folder's name has no dot in it.");
    if (s_depth > 7) return no("That is too many folders deep.");
    if (!fat_free()) return no("There is no room left on the floppy.");
    c = chain_alloc(1);
    for (k = 0; k < 512; k++) s_sec[k] = 0;
    for (k = s_spc; k-- > 0; ) {                 /* the first sector last: it holds . and .. */
        s_sec_lba = cl_lba(c) + k;
        if (k == 0) {
            for (r = 0; r < 11; r++) s_sec[r] = s_sec[32 + r] = ' ';
            s_sec[0] = s_sec[32] = s_sec[33] = '.';
            s_sec[11] = s_sec[32 + 11] = 0x10;
            put16(s_sec + 26, c);
            put16(s_sec + 32 + 26, parent);
            stamp(s_sec, 1);
            stamp(s_sec + 32, 1);
        }
        if (!sec_save()) return bail("The floppy would not take it.");
    }
    if (!fat_flush()) return bail("The floppy would not take it.");
    if (!dir_room(parent, &s)) {
        const char *why = s_why;
        chain_free(c);
        if (s_mounted) fat_flush();
        return no(why);
    }
    for (k = 0; k < 32; k++) s.e[k] = 0;
    memcpy8(s.e, last, 11);
    s.e[11] = 0x10;
    stamp(s.e, 1);
    put16(s.e + 26, c);
    if (!slot_put(&s)) return bail("The floppy would not take it.");
    return 1;
}

/* A file, or a folder with nothing in it. */
int inv_remove(const char *path)
{
    Slot s;
    DirIt it;
    u32 parent, c;
    char last[11];
    u8 *e;
    int r;

    if (!ready()) return 0;
    s_why = "";
    r = follow(path, &parent, last, &s);
    if (r < 0) return 0;
    if (r == 2) return no("That is the inventory itself.");
    if (r == 0) return no("There is nothing by that name.");
    if (s.e[11] & 0x01) return no("That is marked not to be changed.");
    c = get16(s.e + 26);
    if (s.e[11] & 0x10) {
        dir_begin(&it, c);
        while ((e = dir_next(&it))) {
            if (e[0] == 0x00) break;
            if (e[0] != 0xE5 && e[0] != '.') return no("That folder still has things in it.");
        }
        if (s_io) return bail("The floppy will not read.");
    }
    if (!dir_strike(parent, s.lfn, s.ord)) return bail("The floppy would not take it.");
    chain_free(c);
    if (!fat_flush()) return bail("The floppy would not take it.");
    return 1;
}

/* Rename, or move to another folder, or both: `to` is the whole new path. */
int inv_move(const char *from, const char *to)
{
    Slot a, b;
    u32 pa, pb, c, guard = 0;
    char la[11], lb[11];
    int r;

    if (!ready()) return 0;
    s_why = "";
    r = follow(from, &pa, la, &a);
    if (r < 0) return 0;
    if (r == 2) return no("That is the inventory itself.");
    if (r == 0) return no("There is nothing by that name.");
    r = follow(to, &pb, lb, &b);
    if (r < 0) return 0;
    if (r) return no("There is already something by that name there.");
    if ((a.e[11] & 0x10) && lb[8] != ' ') return no("A folder's name has no dot in it.");
    if (a.e[11] & 0x10) {
        u32 depth = s_depth;
        if (depth + levels_below(get16(a.e + 26), 0) > 7) return no("That would put things too many folders deep.");
        if (s_io) return bail("The floppy will not read.");
    }

    if (pa == pb) {                              /* a new name, where it is */
        if (a.lfn < a.ord && !dir_strike(pa, a.lfn, a.ord - 1)) return bail("The floppy would not take it.");
        memcpy8(a.e, lb, 11);
        if (!slot_put(&a)) return bail("The floppy would not take it.");
        return 1;
    }
    if (a.e[11] & 0x10) {
        /* Not into itself, or into anything inside itself: climb from where
         * it is going and see that the way up does not pass through it. */
        u32 moved = get16(a.e + 26);
        for (c = pb; c != s_inv; ) {
            if (c == moved) return no("A folder cannot go inside itself.");
            if (!cl_ok(c) || ++guard > 16 || !sec_load(cl_lba(c))) return bail("That folder is damaged.");
            c = get16(s_sec + 32 + 26);
        }
    }
    /* Its entry in the new folder first, then out of the old: cut between
     * the two, it is in both, and nothing is lost. */
    if (!dir_room(pb, &b)) return 0;
    memcpy8(b.e, a.e, 32);
    memcpy8(b.e, lb, 11);
    if (!slot_put(&b)) return bail("The floppy would not take it.");
    if (!dir_strike(pa, a.lfn, a.ord)) return bail("The floppy would not take it.");
    if (a.e[11] & 0x10) {
        if (!sec_load(cl_lba(get16(a.e + 26)))) return bail("The floppy will not read.");
        put16(s_sec + 32 + 26, pb);
        if (!sec_save()) return bail("The floppy would not take it.");
    }
    return 1;
}

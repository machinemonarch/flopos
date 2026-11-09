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
 * Reading only, with one exception: a file's own clusters can be
 * rewritten in place, as long as nothing about its size or where it lives
 * changes.  The allocation table and the directory are never written --
 * that is where a floppy filesystem gets damaged, and the only copy of the
 * kernel is on this disk.  The editor's documents are files made at their
 * full size when the disk is built, and saved inside that size.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

static int s_mounted;
static u32 s_spc, s_fat_lba, s_root_lba, s_root_n, s_data_lba, s_clusters;
static u8 *s_fat;                        /* the whole first FAT            */
static u8 *s_root;                       /* the whole root directory       */
static u8 s_boot[512];

static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/* The layout comes from the floppy's own boot sector, not from what a
 * 1.44MB disk is supposed to look like. */
int fs_mount(void)
{
    u32 reserved, fats, spf, total;

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

    if (!s_fat) {
        s_fat = (u8 *)halloc(spf * 512);
        s_root = (u8 *)halloc(((s_root_n * 32 + 511) / 512) * 512);
    }
    if (!disk_read_run(s_fat_lba, spf, s_fat)) return 0;
    if (!disk_read_run(s_root_lba, (s_root_n * 32 + 511) / 512, s_root)) return 0;
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

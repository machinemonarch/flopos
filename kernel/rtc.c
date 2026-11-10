/* ===========================================================================
 * rtc.c -- the CMOS real-time clock.
 *
 * Reading it is a two-step dance: wait for the chip to finish an update,
 * take a reading, then take another and only believe it if the two agree.
 * Without that you can catch the registers mid-carry and read 10:59:60.
 *
 * Whether the values come back as BCD or binary, and whether the hour is
 * 12 or 24 hour, is in status register B -- which is honoured here rather
 * than assumed, because 1996 laptops vary.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

typedef struct {
    u8 sec, min, hour, day, month;
    u16 year;
} Rtc;

static u8 cmos_read(u8 reg)
{
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    return cmos_read(0x0A) & 0x80;
}

static void raw_read(Rtc *o)
{
    o->sec   = cmos_read(0x00);
    o->min   = cmos_read(0x02);
    o->hour  = cmos_read(0x04);
    o->day   = cmos_read(0x07);
    o->month = cmos_read(0x08);
    o->year  = cmos_read(0x09);
}

static u8 from_bcd(u8 v) { return (u8)((v & 0x0F) + ((v >> 4) * 10)); }

int rtc_read(u8 *hour, u8 *min, u8 *sec, u16 *year, u8 *month, u8 *day)
{
    Rtc a, b;
    u8 statb;
    int guard = 0;

    while (update_in_progress() && ++guard < 1000000) { }

    raw_read(&a);
    do {
        b = a;
        while (update_in_progress()) { }
        raw_read(&a);
        if (++guard > 1000000) return 0;
    } while (a.sec != b.sec || a.min != b.min || a.hour != b.hour ||
             a.day != b.day || a.month != b.month || a.year != b.year);

    statb = cmos_read(0x0B);
    {
        int pm = (a.hour & 0x80) != 0;
        a.hour &= 0x7F;
        if (!(statb & 0x04)) {           /* values are BCD                 */
            a.sec = from_bcd(a.sec);
            a.min = from_bcd(a.min);
            a.hour = from_bcd(a.hour);
            a.day = from_bcd(a.day);
            a.month = from_bcd(a.month);
            a.year = from_bcd((u8)a.year);
        }
        if (!(statb & 0x02))             /* 12-hour: 12am is 0, 12pm is 12 */
            a.hour = (u8)((a.hour % 12) + (pm ? 12 : 0));
    }

    /* The century register is not dependable on hardware this old, so
     * pivot on a two-digit year the way everything else of the era did. */
    *year  = (u16)(a.year < 80 ? 2000 + a.year : 1900 + a.year);
    *month = a.month;
    *day   = a.day;
    *hour  = a.hour;
    *min   = a.min;
    *sec   = a.sec;

    if (*month < 1 || *month > 12 || *day < 1 || *day > 31) return 0;
    return 1;
}

/* ---- setting it -------------------------------------------------------------
 * The laptop's clock battery may be long dead, and then the clock wakes up
 * in some year of its own choosing -- and no web site's certificate can be
 * checked against a date in 1996.  So the clock is set from two places:
 *
 *   the network, when there is one: one question to a time server, the
 *   one DHCP names or else pool.ntp.org (ip.c), kept in Swedish time,
 *   since that is where this laptop lives;
 *
 *   and, with no network, the time this machine last said goodbye, kept
 *   on the floppy by `bye`: not right, but near, and near is all a
 *   certificate's dates need.
 *
 * The network is never believed when it would put the clock back before
 * that goodbye: making an expired certificate look current is the one
 * harm a lying time server could do.
 */
static void cmos_write(u8 reg, u8 v)
{
    outb(CMOS_ADDR, reg);
    io_wait();
    outb(CMOS_DATA, v);
}

static u8 to_bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }

/* Days since 2000-01-01 for a date, and back again. */
static u32 days_of(u32 y, u32 m, u32 d)
{
    u32 era, yoe, doy;
    y -= m <= 2;
    era = y / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 730425;
}

static void date_from(u32 days, u32 *y, u32 *m, u32 *d)
{
    u32 z = days + 730425, era = z / 146097, doe = z - era * 146097;
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

/* The clock, as seconds since 2000; 0 if it cannot be read. */
u32 rtc_seconds(void)
{
    u8 h, mi, se, mo, da;
    u16 yr;
    if (!rtc_read(&h, &mi, &se, &yr, &mo, &da) || yr < 2000) return 0;
    return days_of(yr, mo, da) * 86400u + (u32)h * 3600u + (u32)mi * 60u + se;
}

/* Set it, in whatever form it keeps its numbers. */
void rtc_set_seconds(u32 s)
{
    u32 t = s % 86400u, y, m, d, h = t / 3600u, mi = (t / 60u) % 60u, se = t % 60u, hh = h;
    u8 statb = cmos_read(0x0B);
    int bcd = !(statb & 0x04), h24 = (statb & 0x02) != 0, guard = 0;
    u8 hv;

    date_from(s / 86400u, &y, &m, &d);
    if (!h24) { hh = h % 12; if (!hh) hh = 12; }
    hv = bcd ? to_bcd(hh) : (u8)hh;
    if (!h24 && h >= 12) hv |= 0x80;
    while (update_in_progress() && ++guard < 1000000) { }
    cmos_write(0x0B, (u8)(statb | 0x80));        /* hold it still meanwhile */
    cmos_write(0x00, bcd ? to_bcd(se) : (u8)se);
    cmos_write(0x02, bcd ? to_bcd(mi) : (u8)mi);
    cmos_write(0x04, hv);
    cmos_write(0x07, bcd ? to_bcd(d) : (u8)d);
    cmos_write(0x08, bcd ? to_bcd(m) : (u8)m);
    cmos_write(0x09, bcd ? to_bcd(y % 100) : (u8)(y % 100));
    cmos_write(0x0B, (u8)(statb & 0x7F));
}

/* Sweden's offset from UTC at a moment in UTC: an hour, and two in summer,
 * from 01:00 UTC on the last Sunday of March to the same on the last
 * Sunday of October, as all of the EU does it.  2000-01-01 was a Saturday. */
static u32 sweden(u32 utc)
{
    u32 y, m, d, mar, oct;
    date_from(utc / 86400u, &y, &m, &d);
    mar = days_of(y, 3, 31);  mar -= (mar + 6) % 7;
    oct = days_of(y, 10, 31); oct -= (oct + 6) % 7;
    return utc >= mar * 86400u + 3600u && utc < oct * 86400u + 3600u ? 7200u : 3600u;
}

/* ---- is the clock's battery any good? ---------------------------------------
 * The clock chip says so itself: register D's top bit stays set while its
 * battery holds up.  And the BIOS, at power-on, notes in byte 0x0E whether
 * the clock had lost its power (bit 7) or its time was nonsense (bit 2) --
 * which is what a laptop left unplugged for long enough shows, battery new
 * or not, since on many laptops of the time it charges only while the
 * machine is on. */
int rtc_battery_ok(void) { return (cmos_read(0x0D) & 0x80) != 0; }
int rtc_lost_power(void) { return (cmos_read(0x0E) & 0x84) != 0; }

/* How far out the clock was when the network last set it, in seconds
 * (positive: it was behind); and whether that has happened. */
static s32 s_drift;
static int s_drift_known;
int clock_drift(s32 *secs) { *secs = s_drift; return s_drift_known; }

int clock_from_network(void)
{
    u32 utc = ip_ntp_time(), local, was = store_clock_get(), before;
    if (!utc) return 0;
    local = utc + sweden(utc);
    if (local < 24u * 365u * 86400u) return 0;            /* before 2024: no */
    if (was && local + 86400u < was) {
        serial_printf("[clock] the time server says earlier than the last goodbye; not believed\n");
        return 0;
    }
    before = rtc_seconds();
    rtc_set_seconds(local);
    if (before) {
        u32 a;
        s_drift = (s32)(local - before);
        s_drift_known = 1;
        a = s_drift < 0 ? (u32)-s_drift : (u32)s_drift;
        serial_printf("[clock] set from the network: it was %u days %u h %u min %s\n",
                      a / 86400, (a / 3600) % 24, (a / 60) % 60, s_drift < 0 ? "ahead" : "behind");
        store_journal_printf("the clock was %u days %u h %u min %s; the network set it",
                             a / 86400, (a / 3600) % 24, (a / 60) % 60, s_drift < 0 ? "ahead" : "behind");
    } else
        serial_printf("[clock] set from the network\n");
    return 1;
}

int clock_from_goodbye(void)
{
    u32 was = store_clock_get(), now = rtc_seconds();
    if (!was || (now && now >= was)) return 0;
    rtc_set_seconds(was);
    return 1;
}

void clock_remember(void)
{
    u32 now = rtc_seconds();
    if (now) store_clock_set(now);
}

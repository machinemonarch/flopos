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

    if (!(statb & 0x04)) {               /* values are BCD                 */
        int pm = (a.hour & 0x80) != 0;
        a.hour &= 0x7F;
        a.sec = from_bcd(a.sec);
        a.min = from_bcd(a.min);
        a.hour = from_bcd(a.hour);
        a.day = from_bcd(a.day);
        a.month = from_bcd(a.month);
        a.year = from_bcd((u8)a.year);
        if (pm) a.hour = (u8)((a.hour % 12) + 12);
    } else if (a.hour & 0x80) {          /* binary, 12-hour, afternoon     */
        a.hour = (u8)(((a.hour & 0x7F) % 12) + 12);
    }

    if (!(statb & 0x02) && a.hour == 12) a.hour = 0;   /* 12am in 12h mode */

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

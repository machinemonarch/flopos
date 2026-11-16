/* tracker.h -- playing .mod, .xm, .s3m and .it songs (tracker.c). */
#ifndef TRACKER_H
#define TRACKER_H
/* data holds the file in its first len bytes, and has room bytes in all:
 * IT's packed samples are unpacked into what is spare.  The data is
 * changed, and kept, so a file is loaded only once. */
int  tracker_load(u8 *data, u32 len, u32 room, int rate);
void tracker_rate(int rate);                     /* another rate, after loading */
int  tracker_render(u8 *out, int n);             /* < n: the song has ended       */
const char *tracker_title(void);
const char *tracker_kind(void);
int  tracker_channels(void);
int  tracker_position(int *ord, int *nord);      /* -> the row                    */
u32  tracker_samples(void);                     /* made so far                   */
#endif

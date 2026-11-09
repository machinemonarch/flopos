/* tracker.h -- playing .mod and .xm songs (tracker.c). */
#ifndef TRACKER_H
#define TRACKER_H
int  tracker_load(u8 *data, u32 len, int rate);  /* the data is changed, and kept */
int  tracker_render(u8 *out, int n);             /* < n: the song has ended       */
const char *tracker_title(void);
const char *tracker_kind(void);
int  tracker_channels(void);
int  tracker_position(int *ord, int *nord);      /* -> the row                    */
u32  tracker_samples(void);                     /* made so far                   */
#endif

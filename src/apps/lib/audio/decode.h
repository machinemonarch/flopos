/* ===========================================================================
 * decode.h -- recordings into samples, one way for every format.
 *
 * A decoder reads bytes from an AudioIn -- a whole file in memory, or as
 * much of a stream as has arrived -- and gives back mono 16-bit samples at
 * the recording's own rate.  A Resampler then brings them to the sound
 * card's rate as the 8-bit samples audio_write wants.
 *
 * Streams: more bytes may be added at the end of an AudioIn between calls.
 * A decoder that runs out in the middle of something returns AUDIO_MORE
 * without having used any of it, and is called again once more has come.
 * ===========================================================================
 */
#ifndef DECODE_H
#define DECODE_H
#include "app.h"

typedef short s16;

typedef struct {
    u8  *buf;                            /* the bytes                        */
    u32  len;                            /* how many there are so far       */
    u32  pos;                            /* how far the decoder has read    */
    int  done;                           /* no more are coming              */
} AudioIn;

#define AUDIO_END   0                    /* nothing more, ever              */
#define AUDIO_MORE  (-1)                 /* not enough bytes yet            */
#define AUDIO_BAD   (-2)                 /* cannot be read                  */

/* Loud recordings are mastered right up to full scale, and decoding one
 * overshoots it: without a little room the peaks are cut off and crackle.
 * About 3 dB, applied by every decoder that makes its own samples. */
#define AUDIO_HEADROOM 0.7f

typedef struct Decoder {
    const char *kind;                    /* "WAV", "MP3", ...               */
    int   rate;                          /* of the recording                */
    int   channels;
    int   kbps;                          /* 0 if it does not say            */
    /* Up to max mono samples into out: how many, or AUDIO_END, AUDIO_MORE,
     * AUDIO_BAD. */
    int  (*decode)(struct Decoder *d, AudioIn *in, s16 *out, int max);
} Decoder;

/* What it is, from the first bytes (a few KB is plenty).  1 and d filled
 * in if it is something that can be played; 0 if not; AUDIO_MORE if it
 * cannot tell yet. */
int  audio_detect(AudioIn *in, Decoder *d, const char *ctype);

/* Each format. */
int  wav_open(AudioIn *in, Decoder *d);
int  mp3_open(AudioIn *in, Decoder *d);
int  vorbis_open(AudioIn *in, Decoder *d);
int  flac_open(AudioIn *in, Decoder *d);
int  aac_open(AudioIn *in, Decoder *d);
int  mp4_open(AudioIn *in, Decoder *d);  /* AAC in an MP4 file (.m4a) */

/* How many bytes audio_detect may want before it can tell: a few KB for
 * most, but an MP4 file's index comes first and can be hundreds of KB. */
u32  audio_detect_limit(const AudioIn *in);

/* MP4 (mp4.c).  mp4_room lends memory for a long file's index (two bytes
 * a sample); without it a few minutes' worth fit.  The boxes and the
 * sound track's description, for others that read MP4 (browzer's HLS). */
typedef struct { int mp3, prof, sfi, ch; u32 track; } Mp4Sound;
void      mp4_room(u8 *room, u32 cap);
const u8 *mp4_box(const u8 *p, u32 n, u32 *at, const char *t, u32 *len, const u8 **type);
const u8 *mp4_in(const u8 *p, u32 n, const char *t, u32 *len);
int       mp4_sound(const u8 *trak, u32 n, Mp4Sound *s);   /* 1 if it is sound that can be played */

/* Opus (opus.c), for vorbis.c's Ogg: set up (half: at 24 kHz), then a
 * packet at a time into out (room for 120 ms); opus_playable says if a
 * packet is one it reads (CELT), not speech (SILK, hybrid: silence). */
#ifndef OPUS_HALF
#define OPUS_HALF 1                      /* Opus at 24 kHz; 0 (48 kHz) only to check it against libopus */
#endif
void opus_start(int half);
int  opus_packet(const u8 *p, int n, s16 *out);
int  opus_playable(const u8 *p, int n);

/* AAC's bare blocks (aac.c), for mp4.c: set up from the rate index and
 * channels; one block in; its samples out. */
int  aac_raw_open(int sri, int ch, Decoder *d);
int  aac_raw(const u8 *p, u32 len);
int  aac_take(s16 *out, int max);

/* Ogg carries FLAC too: vorbis.c reads the Ogg pages and hands flac.c the
 * STREAMINFO block's 34 bytes (the rate, or 0 if it cannot be played), then
 * one frame at a time (how many samples it gave, or 0). */
int  flac_info(const u8 *streaminfo, int *channels);
int  flac_one(const u8 *frame, u32 len, s16 *out);

/* From the recording's rate to the card's, as 8-bit unsigned.  From twice
 * the card's rate or more, neighbouring samples are averaged first, so
 * what is too high to play does not fold back down as noise. */
typedef struct { u32 step, phase; s16 prev; int taps, h; s32 sum, recip, err; s16 hist[8]; } Resampler;
void rs_init(Resampler *r, int from, int to);
int  rs_run(Resampler *r, const s16 *in, int n, u8 *out);   /* out: n * to / from + 2 room */

#endif

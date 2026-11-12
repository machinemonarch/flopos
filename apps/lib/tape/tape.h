/* ===========================================================================
 * tape.h -- files as sound, for an audio cassette.
 *
 * The way home computers of the 1980s did it: two tones.  A 0 is one cycle
 * of 1200 Hz and a 1 is two cycles of 2400 Hz, so every bit lasts the same
 * time, 1200 of them a second.  A byte is a 0 to start it, its eight bits
 * (lowest first) and a 1 to end it: 120 bytes a second.
 *
 * On the tape, a file is a run of blocks.  Each block has a short stretch
 * of plain 2400 Hz in front (the leader, which is all 1s; a long one opens
 * the file), then:
 *
 *      A5 5A   what follows is a block
 *      type    1 the file's name and size, 2 a piece of it, 3 the end
 *      seq     which piece, two bytes, lowest first
 *      len     how many bytes follow, two bytes (at most TAPE_PIECE)
 *      ...     the bytes
 *      crc     CRC-16/CCITT of type..bytes, two bytes
 *
 * A block that fails its check is dropped whole, and the others still
 * count: the tape can be played again to fill in what was missed.
 *
 * Reading goes by the time between the wave's crossings of its middle,
 * which is all that survives a cassette intact: loudness, a drifting
 * middle, the wave turned upside down and a tape running a few percent
 * fast or slow all leave it standing.  Whole numbers only.
 * ===========================================================================
 */
#ifndef TAPE_H
#define TAPE_H

#ifndef APP_H
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
#endif

#define TAPE_RATE    22050               /* samples a second, both ways    */
#define TAPE_PIECE   256                 /* bytes of the file in a block   */
#define TAPE_NAME    13                  /* "WRITE1.MD" and its zero       */

enum { TAPE_HEAD = 1, TAPE_DATA = 2, TAPE_END = 3 };

/* ---- writing ------------------------------------------------------------- */
typedef struct {
    u8  blk[TAPE_PIECE + 16];            /* the block being sent           */
    int len, pos;                        /* its bytes, and the one in hand */
    int bit;                             /* 0 start, 1..8 data, 9 stop     */
    int lead;                            /* leader bits still to send      */
    int half, halves;                    /* half-waves of this bit sent, of how many */
    u32 phase, step;                     /* where in the half-wave, 16.16  */
    int level;                           /* +1 or -1: which half           */
    int busy;
} TapeTx;

void tape_tx_begin(TapeTx *t);
/* Queue one block behind lead_ms of leader.  Only when tape_tx_busy is 0. */
void tape_tx_block(TapeTx *t, int type, int seq, const u8 *data, int len, int lead_ms);
int  tape_tx_busy(const TapeTx *t);
/* Up to max samples of it (8-bit, 128 in the middle); returns how many. */
int  tape_tx_samples(TapeTx *t, u8 *out, int max);

/* ---- reading ------------------------------------------------------------- */
typedef struct {
    void (*block)(void *ctx, int type, int seq, const u8 *data, int len);
    void *ctx;
    int  mid;                            /* the wave's middle, x256        */
    int  last;                           /* the last sample, about its middle */
    int  sign;                           /* which side of the middle       */
    u32  since;                          /* 1/16 samples since the last crossing */
    u32  shrt;                           /* a short half-wave's length, 1/16 samples */
    int  shorts;                         /* short half-waves in a row      */
    int  pend;                           /* a long half-wave waiting for its pair */
    int  state, nbit;                    /* in a byte: which bit           */
    u8   byte;
    u8   buf[TAPE_PIECE + 16];
    int  n, want;                        /* bytes of a block so far        */
    int  sync;                           /* 0 hunting, 1 saw A5, 2 in a block */
    u32  good, bad;                      /* blocks that passed, and failed  */
    int  loud;                           /* how strong the signal is, 0..127 */
    int  tone;                           /* hearing leader or data now      */
} TapeRx;

void tape_rx_begin(TapeRx *r, void (*block)(void *, int, int, const u8 *, int), void *ctx);
void tape_rx_samples(TapeRx *r, const u8 *in, int n);

u16 tape_crc(const u8 *p, int n);
#endif

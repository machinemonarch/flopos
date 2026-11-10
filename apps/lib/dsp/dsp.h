/* ===========================================================================
 * dsp.h -- arithmetic that signals and pictures share.
 *
 *   dsp_round      a float to the nearest int, without what a plain cast
 *                  costs on a Pentium
 *   dsp_dct2       the cosine transform (DCT-II) of 2 to 64 numbers, by
 *                  Lee's fast algorithm: an mp3's synthesis today, a JPEG's
 *                  8x8 blocks one day
 *   mdct.h         the inverse MDCT, by FFT: AAC, Vorbis
 * ===========================================================================
 */
#ifndef DSP_H
#define DSP_H
#include "app.h"

/* A cast from float to int makes the x87 switch its rounding mode to
 * truncate and back -- two control-word loads that stall a Pentium's
 * floating-point pipe for tens of cycles, at every sample.  Adding
 * 1.5 x 2^23 instead puts the rounded integer in the low bits of the
 * float's mantissa, where it can simply be read.  Good for |v| < 2^22;
 * rounds to nearest, which is better than truncating anyway. */
static inline int dsp_round(float v)
{
    union { float f; s32 i; } u;
    u.f = v + 12582912.0f;
    return u.i - 0x4B400000;
}

/* X[i] = sum over k < n of x[k] cos(pi i (2k + 1) / (2n)), in place;
 * n a power of two, 2 to 64. */
void dsp_dct2(float *x, int n);

#endif

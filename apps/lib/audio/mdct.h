/* mdct.h -- the inverse MDCT (mdct.c). */
#ifndef MDCT_H
#define MDCT_H
void imdct(const float *X, float *y, int n);   /* N/2 in, N out, N a power of two up to 8192 */
#endif

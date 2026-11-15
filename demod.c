/* demod.c - FM discriminator for SSTV
 *
 * The audio is mixed down to complex baseband around 1750 Hz (middle of the
 * 1200..2300 Hz SSTV band), low-pass filtered (Blackman windowed sinc, 1100 Hz)
 * and the phase difference between consecutive samples gives the frequency.
 */
#include <math.h>
#include <stdlib.h>
#include "sstv.h"
#include "demod.h"

#define FM_CUTOFF 1100.0

struct fmdemod {
    double fs;
    int    ntap;
    float *h;
    float *bre, *bim;     /* doubled delay lines (no modulo in the FIR loop) */
    int    pos;
    double ph, dph;
    double pre, pim;      /* previous filter output */
};

fmdemod *fmdemod_new(double fs)
{
    fmdemod *d = calloc(1, sizeof *d);
    int N = (int)(fs * 0.005);
    if (N < 15) N = 15;
    N |= 1;
    d->fs = fs;
    d->ntap = N;
    d->h = malloc((size_t)N * sizeof(float));
    d->bre = calloc((size_t)2 * N, sizeof(float));
    d->bim = calloc((size_t)2 * N, sizeof(float));

    double fc = FM_CUTOFF / fs, sum = 0;
    int M = N - 1;
    for (int i = 0; i < N; i++) {
        double k = i - M / 2.0;
        double s = (k == 0) ? 2 * fc : sin(2 * M_PI * fc * k) / (M_PI * k);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / M) + 0.08 * cos(4 * M_PI * i / M);
        d->h[i] = (float)(s * w);
        sum += s * w;
    }
    for (int i = 0; i < N; i++) d->h[i] = (float)(d->h[i] / sum);

    d->dph = 2 * M_PI * FM_CENTER / fs;
    d->pre = 1;
    d->pim = 0;
    return d;
}

void fmdemod_free(fmdemod *d)
{
    if (!d) return;
    free(d->h);
    free(d->bre);
    free(d->bim);
    free(d);
}

int fmdemod_delay(const fmdemod *d) { return (d->ntap - 1) / 2; }

float fmdemod_process(fmdemod *d, float x)
{
    double c = cos(d->ph), s = sin(d->ph);
    d->ph += d->dph;
    if (d->ph > 2 * M_PI) d->ph -= 2 * M_PI;

    int N = d->ntap;
    d->pos = (d->pos == 0 ? N : d->pos) - 1;
    d->bre[d->pos] = d->bre[d->pos + N] = (float)(x * c);
    d->bim[d->pos] = d->bim[d->pos + N] = (float)(-x * s);

    const float *br = d->bre + d->pos, *bi = d->bim + d->pos;
    double yr = 0, yi = 0;
    for (int i = 0; i < N; i++) {
        yr += d->h[i] * br[i];
        yi += d->h[i] * bi[i];
    }

    /* y[n] * conj(y[n-1]) */
    double dr = yr * d->pre + yi * d->pim;
    double di = yi * d->pre - yr * d->pim;
    d->pre = yr;
    d->pim = yi;
    return (float)(FM_CENTER + atan2(di, dr) * d->fs / (2 * M_PI));
}

float *fm_demod_buffer(const float *x, long n, double fs)
{
    fmdemod *d = fmdemod_new(fs);
    int D = fmdemod_delay(d);
    float *f = malloc((size_t)(n ? n : 1) * sizeof(float));
    for (long i = 0; i < n + D; i++) {
        float y = fmdemod_process(d, i < n ? x[i] : 0.0f);
        long j = i - D;
        if (j >= 0 && j < n) f[j] = y;
    }
    fmdemod_free(d);
    return f;
}

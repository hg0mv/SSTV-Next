/* tones.c - sliding DFT tone powers (see tones.h) */
#include <math.h>
#include <stdlib.h>
#include "tones.h"

struct tonespec {
    double fs;
    int    nw, delay;
    float *x;              /* last nw samples (ring) */
    int    pos;
    long   n, row;         /* samples taken, rows emitted */
    double ar[TS_NB], ai[TS_NB];     /* window sums of x * e^{-jwn} */
    double pr[TS_NB], pi[TS_NB];     /* e^{-jwn} for the current n */
    double sr[TS_NB], si[TS_NB];     /* one-sample step */
    double cr[TS_NB], ci[TS_NB];     /* e^{+jw nw}: phasor of the leaving sample relative to n */
};

tonespec *ts_new(double fs, int delay)
{
    tonespec *t = calloc(1, sizeof *t);
    t->fs = fs;
    t->nw = (int)lround(fs * TS_WIN);
    if (t->nw < 8) t->nw = 8;
    t->delay = delay;
    t->x = calloc((size_t)t->nw, sizeof(float));
    for (int k = 0; k < TS_NB; k++) {
        double w = 2 * M_PI * (TS_F0 + k * TS_DF) / fs;
        t->pr[k] = 1; t->pi[k] = 0;
        t->sr[k] = cos(w); t->si[k] = -sin(w);
        t->cr[k] = cos(w * t->nw); t->ci[k] = sin(w * t->nw);
    }
    return t;
}

void ts_free(tonespec *t)
{
    if (!t) return;
    free(t->x);
    free(t);
}

long ts_process(tonespec *t, float x, float *row)
{
    float old = t->x[t->pos];
    t->x[t->pos] = x;
    t->pos = (t->pos + 1) % t->nw;
    for (int k = 0; k < TS_NB; k++) {
        double pr = t->pr[k], pi = t->pi[k];
        /* leaving sample's phasor = p(n) * e^{+jw nw} */
        double qr = pr * t->cr[k] - pi * t->ci[k], qi = pr * t->ci[k] + pi * t->cr[k];
        t->ar[k] += x * pr - old * qr;
        t->ai[k] += x * pi - old * qi;
        t->pr[k] = pr * t->sr[k] - pi * t->si[k];
        t->pi[k] = pr * t->si[k] + pi * t->sr[k];
    }
    t->n++;
    if ((t->n & 4095) == 0)                       /* keep the phasors on the unit circle */
        for (int k = 0; k < TS_NB; k++) {
            double m = 1.0 / hypot(t->pr[k], t->pi[k]);
            t->pr[k] *= m; t->pi[k] *= m;
        }
    if ((t->n & 65535) == 0) {                    /* and the sums exact: recompute them */
        for (int k = 0; k < TS_NB; k++) {
            double ar = 0, ai = 0, w = 2 * M_PI * (TS_F0 + k * TS_DF) / t->fs;
            for (int i = 0; i < t->nw; i++) {     /* sample i steps back from n-1 */
                double s = t->x[(t->pos - 1 - i + 2 * t->nw) % t->nw];
                /* the phasor of the current n is p(n); sample n-1-i has p(n) * e^{+jw(i+1)} */
                double qr = cos(w * (i + 1)), qi = sin(w * (i + 1));
                ar += s * (t->pr[k] * qr - t->pi[k] * qi);
                ai += s * (t->pr[k] * qi + t->pi[k] * qr);
            }
            t->ar[k] = ar; t->ai[k] = ai;
        }
    }
    if (t->n < t->nw) return -1;
    /* window centre in FM-stream samples */
    double c = (double)(t->n - 1) - (t->nw - 1) / 2.0 + t->delay;
    if (t->n == t->nw) t->row = (long)ceil(c * 1000.0 / t->fs);     /* first full window */
    if (c < t->row * t->fs / 1000.0) return -1;
    long j = t->row++;
    double g = 1.0 / ((double)t->nw * t->nw);
    for (int k = 0; k < TS_NB; k++) row[k] = (float)((t->ar[k] * t->ar[k] + t->ai[k] * t->ai[k]) * g);
    return j;
}

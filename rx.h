/* rx.h - SSTV receiver: VIS detection, sync tracking, slant correction */
#ifndef RX_H
#define RX_H

#include "sstv.h"
#include "image.h"

/* Demodulated frequency stream + prefix sums for fast window averages */
typedef struct {
    const float *f;
    long   n;
    double fs;
    double *P, *Q;     /* prefix sums of f and f^2, length n+1 */
    /* optional tone powers (tones.h), NULL if not available: row r is centred on
       sample t0f + r * tstep of f; used to find weak VIS headers */
    const float *T;
    long   tn;
    const float *y;    /* optional complex baseband of the discriminator (re, im per sample of f) */
    double t0f, tstep;
} sstv_fbuf;

int  sstv_fbuf_init(sstv_fbuf *b, const float *f, long n, double fs);
void sstv_fbuf_free(sstv_fbuf *b);

typedef struct {
    int    code;        /* VIS code, -1: some bits unreadable (QRM) */
    int    complete;    /* all bits read, parity ok, mode known */
    unsigned long long cand;   /* modes that fit the readable bits (bit i = mode index i) */
    double offset;      /* measured mistuning, Hz (leader - 1900) */
    double img_start;   /* sample index right after the stop bit */
} sstv_vis;

/* Searches for the next VIS header starting at sample 'from'. 1 = found.
 * A header damaged by QRM is still found (code -1, cand = possible modes). */
int sstv_vis_find(const sstv_fbuf *b, long from, sstv_vis *v);

/* Damaged header: picks the mode from the sync pulses of the first lines (the
 * readable bits only give a bonus). final: decide with the audio there is.
 * 1 = *out set, 0 = not enough audio yet, -1 = no mode fits. */
int sstv_vis_resolve(const sstv_fbuf *b, const sstv_vis *v, int final, const sstv_mode **out);

/* Without VIS and without knowing the mode: finds a regular train of sync
 * pulses (at least 10) and the mode whose timing matches it. */
int sstv_auto_find(const sstv_fbuf *b, long from, double off, const sstv_mode **mode, double *t0);

/* Without VIS: looks for a periodic sync pattern of the given mode. */
int sstv_sync_find(const sstv_fbuf *b, const sstv_mode *m, long from, double off, double *t0);

typedef struct {
    int    syncs;       /* sync pulses used in the final fit */
    double line_ms;     /* measured line period */
    double clock_ppm;   /* estimated TX/RX clock difference */
    double end;         /* sample index of image end */
    int    last_sync;   /* last line where a sync pulse was seen, -1: none */
    double offset;      /* mistuning used (measured on the syncs if off was NAN) */
} sstv_rxinfo;

#define SSTV_NR 1      /* sstv_decode_ex flag: remove impulse noise (QRM specks) */

/* Decodes one image whose first line starts at sample t0 (after VIS).
 * off: mistuning from the VIS; NAN = measure it on the sync pulses. */
image *sstv_decode(const sstv_fbuf *b, const sstv_mode *m, double t0, double off, sstv_rxinfo *info);
image *sstv_decode_ex(const sstv_fbuf *b, const sstv_mode *m, double t0, double off, int flags, sstv_rxinfo *info);

#endif

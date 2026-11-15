/* tx.c - SSTV encoder
 *
 * Phase-continuous FM. Segment boundaries are computed from the absolute
 * time since the start, so rounding never accumulates over the image.
 */
#include <math.h>
#include <stdlib.h>
#include "tx.h"

typedef struct {
    double fs, k, foff, amp, ph, T;   /* T: transmitter time in seconds */
    long   n, cap;
    float *buf;
} txs;

void sstv_tx_defaults(sstv_tx_opts *o)
{
    o->fs = 11025;
    o->clock_ppm = 0;
    o->freq_offset = 0;
    o->snr_db = -1000;
    o->lead_ms = 500;
    o->tail_ms = 500;
    o->seed = 12345;
}

static void push(txs *t, double f)
{
    if (t->n >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1 << 16;
        t->buf = realloc(t->buf, (size_t)t->cap * sizeof(float));
    }
    if (f <= 0) { t->buf[t->n++] = 0; return; }
    t->buf[t->n++] = (float)(t->amp * sin(t->ph));
    t->ph += 2 * M_PI * (f * t->k + t->foff) / t->fs;
    if (t->ph > 2 * M_PI) t->ph -= 2 * M_PI;
}

/* sample index (receiver time) where transmitter time T falls */
static long sample_at(const txs *t, double T) { return llround(T * t->fs / t->k); }

static void tone(txs *t, double f, double ms)
{
    double T1 = t->T + ms / 1000.0;
    long end = sample_at(t, T1);
    while (t->n < end) push(t, f);
    t->T = T1;
}

/* one colour/luma scan; vals[] holds the 0..255 value of every pixel */
static void scan(txs *t, const unsigned char *vals, int w, double ms)
{
    double T0 = t->T, T1 = T0 + ms / 1000.0, pd = (ms / 1000.0) / w;
    long end = sample_at(t, T1);
    while (t->n < end) {
        double tt = t->n * t->k / t->fs - T0;
        int p = (int)(tt / pd);
        if (p < 0) p = 0;
        if (p >= w) p = w - 1;
        push(t, SSTV_F_BLACK + (SSTV_F_WHITE - SSTV_F_BLACK) * vals[p] / 255.0);
    }
    t->T = T1;
}

/* pixel values of one scan segment for transmitted line 'line' */
static void seg_values(const sstv_mode *m, const image *img, int line, const sstv_seg *g,
                       unsigned char *out)
{
    int w = img->w;
    for (int x = 0; x < w; x++) {
        if (g->ch <= CH_B) {
            out[x] = img->px[((line * m->rows + g->row) * w + x) * 3 + g->ch];
            continue;
        }
        double acc = 0;
        int cnt = 0;
        for (int r = 0; r < m->rows; r++) {
            if (g->ch == CH_Y && r != g->row) continue;   /* luma: own row only */
            double y, cr, cb;
            sstv_rgb_to_ycc(&img->px[((line * m->rows + r) * w + x) * 3], &y, &cr, &cb);
            acc += g->ch == CH_Y ? y : g->ch == CH_CR ? cr : cb;
            cnt++;
        }
        double v = acc / cnt;                            /* chroma: average of the rows */
        out[x] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
    }
}

static void vis_header(txs *t, int code)
{
    tone(t, 1900, 300);
    tone(t, 1200, 10);
    tone(t, 1900, 300);
    tone(t, 1200, 30);                       /* start bit */
    int ones = 0;
    for (int i = 0; i < 7; i++) {            /* LSB first, 1 = 1100 Hz, 0 = 1300 Hz */
        int b = (code >> i) & 1;
        ones += b;
        tone(t, b ? 1100 : 1300, 30);
    }
    tone(t, (ones & 1) ? 1100 : 1300, 30);   /* even parity */
    tone(t, 1200, 30);                       /* stop bit */
}

static double gauss(unsigned *s)
{
    double u1, u2;
    do {
        *s = *s * 1103515245u + 12345u; u1 = ((*s >> 8) + 0.5) / 16777216.0;
        *s = *s * 1103515245u + 12345u; u2 = ((*s >> 8) + 0.5) / 16777216.0;
    } while (u1 <= 0);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

float *sstv_encode(const sstv_mode *m, const image *src, const sstv_tx_opts *o, long *nout)
{
    image *img = image_resize(src, m->width, m->height);
    txs t = {0};
    t.fs = o->fs;
    t.k = 1.0 + o->clock_ppm * 1e-6;
    t.foff = o->freq_offset;
    t.amp = 0.5;

    tone(&t, 0, o->lead_ms);
    vis_header(&t, m->vis);
    if (m->pre_sync_ms > 0) tone(&t, SSTV_F_SYNC, m->pre_sync_ms);

    unsigned char *vals = malloc((size_t)img->w);
    for (int line = 0; line < sstv_nlines(m); line++)
        for (int s = 0; s < m->nseg; s++) {
            const sstv_seg *g = &m->seg[s];
            if (g->type == SEG_SCAN) {
                seg_values(m, img, line, g, vals);
                scan(&t, vals, img->w, g->ms);
            } else {
                tone(&t, g->freq, g->ms);
            }
        }
    free(vals);
    tone(&t, 0, o->tail_ms);

    if (o->snr_db > -100) {
        /* SNR referred to 3 kHz: sigma^2 = Ps / (3000 * snr) * fs/2 */
        double ps = t.amp * t.amp / 2;
        double sigma = sqrt(ps / (3000.0 * pow(10, o->snr_db / 10)) * o->fs / 2);
        unsigned seed = o->seed;
        for (long i = 0; i < t.n; i++) t.buf[i] += (float)(sigma * gauss(&seed));
    }

    image_free(img);
    *nout = t.n;
    return t.buf;
}

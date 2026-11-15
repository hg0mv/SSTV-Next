/* rx.c - SSTV receiver: VIS detection, sync tracking, slant correction
 *
 * Works on the demodulated frequency stream. All window averages come from
 * prefix sums, so every check is O(1) regardless of window length.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rx.h"

int sstv_fbuf_init(sstv_fbuf *b, const float *f, long n, double fs)
{
    b->f = f;
    b->n = n;
    b->fs = fs;
    b->P = malloc((size_t)(n + 1) * sizeof(double));
    b->Q = malloc((size_t)(n + 1) * sizeof(double));
    if (!b->P || !b->Q) return -1;
    b->P[0] = b->Q[0] = 0;
    for (long i = 0; i < n; i++) {
        b->P[i + 1] = b->P[i] + f[i];
        b->Q[i + 1] = b->Q[i] + (double)f[i] * f[i];
    }
    return 0;
}

void sstv_fbuf_free(sstv_fbuf *b)
{
    free(b->P);
    free(b->Q);
    b->P = b->Q = NULL;
}

/* mean of f over sample range [a, c) */
static double mean_rng(const sstv_fbuf *b, double a, double c)
{
    long i = (long)floor(a), j = (long)floor(c);
    if (i < 0) i = 0;
    if (j > b->n) j = b->n;
    if (j <= i) return NAN;
    return (b->P[j] - b->P[i]) / (double)(j - i);
}

/* exact average of the piecewise-constant signal over [x0, x1) (fractional) */
static double avg_frac(const sstv_fbuf *b, double x0, double x1)
{
    if (x0 < 0) x0 = 0;
    if (x1 > b->n) x1 = (double)b->n;
    if (x1 <= x0) return NAN;
    long i0 = (long)x0, i1 = (long)x1;
    double F0 = b->P[i0] + (i0 < b->n ? (x0 - i0) * b->f[i0] : 0);
    double F1 = b->P[i1] + (i1 < b->n ? (x1 - i1) * b->f[i1] : 0);
    return (F1 - F0) / (x1 - x0);
}

/* ---------------------------------------------------------------- VIS --- */

/* k-th smallest of v[0..n-1] (Hoare selection, O(n), reorders v) */
static float select_k(float *v, long n, long k)
{
    long lo = 0, hi = n - 1;
    while (lo < hi) {
        float piv = v[(lo + hi) / 2];
        long i = lo, j = hi;
        while (i <= j) {
            while (v[i] < piv) i++;
            while (v[j] > piv) j--;
            if (i <= j) { float t = v[i]; v[i] = v[j]; v[j] = t; i++; j--; }
        }
        if (k <= j) hi = j; else if (k >= i) lo = i; else return v[k];
    }
    return v[k];
}

/* median of f over [a, c): a voice burst covering part of the window does not move it */
static double median_rng(const sstv_fbuf *b, double a, double c, float *tmp)
{
    long i = (long)floor(a), j = (long)floor(c);
    if (i < 0) i = 0;
    if (j > b->n) j = b->n;
    if (j <= i) return NAN;
    long n = j - i;
    memcpy(tmp, b->f + i, (size_t)n * sizeof(float));
    return select_k(tmp, n, n / 2);
}

/* s = sample where the start bit begins. Returns error (>=0) or -1 if this is no
 * VIS frame. Robust against voice QRM (contests on 14.230): medians for every
 * window, so a burst over part of a bit does not move it; a burst over whole bits
 * only makes those bits unknown (known: bit i set = bit i readable, bit 7 = parity)
 * while the frame - and so the exact start of the picture - is still found. */
static double vis_eval(const sstv_fbuf *b, double s, int *code, unsigned *known, double *leader)
{
    double ms = b->fs / 1000.0;
    if (s - 312 * ms < 0 || s + 300 * ms >= b->n) return -1;

    /* cheap pre-check: a 1900 Hz-ish leader and at least one 1200 Hz framing bit */
    double ml = mean_rng(b, s - 250 * ms, s - 5 * ms);
    double m0 = mean_rng(b, s + 4 * ms, s + 26 * ms), m9 = mean_rng(b, s + 274 * ms, s + 296 * ms);
    if (!(ml > 1450 && ml < 2350)) return -1;
    if (!((m0 > 900 && m0 < 1500) || (m9 > 900 && m9 < 1500))) return -1;

    float *tmp = malloc((size_t)(260 * ms + 8) * sizeof(float));
    double err = -1, d;
    /* the short framing windows first (by median, absolute range): rejects most positions
     * before the long leader median is computed */
    double mb = median_rng(b, s - 308 * ms, s - 302 * ms, tmp);     /* 10 ms break */
    double ms0 = median_rng(b, s + 4 * ms, s + 26 * ms, tmp);       /* start bit */
    double ms9 = median_rng(b, s + 274 * ms, s + 296 * ms, tmp);    /* stop bit */
    if ((mb > 850 && mb < 1550) + (ms0 > 870 && ms0 < 1530) + (ms9 > 870 && ms9 < 1530) < 2) goto out;
    double L = median_rng(b, s - 250 * ms, s - 5 * ms, tmp);
    if (!(fabs(L - 1900) <= 250)) goto out;
    {   /* the leader must mostly be one tone: >= 50 % of it within +-120 Hz */
        long i0 = (long)(s - 250 * ms), i1 = (long)(s - 5 * ms), in = 0;
        for (long i = i0; i < i1; i++) in += fabsf(b->f[i] - (float)L) <= 120.0f;
        if (in * 2 < i1 - i0) goto out;
    }
    double ref = L - 700.0;     /* 1200 Hz, corrected for mistuning */
    double e = 0;
    int frame = 0;

    d = mb - ref;                                                  /* 10 ms break between the leaders */
    if (fabs(d) <= 100) frame++;
    d = ms0 - ref;                                                 /* start bit */
    if (fabs(d) <= 80) { frame++; e += d * d; } else e += 80 * 80;
    d = ms9 - ref;                                                 /* stop bit */
    if (fabs(d) <= 80) { frame++; e += d * d; } else e += 80 * 80;
    if (frame < 2) goto out;

    int c = 0;
    unsigned kn = 0;
    for (int i = 0; i < 8; i++) {                                  /* 7 data bits + parity */
        double t0 = 30.0 * (i + 1), t1 = t0 + 30.0;
        d = median_rng(b, s + (t0 + 4) * ms, s + (t1 - 4) * ms, tmp) - ref;   /* 1: -100 Hz, 0: +100 Hz */
        double dev = fabs(d) - 100.0;
        if (fabs(dev) <= 70) { kn |= 1u << i; e += dev * dev; if (d < 0) c |= 1 << i; }
        else e += 70 * 70;
    }
    *code = c;
    *known = kn;
    *leader = L;
    err = e;
out:
    free(tmp);
    return err;
}

static int parity7(int v) { int p = 0; for (int i = 0; i < 7; i++) p ^= (v >> i) & 1; return p; }

/* modes that agree with the readable bits (bit i of the mask = mode index i) */
static unsigned long long vis_candidates(int code, unsigned known)
{
    unsigned long long m = 0;
    int full = (known & 0xFF) == 0xFF, pok = full && parity7(code & 0x7F) == ((code >> 7) & 1);
    for (int i = 0; i < sstv_mode_count() && i < 64; i++) {
        int v = sstv_mode_at(i)->vis, bad = 0;
        for (int k = 0; k < 7; k++) if ((known >> k & 1) && ((v ^ code) >> k & 1)) bad++;
        if ((known >> 7 & 1) && !full && parity7(v) != ((code >> 7) & 1)) bad++;
        if (bad == 0 || (full && !pok && bad <= 1)) m |= 1ull << i;   /* parity error: one bit may be wrong */
    }
    return m;
}

int sstv_vis_find(const sstv_fbuf *b, long from, sstv_vis *v)
{
    double ms = b->fs / 1000.0;
    for (long s = from + (long)(312 * ms); s + (long)(300 * ms) < b->n; s++) {
        int code;
        unsigned known;
        double L;
        if (vis_eval(b, s, &code, &known, &L) < 0) continue;

        /* exact position: slide the whole header pattern (end of the leader, start bit,
         * readable bits, stop bit) over +-15 ms and take the best match. Errors are
         * clipped, so voice bursts cannot pull it like they pull a single edge. */
        for (long t = s + 1; t <= s + (long)(20 * ms); t++) {      /* most readable bits first */
            int c2; unsigned k2; double L2;
            if (vis_eval(b, t, &c2, &k2, &L2) >= 0 && __builtin_popcount(k2) > __builtin_popcount(known)) { code = c2; known = k2; L = L2; }
        }
        double ref = L - 700.0, tm[11];
        unsigned use = 0;
        tm[0] = ref; use |= 1;                                      /* start bit */
        for (int i = 0; i < 8; i++) if (known >> i & 1) { tm[i + 1] = ref + ((code >> i & 1) ? -100 : 100); use |= 1u << (i + 1); }
        tm[9] = ref; use |= 1u << 9;                                /* stop bit */
        long best = s;
        double bc = 1e300;
        long lo = s - (long)(15 * ms), hi = s + (long)(25 * ms);
        if (lo < (long)(312 * ms)) lo = (long)(312 * ms);
        for (long t = lo; t <= hi && t + (long)(300 * ms) < b->n; t++) {
            double cst = 0;
            long l0 = t - (long)(30 * ms);                          /* last 30 ms of the leader */
            for (long i = l0; i < t; i++) cst += fmin(fabs(b->f[i] - L), 150.0);
            for (int k = 0; k < 10; k++) {
                if (!(use >> k & 1)) continue;
                long i0 = t + (long)(30 * k * ms), i1 = t + (long)(30 * (k + 1) * ms);
                for (long i = i0; i < i1; i++) cst += fmin(fabs(b->f[i] - tm[k]), 150.0);
            }
            if (cst < bc) { bc = cst; best = t; }
        }
        {
            int c2; unsigned k2; double L2;
            if (vis_eval(b, best, &c2, &k2, &L2) >= 0) {
                s = best;
                if (__builtin_popcount(k2) >= __builtin_popcount(known)) { code = c2; known = k2; L = L2; }
            }
        }

        v->offset = L - 1900.0;
        v->img_start = s + 300 * ms;
        v->cand = vis_candidates(code, known);
        v->complete = (known & 0xFF) == 0xFF && parity7(code & 0x7F) == ((code >> 7) & 1) && sstv_mode_by_vis(code & 0x7F);
        v->code = v->complete ? (code & 0x7F) : -1;
        if (!v->cand) v->cand = ~0ull >> (64 - (sstv_mode_count() < 64 ? sstv_mode_count() : 64));
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- sync --- */

/* Prefix sums of a soft "is this sync" indicator: 1 at 1200 Hz, 0 at 1500 Hz */
typedef struct {
    long    r0, r1;
    int     W;
    double *S;
} syncbox;

static void syncbox_init(syncbox *sb, const sstv_fbuf *b, long r0, long r1, int W, double off)
{
    if (r0 < 0) r0 = 0;
    if (r1 > b->n) r1 = b->n;
    if (r1 < r0) r1 = r0;
    sb->r0 = r0;
    sb->r1 = r1;
    sb->W = W;
    sb->S = malloc((size_t)(r1 - r0 + 1) * sizeof(double));
    sb->S[0] = 0;
    for (long i = r0; i < r1; i++) {
        double x = (SSTV_F_BLACK + off - b->f[i]) / 300.0;
        if (x < 0) x = 0;
        if (x > 1) x = 1;
        sb->S[i - r0 + 1] = sb->S[i - r0] + x;
    }
}

/* fraction of the W-sample window starting at p that looks like sync */
static double syncbox_at(const syncbox *sb, long p)
{
    if (p < sb->r0 || p + sb->W > sb->r1) return 0;
    return (sb->S[p - sb->r0 + sb->W] - sb->S[p - sb->r0]) / sb->W;
}

static long syncbox_peak(const syncbox *sb, long lo, long hi, double *val)
{
    long best = -1;
    double bv = -1;
    for (long p = lo; p <= hi; p++) {
        double v = syncbox_at(sb, p);
        if (v > bv) { bv = v; best = p; }
    }
    *val = bv;
    return best;
}

int sstv_sync_find(const sstv_fbuf *b, const sstv_mode *m, long from, double off, double *t0)
{
    double ms = b->fs / 1000.0, Ln = sstv_line_ms(m) * ms;
    int si = sstv_sync_seg(m);
    int W = (int)lround(m->seg[si].ms * ms);
    if (W < 1) W = 1;
    syncbox sb;
    syncbox_init(&sb, b, from, b->n, W, off);

    int found = 0;
    for (long p = from; p + (long)(3 * Ln) < b->n; p++) {
        if (syncbox_at(&sb, p) < 0.7) continue;
        double v1, v2;
        long q1 = syncbox_peak(&sb, p + (long)(0.99 * Ln), p + (long)(1.01 * Ln), &v1);
        long q2 = syncbox_peak(&sb, p + (long)(1.98 * Ln), p + (long)(2.02 * Ln), &v2);
        if (v1 < 0.7 || v2 < 0.7 || q1 < 0 || q2 < 0) continue;
        double v;
        long pk = syncbox_peak(&sb, p, p + W / 2, &v);
        *t0 = pk - (m->pre_sync_ms + sstv_seg_start_ms(m, si)) * ms;
        found = 1;
        break;
    }
    free(sb.S);
    return found;
}

/* -------------------------------------------------- damaged VIS fix --- */

/* how well the sync pulses after t0 fit mode m over its first K lines:
 * hits at the expected places, minus pulses half way between them */
static double sync_fit(const syncbox *sb, const sstv_mode *m, double t0, double ms, int K)
{
    double Ln = sstv_line_ms(m) * ms, pos[2 * 16 * 2];
    int np = 0, hits = 0, extra = 0;
    for (int k = 0; k < K; k++)
        for (int j = 0; j < m->nseg && np < (int)(sizeof pos / sizeof pos[0]); j++)
            if (m->seg[j].type == SEG_SYNC) pos[np++] = t0 + (m->pre_sync_ms + sstv_seg_start_ms(m, j)) * ms + k * Ln;
    for (int i = 0; i < np; i++) {
        double w = 3 * ms + 0.002 * (pos[i] - t0), v;
        syncbox_peak(sb, (long)(pos[i] - w), (long)(pos[i] + w), &v);
        hits += v >= 0.6;
        if (i + 1 < np) {
            double q = 0.5 * (pos[i] + pos[i + 1]);
            syncbox_peak(sb, (long)(q - 3 * ms), (long)(q + 3 * ms), &v);
            extra += v >= 0.6;
        }
    }
    if (getenv("SSTV_DEBUG")) fprintf(stderr, "    %-14s K %d syncs %d hits %d extra %d\n", m->name, K, np, hits, extra);
    return np ? (hits - extra) / (double)np : -1;
}

int sstv_vis_resolve(const sstv_fbuf *b, const sstv_vis *v, int final, const sstv_mode **out)
{
    /* Every mode is scored on its sync pulses; the readable VIS bits only give a
     * bonus, because a strong voice burst can also flip a bit, not just blur it. */
    double ms = b->fs / 1000.0, t0 = v->img_start, need = 0;
    int nm = sstv_mode_count() < 64 ? sstv_mode_count() : 64;
    int all = (v->cand & (~0ull >> (64 - nm))) == (~0ull >> (64 - nm));
    for (int i = 0; i < nm; i++) {
        const sstv_mode *m = sstv_mode_at(i);
        double e = (m->pre_sync_ms + 6 * sstv_line_ms(m)) * ms + 20 * ms;
        if (e > need) need = e;
    }
    if (b->n < (long)(t0 + need)) {
        if (!final) return 0;                                   /* wait for 6 lines of the slowest */
        need = b->n - t0;
    }
    syncbox sb;
    int W = (int)lround(4.5 * ms);                              /* shortest sync (Martin 4.86 ms) */
    syncbox_init(&sb, b, (long)(t0 - 20 * ms), (long)(t0 + need + 20 * ms), W, v->offset);
    const sstv_mode *best = NULL;
    double bs = -1e9, bfit = -1;
    for (int i = 0; i < nm; i++) {
        const sstv_mode *m = sstv_mode_at(i);
        int K = 6;
        while (K > 3 && (m->pre_sync_ms + K * sstv_line_ms(m)) * ms > need) K--;
        if ((m->pre_sync_ms + K * sstv_line_ms(m)) * ms > need + 20 * ms) continue;   /* too little audio for it */
        double fit = sync_fit(&sb, m, t0, ms, K);
        double sc = fit + (!all && (v->cand >> i & 1) ? 0.25 : 0);
        if (getenv("SSTV_DEBUG")) fprintf(stderr, "  resolve %-14s fit %.2f%s\n", m->name, fit, !all && (v->cand >> i & 1) ? " (VIS bits fit)" : "");
        if (sc > bs + 1e-9) { bs = sc; bfit = fit; best = m; }  /* equal timing (S1/S3 ...): the first one */
    }
    free(sb.S);
    if (!best || bfit < 0.5) return -1;
    *out = best;
    return 1;
}

/* ---------------------------------------------------- auto detection --- */

/* Mode recognition without VIS: find the 1200 Hz sync pulses, then look for a
 * regular train of them whose spacing and pulse length match a mode. */
typedef struct { long s; double len; } pulse;

static int find_pulses(const sstv_fbuf *b, long from, double off, pulse *p, int maxp)
{
    double ms = b->fs / 1000.0, thr = 1350.0 + off;
    long h = (long)(0.5 * ms) + 1, start = -1;
    int np = 0;
    for (long i = from + h; i < b->n - h && np < maxp; i++) {
        int in = (b->P[i + h] - b->P[i - h]) / (2 * h) < thr;     /* 1 ms average */
        if (in && start < 0) start = i;
        else if (!in && start >= 0) {
            double len = (double)(i - start);
            if (len >= 3.5 * ms && len <= 24 * ms) { p[np].s = start; p[np].len = len; np++; }
            start = -1;
        }
    }
    return np;
}

/* how well the tones of the line starting at 'ls' match the mode (lower is better) */
static double tone_error(const sstv_fbuf *b, const sstv_mode *m, double ls, double off)
{
    double ms = b->fs / 1000.0, e = 0;
    int k = 0;
    for (int j = 0; j < m->nseg; j++) {
        const sstv_seg *g = &m->seg[j];
        if (g->type == SEG_SCAN || g->ms < 1.0) continue;
        double a = ls + sstv_seg_start_ms(m, j) * ms, d = g->ms * ms;
        double v = mean_rng(b, a + 0.2 * d, a + 0.8 * d);
        if (isnan(v)) continue;
        e += fabs(v - (g->freq + off)); k++;
    }
    return k ? e / k : 1e9;
}

int sstv_auto_find(const sstv_fbuf *b, long from, double off, const sstv_mode **mode, double *t0)
{
    enum { MAXP = 2048 };
    pulse *p = malloc(MAXP * sizeof *p);
    int np = find_pulses(b, from, off, p, MAXP);
    double ms = b->fs / 1000.0;
    const sstv_mode *best = NULL;
    int bestn = 0, bestfirst = -1;
    double bestscore = 7.0;          /* at least ~8 clean periods */

    for (int mi = 0; mi < sstv_mode_count(); mi++) {
        const sstv_mode *m = sstv_mode_at(mi);
        /* same timing as an earlier mode (S3 = S1 ...): cannot be told apart without VIS */
        int dup = 0;
        for (int k = 0; k < mi && !dup; k++) {
            const sstv_mode *o = sstv_mode_at(k);
            dup = fabs(sstv_line_ms(o) - sstv_line_ms(m)) < 0.01 && sstv_sync_count(o) == sstv_sync_count(m) && o->height >= m->height;
        }
        if (dup) continue;
        double sp = sstv_line_ms(m) * ms / sstv_sync_count(m);
        double sl = m->seg[sstv_sync_seg(m)].ms * ms;
        double ltol = fmax(1.5 * ms, 0.35 * sl);
        for (int i = 0; i < np; i++) {
            if (fabs(p[i].len - sl) > ltol) continue;
            int cnt = 1, last = i, miss = 0;
            for (int j = i + 1; j < np; j++) {
                double d = (double)(p[j].s - p[last].s);
                if (d > 3.5 * sp) break;
                double k = floor(d / sp + 0.5);
                if (k < 1 || fabs(d - k * sp) > 1.5 * ms + 0.003 * k * sp) continue;
                if (fabs(p[j].len - sl) > ltol) continue;
                cnt++; miss += (int)k - 1; last = j;
            }
            /* missing pulses cost: a Robot 72 train must not pass for Robot 36 with every other pulse missing */
            double score = cnt - 0.7 * miss;
            if (cnt >= 10 && score > bestscore) { bestscore = score; bestn = cnt; best = m; bestfirst = i; }
            if (cnt >= 10) break;           /* first good train of this mode is enough */
        }
    }
    int ok = 0;
    if (best && bestn >= 10) {
        const sstv_mode *m = best;
        double ps = (double)p[bestfirst].s;
        int si = sstv_sync_seg(m), ns = sstv_sync_count(m);
        double ls = ps - sstv_seg_start_ms(m, si) * ms;           /* pulse = first sync of a line */
        if (ns > 1) {
            /* Robot 36: is this pulse the even (R-Y) or the odd (B-Y) half? */
            double sp = sstv_line_ms(m) * ms / ns, e0 = 0, e1 = 0;
            for (int k = 0; k < 4; k++) {
                e0 += tone_error(b, m, ls + k * ns * sp, off);
                e1 += tone_error(b, m, ls - sp + (k + 1) * ns * sp, off);
            }
            if (e1 < e0) ls += sp;                                /* start at the next even line */
        }
        *mode = m;
        *t0 = ls - m->pre_sync_ms * ms;
        if (*t0 < 0) *t0 = 0;
        ok = 1;
    }
    free(p);
    return ok;
}

/* ---------------------------------------------------------- line fit --- */

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof(double), cmp_dbl);
    return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* Robust fit T = a + b*K: Theil-Sen start, then least squares on inliers.
 * Returns the number of inliers. */
static int fit_robust(const int *K, const double *T, int n, double tol, double *a, double *b)
{
    if (n < 2) return 0;
    int npair = 0, cap = n * (n - 1) / 2;
    double *tmp = malloc((size_t)(cap > n ? cap : n) * sizeof(double));
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (K[j] - K[i] >= 4) tmp[npair++] = (T[j] - T[i]) / (K[j] - K[i]);
    if (npair == 0) { free(tmp); return 0; }
    double bb = median(tmp, npair);
    for (int i = 0; i < n; i++) tmp[i] = T[i] - bb * K[i];
    double aa = median(tmp, n);
    free(tmp);

    int cnt = 0;
    for (int it = 0; it < 2; it++) {
        double sk = 0, st = 0, skk = 0, skt = 0;
        cnt = 0;
        for (int i = 0; i < n; i++) {
            if (fabs(T[i] - (aa + bb * K[i])) > tol) continue;
            sk += K[i]; st += T[i]; skk += (double)K[i] * K[i]; skt += K[i] * T[i];
            cnt++;
        }
        double den = cnt * skk - sk * sk;
        if (cnt < 2 || den <= 0) break;
        bb = (cnt * skt - sk * st) / den;
        aa = (st - bb * sk) / cnt;
    }
    *a = aa;
    *b = bb;
    return cnt;
}

/* ------------------------------------------------------------ decode --- */

/* ------------------------------------------------------ noise removal --- */

static int med3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

/* Interference hits a picture along its lines (time runs along a line), so a
 * damaged pixel stands out from its upper and lower neighbours, which were sent
 * a whole line earlier/later. A pixel is replaced by the vertical median only if
 * it is an outlier both vertically and horizontally: thin vertical lines (equal
 * up/down) and thin horizontal lines (equal left/right) are left alone. */
static void despeckle(unsigned char *pl, int w, int h, int thr, const unsigned char *rows_ok)
{
    unsigned char *src = malloc((size_t)w * h);
    for (int pass = 0; pass < 2; pass++) {
        memcpy(src, pl, (size_t)w * h);
        for (int y = 1; y < h - 1; y++) {
            if (!rows_ok[y - 1] || !rows_ok[y] || !rows_ok[y + 1]) continue;
            const unsigned char *u = src + (size_t)(y - 1) * w, *c = src + (size_t)y * w, *d = src + (size_t)(y + 1) * w;
            unsigned char *o = pl + (size_t)y * w;
            for (int x = 1; x < w - 1; x++) {
                int p = c[x], mv = med3(u[x], p, d[x]), mh = med3(c[x - 1], p, c[x + 1]);
                if (abs(p - mv) > thr && abs(p - mh) > thr) o[x] = (unsigned char)mv;
            }
        }
    }
    free(src);
}

image *sstv_decode(const sstv_fbuf *b, const sstv_mode *m, double t0, double off, sstv_rxinfo *info)
{
    return sstv_decode_ex(b, m, t0, off, 0, info);
}

image *sstv_decode_ex(const sstv_fbuf *b, const sstv_mode *m, double t0, double off, int flags, sstv_rxinfo *info)
{
    double ms = b->fs / 1000.0;
    double Ln = sstv_line_ms(m) * ms;              /* nominal line, samples */
    int si = sstv_sync_seg(m), H = sstv_nlines(m), Wd = m->width;   /* H: transmitted lines */
    double sync_in_line = sstv_seg_start_ms(m, si);
    int W = (int)lround(m->seg[si].ms * ms);
    if (W < 1) W = 1;

    syncbox sb;
    int measure_off = isnan(off);              /* no VIS: measure the mistuning on the sync pulses */
    if (measure_off) off = 0;
    syncbox_init(&sb, b, (long)(t0 - 50 * ms), (long)(t0 + (H + 2) * Ln * 1.02), W, off);

    /* model: sync of line k starts at a + bb*k */
    double a = t0 + (m->pre_sync_ms + sync_in_line) * ms, bb = Ln;
    double win = 0.03 * Ln;
    if (win < 4 * ms) win = 4 * ms;

    int *K = malloc((size_t)H * sizeof(int));
    double *T = malloc((size_t)H * sizeof(double));
    int np = 0, last_sync = -1;
    double sk = 0, st = 0, skk = 0, skt = 0;

    /* pass 1: track the sync pulses line by line */
    for (int k = 0; k < H; k++) {
        double pred = a + bb * k, v;
        long p = syncbox_peak(&sb, (long)(pred - win), (long)(pred + win), &v);
        if (p < 0 || v < 0.6) continue;
        last_sync = k;
        K[np] = k;
        T[np] = (double)p;
        np++;
        sk += k; st += p; skk += (double)k * k; skt += (double)k * p;
        if (np >= 8 && K[np - 1] - K[0] >= 8) {
            double den = np * skk - sk * sk;
            double nb = (np * skt - sk * st) / den;
            if (fabs(nb / Ln - 1) < 0.01) { bb = nb; a = (st - bb * sk) / np; }
        } else {
            a = st / np - bb * sk / np;   /* keep nominal slope, fix offset */
        }
    }

    /* pass 2: robust final fit -> slant correction */
    int used = 0;
    if (np >= 8) {
        double fa, fb;
        used = fit_robust(K, T, np, 1.5 * ms, &fa, &fb);
        if (used >= 8 && fabs(fb / Ln - 1) < 0.01) { a = fa; bb = fb; }
        else used = 0;
    }
    if (used >= 8) {
        /* last line whose sync fits the line model: foreign pulses (another station
         * starting with a different line rate) must not keep a finished picture alive */
        unsigned char *in = calloc((size_t)H, 1);
        for (int i = 0; i < np; i++) if (fabs(T[i] - (a + bb * K[i])) <= 1.5 * ms) in[K[i]] = 1;
        int ls = -1;
        for (int k = 0; k < H; k++) {
            if (!in[k]) continue;
            int run = 0;
            for (int j = k - 4; j <= k; j++) run += j >= 0 && in[j];
            if (run >= 3 || k < 4) ls = k;           /* sporadic coincidences do not count */
        }
        free(in);
        last_sync = ls;
    }
    if (measure_off && np >= 8) {
        /* mean frequency in the middle of every sync pulse that fits the line model */
        double acc = 0; int cnt = 0;
        for (int i = 0; i < np; i++) {
            if (fabs(T[i] - (a + bb * K[i])) > 1.5 * ms) continue;
            double v = mean_rng(b, T[i] + 0.2 * W, T[i] + 0.8 * W);
            if (!isnan(v)) { acc += v; cnt++; }
        }
        if (cnt >= 8) off = acc / cnt - SSTV_F_SYNC;
    }
    free(K);
    free(T);
    free(sb.S);

    /* pass 3: render, every segment scaled with the measured line period.
     * Values go into per-channel planes (RGB, or Y/Cr/Cb for PD) first. */
    double r = bb / Ln;
    int ycc = sstv_is_ycc(m);
    size_t plane = (size_t)Wd * m->height;
    unsigned char *pl = calloc(3 * plane, 1);      /* [ch][row][x], ch: R/G/B or Y/Cr/Cb */
    if (ycc) memset(pl + plane, 128, 2 * plane);    /* neutral chroma: lines not received yet stay black */
    for (int k = 0; k < H; k++) {
        double s = a + bb * k;
        for (int j = 0; j < m->nseg; j++) {
            const sstv_seg *g = &m->seg[j];
            if (g->type != SEG_SCAN) continue;
            int ch = ycc ? (int)g->ch - (int)CH_Y : (int)g->ch;
            double st0 = s + (sstv_seg_start_ms(m, j) - sync_in_line) * ms * r;
            double pd = g->ms * ms * r / Wd;
            for (int x = 0; x < Wd; x++) {
                double f = avg_frac(b, st0 + x * pd, st0 + (x + 1) * pd);
                if (isnan(f)) continue;                   /* not received yet: keep black */
                double v = (f - off - SSTV_F_BLACK) * 255.0 / (SSTV_F_WHITE - SSTV_F_BLACK);
                unsigned char u = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
                /* luma/RGB scan belongs to one row; PD chroma is shared by all rows */
                for (int rr = 0; rr < m->rows; rr++) {
                    if (ch == 0 && ycc && rr != g->row) continue;
                    if (!ycc && rr != g->row) continue;
                    pl[ch * plane + (size_t)(k * m->rows + rr) * Wd + x] = u;
                }
            }
            /* the filter smears the next tone (sync, separator) into the last pixel(s) of a scan,
             * which shows as a coloured column at the right edge: repeat the inner pixel */
            if (Wd > 3 && !isnan(avg_frac(b, st0 + (Wd - 1) * pd, st0 + Wd * pd)))
                for (int rr = 0; rr < m->rows; rr++) {
                    if ((ch == 0 || !ycc) && rr != g->row) continue;
                    unsigned char *row = pl + ch * plane + (size_t)(k * m->rows + rr) * Wd;
                    if (ycc && ch != 0) row[Wd - 1] = row[Wd - 2] = row[Wd - 3];   /* chroma: half-rate, 2 px */
                    else row[Wd - 1] = row[Wd - 2];
                }
        }
    }
    if (flags & SSTV_NR) {
        /* only rows that have been received (an unreceived row reads as all 0) */
        unsigned char *ok = calloc((size_t)m->height, 1);
        for (int k = 0; k < H; k++) {
            double s = a + bb * k + (sstv_line_ms(m) - sstv_seg_start_ms(m, si)) * ms * r;   /* line end */
            if (s <= b->n) for (int rr = 0; rr < m->rows; rr++) ok[k * m->rows + rr] = 1;
        }
        for (int ch = 0; ch < 3; ch++) despeckle(pl + ch * plane, Wd, m->height, ycc && ch ? 28 : 40, ok);
        free(ok);
    }
    image *img = image_new(Wd, m->height);
    for (size_t i = 0; i < plane; i++) {
        unsigned char *px = img->px + i * 3;
        if (ycc) sstv_ycc_to_rgb(pl[i], pl[plane + i], pl[2 * plane + i], px);
        else { px[0] = pl[i]; px[1] = pl[plane + i]; px[2] = pl[2 * plane + i]; }
    }
    free(pl);

    if (info) {
        info->syncs = used;
        info->line_ms = bb / ms;
        info->clock_ppm = (Ln / bb - 1) * 1e6;
        info->end = a - sync_in_line * ms * r + bb * H;
        info->last_sync = last_sync;
        info->offset = off;
    }
    return img;
}

/* combine.c - several receptions of the same picture -> one (see combine.h) */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "combine.h"

/* ---------------------------------------------------------------- helpers */

static int row_valid(const image *im, int y)
{
    const unsigned char *p = im->px + (size_t)y * im->w * 3;
    for (int i = 0; i < im->w * 3; i++) if (p[i] > 2) return 1;
    return 0;
}

int cmb_rows(const image *im)
{
    int n = 0;
    for (int y = 0; y < im->h; y++) n += row_valid(im, y);
    return n;
}

/* luma, box-blurred (radius r) over received rows; invalid rows marked in v[] */
static float *plane_blur(const image *im, int ch, int r, unsigned char *v);
static float *luma_blur(const image *im, int r, unsigned char *v) { return plane_blur(im, -1, r, v); }

/* one channel (ch 0..2) or the luma (ch -1), box-blurred over received rows */
static float *plane_blur(const image *im, int ch, int r, unsigned char *v)
{
    int w = im->w, h = im->h;
    float *L = malloc((size_t)w * h * sizeof(float)), *T = malloc((size_t)w * h * sizeof(float));
    for (int y = 0; y < h; y++) {
        v[y] = (unsigned char)row_valid(im, y);
        for (int x = 0; x < w; x++) {
            const unsigned char *p = im->px + ((size_t)y * w + x) * 3;
            L[(size_t)y * w + x] = ch < 0 ? 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] : p[ch];
        }
    }
    /* horizontal */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float s = 0; int c = 0;
            for (int k = -r; k <= r; k++) { int xx = x + k; if (xx >= 0 && xx < w) { s += L[(size_t)y * w + xx]; c++; } }
            T[(size_t)y * w + x] = s / c;
        }
    /* vertical, only over valid rows */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float s = 0; int c = 0;
            for (int k = -r; k <= r; k++) { int yy = y + k; if (yy >= 0 && yy < h && v[yy]) { s += T[(size_t)yy * w + x]; c++; } }
            L[(size_t)y * w + x] = c ? s / c : 0;
        }
    free(T);
    return L;
}

/* NCC of b against a with b(x,y) ~ a(x+dx, y+dy), sampled every 'st' pixels;
 * rows: overlapping valid rows */
static double ncc(const float *A, const unsigned char *va, const float *B, const unsigned char *vb,
                  int w, int h, int dx, int dy, int st, int *rows)
{
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    long n = 0;
    int nr = 0;
    for (int y = 0; y < h; y += st) {
        int ya = y + dy;
        if (ya < 0 || ya >= h || !vb[y] || !va[ya]) continue;
        nr++;
        for (int x = 0; x < w; x += st) {
            int xa = x + dx;
            if (xa < 0 || xa >= w) continue;
            double p = A[(size_t)ya * w + xa], q = B[(size_t)y * w + x];
            sa += p; sb += q; saa += p * p; sbb += q * q; sab += p * q; n++;
        }
    }
    if (rows) *rows = nr * st;
    if (n < 16) return -1;
    double ca = saa - sa * sa / n, cb = sbb - sb * sb / n, cab = sab - sa * sb / n;
    if (ca <= 1e-6 * n || cb <= 1e-6 * n) return 0;
    return cab / sqrt(ca * cb);
}

/* 4x4 box downscale of a luma plane, valid rows: all four */
static float *down4(const float *L, const unsigned char *v, int w, int h, unsigned char *v4)
{
    int w4 = w / 4, h4 = h / 4;
    float *D = calloc((size_t)w4 * h4, sizeof(float));
    for (int y = 0; y < h4; y++) {
        v4[y] = v[4 * y] && v[4 * y + 1] && v[4 * y + 2] && v[4 * y + 3];
        for (int x = 0; x < w4; x++) {
            float s = 0;
            for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) s += L[(size_t)(4 * y + j) * w + 4 * x + i];
            D[(size_t)y * w4 + x] = s / 16;
        }
    }
    return D;
}

/* ------------------------------------------------------------------ match */

int cmb_match(const image *a, const image *b, int *dxo, int *dyo, double *score)
{
    if (score) *score = 0;
    if (!a || !b || a->w != b->w || a->h != b->h || a->w < 32 || a->h < 32) return 0;
    int w = a->w, h = a->h;
    unsigned char *va = malloc(h), *vb = malloc(h);
    float *A = luma_blur(a, 2, va), *B = luma_blur(b, 2, vb);
    int ra = 0, rb = 0;
    for (int y = 0; y < h; y++) { ra += va[y]; rb += vb[y]; }
    int need = (ra < rb ? ra : rb) * 3 / 10;          /* at least 30 % of the smaller copy overlaps */
    if (need < 32) need = 32;
    int ok = 0;
    if (ra >= 32 && rb >= 32) {
        /* coarse: 4x smaller, any vertical offset up to half the picture */
        int w4 = w / 4, h4 = h / 4;
        unsigned char *va4 = malloc(h4), *vb4 = malloc(h4);
        float *A4 = down4(A, va, w, h, va4), *B4 = down4(B, vb, w, h, vb4);
        double best = -2;
        int bx = 0, by = 0;
        for (int dy = -h4 / 2; dy <= h4 / 2; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                int rows;
                double c = ncc(A4, va4, B4, vb4, w4, h4, dx, dy, 1, &rows);
                if (rows * 4 < need) continue;
                if (c > best) { best = c; bx = dx; by = dy; }
            }
        free(A4); free(B4); free(va4); free(vb4);
        if (best > 0.3) {
            /* fine: full resolution around it */
            double fb = -2;
            int fx = 4 * bx, fy = 4 * by;
            for (int dy = 4 * by - 5; dy <= 4 * by + 5; dy++)
                for (int dx = 4 * bx - 5; dx <= 4 * bx + 5; dx++) {
                    int rows;
                    double c = ncc(A, va, B, vb, w, h, dx, dy, 2, &rows);
                    if (rows < need) continue;
                    if (c > fb) { fb = c; fx = dx; fy = dy; }
                }
            /* verify block by block: the same frame with another photo in it (ISS series,
             * templates) agrees overall but not in the blocks of the photo. A block agrees
             * when its difference is no more than the noise, which is the typical (median)
             * difference of the blocks when the pictures are the same. */
            double D[256];
            int nb = 0;
            for (int y0 = 0; y0 + 32 <= h; y0 += 32)
                for (int x0 = 0; x0 + 32 <= w && nb < 256; x0 += 32) {
                    double sd = 0;
                    int n = 0;
                    for (int y = y0; y < y0 + 32; y += 2) {
                        int ya = y + fy;
                        if (ya < 0 || ya >= h || !vb[y] || !va[ya]) continue;
                        for (int x = x0; x < x0 + 32; x += 2) {
                            int xa = x + fx;
                            if (xa < 0 || xa >= w) continue;
                            double d = A[(size_t)ya * w + xa] - B[(size_t)y * w + x];
                            sd += d * d; n++;
                        }
                    }
                    if (n >= 128) D[nb++] = sqrt(sd / n);
                }
            double S[256];
            memcpy(S, D, nb * sizeof(double));
            for (int i = 1; i < nb; i++) { double t = S[i]; int k = i - 1; while (k >= 0 && S[k] > t) { S[k + 1] = S[k]; k--; } S[k + 1] = t; }
            double med = nb ? S[nb / 2] : 0, lim = 2.5 * med < 14 ? 14 : 2.5 * med;
            int good = 0;
            for (int i = 0; i < nb; i++) good += D[i] <= lim;
            double frac = nb ? (double)good / nb : 0;
            if (score) *score = fb > 0 ? fb * frac : 0;
            ok = nb >= 6 && fb >= 0.6 && frac >= 0.95 && med < 30;
            if (getenv("CMB_DEBUG")) fprintf(stderr, "match: coarse %.2f fine %.2f at %+d %+d, blocks %d/%d, median diff %.1f p90 %.1f\n",
                                             best, fb, fx, fy, good, nb, med, nb ? S[nb * 9 / 10] : 0);
            *dxo = fx; *dyo = fy;
        }
    }
    free(A); free(B); free(va); free(vb);
    return ok;
}

/* ------------------------------------------------------------------ merge */

static unsigned char med3(unsigned char a, unsigned char b, unsigned char c)
{
    if (a > b) { unsigned char t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

/* noise of every row: mean |pixel - horizontal 3-median|, smoothed over +-4 received rows */
static float *row_noise(const image *im, const unsigned char *v)
{
    int w = im->w, h = im->h;
    float *n0 = calloc(h, sizeof(float)), *n = calloc(h, sizeof(float));
    for (int y = 0; y < h; y++) {
        if (!v[y]) continue;
        const unsigned char *p = im->px + (size_t)y * w * 3;
        double s = 0;
        for (int x = 1; x < w - 1; x++)
            for (int c = 0; c < 3; c++) {
                int i = x * 3 + c;
                s += abs((int)p[i] - (int)med3(p[i - 3], p[i], p[i + 3]));
            }
        n0[y] = (float)(s / ((w - 2) * 3));
    }
    for (int y = 0; y < h; y++) {
        if (!v[y]) continue;
        double s = 0; int c = 0;
        for (int k = -4; k <= 4; k++) { int yy = y + k; if (yy >= 0 && yy < h && v[yy]) { s += n0[yy]; c++; } }
        n[y] = (float)(s / c);
    }
    free(n0);
    return n;
}

/* 3x3 median of channel c at (x, y), received rows only */
static int med9(const image *im, const unsigned char *v, int x, int y, int c)
{
    int a[9], n = 0;
    for (int j = -1; j <= 1; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= im->h || !v[yy]) continue;
        for (int i = -1; i <= 1; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= im->w) continue;
            a[n++] = im->px[((size_t)yy * im->w + xx) * 3 + c];
        }
    }
    for (int i = 1; i < n; i++) { int t = a[i], k = i - 1; while (k >= 0 && a[k] > t) { a[k + 1] = a[k]; k--; } a[k + 1] = t; }
    return n ? a[n / 2] : 0;
}

/* one weighted merge of the aligned copies into out */
static void merge_pass(image *const *ims, unsigned char *const *v, float *const *Wr, float *const *rn,
                       const int *dx, const int *dy, int n, image *out)
{
    int w = out->w, h = out->h;
    double *val = malloc(n * 3 * sizeof(double)), *wt = malloc(n * 3 * sizeof(double));
    for (int Y = 0; Y < h; Y++)
        for (int X = 0; X < w; X++) {
            int m = 0;
            for (int i = 0; i < n; i++) {
                int x = X - dx[i], y = Y - dy[i];
                if (x < 0 || x >= w || y < 0 || y >= h || !v[i][y]) continue;
                double nz = rn[i][y], W = Wr[i][y], k = 3 * nz + 12;
                const unsigned char *p = ims[i]->px + ((size_t)y * w + x) * 3;
                double d = 0;                       /* speck: far from its own neighbourhood */
                for (int c = 0; c < 3; c++) { double e = abs((int)p[c] - med9(ims[i], v[i], x, y, c)); if (e > d) d = e; }
                double g = exp(-(d / k) * (d / k));
                for (int c = 0; c < 3; c++) { val[m * 3 + c] = p[c]; wt[m * 3 + c] = W * (0.05 + g); }
                m++;
            }
            unsigned char *o = out->px + ((size_t)Y * w + X) * 3;
            if (!m) { o[0] = o[1] = o[2] = 0; continue; }
            for (int c = 0; c < 3; c++) {
                double sum = 0, sw = 0;
                for (int i = 0; i < m; i++) { sum += val[i * 3 + c] * wt[i * 3 + c]; sw += wt[i * 3 + c]; }
                double r = sw > 1e-15 ? sum / sw : val[c];
                o[c] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r + 0.5);
            }
        }
    free(val); free(wt);
}

image *cmb_merge(image *const *ims, const int *dx, const int *dy, int n)
{
    return cmb_merge_q(ims, dx, dy, NULL, n);
}

image *cmb_merge_q(image *const *ims, const int *dx, const int *dy, const float *const *rowq, int n)
{
    if (n < 1) return NULL;
    int w = ims[0]->w, h = ims[0]->h;
    unsigned char **v = malloc(n * sizeof *v);
    float **rn = malloc(n * sizeof *rn), **Wr = malloc(n * sizeof *Wr);
    for (int i = 0; i < n; i++) {
        v[i] = malloc(h);
        for (int y = 0; y < h; y++) v[i][y] = (unsigned char)row_valid(ims[i], y);
        rn[i] = row_noise(ims[i], v[i]);
        Wr[i] = malloc(h * sizeof(float));
        /* row weight: the signal/noise of its line if the receiver measured it, else
           from how noisy the row looks */
        for (int y = 0; y < h; y++)
            Wr[i][y] = (float)((rowq && rowq[i] ? sqrt(rowq[i][y]) : 1.0) / (rn[i][y] * rn[i][y] + 4));
    }
    image *out = image_new(w, h);
    merge_pass(ims, v, Wr, rn, dx, dy, n, out);
    if (n >= 3) {
        /* second pass: a row that disagrees with the others (a fade, a level shift, QRM)
           counts less, even if it looked clean */
        unsigned char *vo = malloc(h), *vi = malloc(h);
        float *O = plane_blur(out, -1, 2, vo);
        for (int i = 0; i < n; i++) {
            float *B = plane_blur(ims[i], -1, 2, vi);
            for (int y = 0; y < h; y++) {
                int Y = y + dy[i];
                if (!v[i][y] || Y < 0 || Y >= h) continue;
                double s = 0; int c = 0;
                for (int x = 0; x < w; x += 2) {
                    int X = x + dx[i];
                    if (X < 0 || X >= w) continue;
                    s += fabs(B[(size_t)y * w + x] - O[(size_t)Y * w + X]); c++;
                }
                double dev = c ? s / c : 0;
                Wr[i][y] = (float)(Wr[i][y] / (1 + (dev / 10) * (dev / 10)));
            }
            free(B);
        }
        free(O); free(vo); free(vi);
        merge_pass(ims, v, Wr, rn, dx, dy, n, out);
    }
    for (int i = 0; i < n; i++) { free(v[i]); free(rn[i]); free(Wr[i]); }
    free(v); free(rn); free(Wr);
    return out;
}

/* ------------------------------------------------- per-line quality text */

static const char QALPHA[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

void cmb_q_encode(const unsigned char *lq, int n, char *out, size_t size)
{
    size_t o = 0;
    for (int k = 0; k < n && o + 1 < size; k++) {
        int q = lq[k] ? (lq[k] + 1) / 2 : 0;            /* 2 dB steps, 0 unknown */
        if (q > 61) q = 61;
        out[o++] = QALPHA[q];
    }
    if (size) out[o] = 0;
}

int cmb_q_rows(const char *q, int rows, int rows_per_line, float *w)
{
    int n = (int)strlen(q), got = 0;
    if (rows_per_line < 1) rows_per_line = n > 0 && rows >= 2 * n ? rows / n : 1;   /* PD: 2 rows a line */
    for (int y = 0; y < rows; y++) {
        int k = y / rows_per_line;
        const char *p = k < n ? strchr(QALPHA, q[k]) : NULL;
        int v = p && q[k] ? (int)(p - QALPHA) : 0;
        if (v) {
            double snr = 2.0 * v - 20;                   /* dB */
            if (snr > 30) snr = 30;
            /* noise power in a pixel goes ~ 1/SNR above the FM threshold and faster below */
            double lin = pow(10, snr / 10);
            w[y] = (float)(snr < 8 ? pow(lin, 1.5) * pow(10, -0.4) : lin);
            got++;
        } else w[y] = -1;
    }
    /* unknown lines: the mean of the known ones */
    double s = 0; int c = 0;
    for (int y = 0; y < rows; y++) if (w[y] >= 0) { s += w[y]; c++; }
    for (int y = 0; y < rows; y++) if (w[y] < 0) w[y] = c ? (float)(s / c) : 1;
    return got;
}

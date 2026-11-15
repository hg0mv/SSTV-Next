/* drm_tx.c - HamDRM OFDM transmitter
 *
 * MSC: energy dispersal -> multilevel convolutional coding -> bit
 * interleaving -> QAM mapping -> cell interleaving -> cell stream.
 * FAC: 48 bits + CRC8, rate 0.6 code, 4-QAM, fixed cell positions.
 * OFDM: pilots + cells -> IFFT -> guard interval -> shift to 350 Hz.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "drm_tx.h"

static const double complex dummy16[2] = {0.3162277660 + 0.3162277660 * I, 0.3162277660 - 0.3162277660 * I};
static const double complex dummy64[2] = {0.1543033499 + 0.1543033499 * I, 0.1543033499 - 0.1543033499 * I};

int drm_tx_init(drm_tx *t, const drm_params *p, drm_src_fn src, void *user)
{
	memset(t, 0, sizeof *t);
	if (drm_setup(&t->c, p)) return -1;
	drm_cfg *c = &t->c;
	t->src = src;
	t->user = user;
	t->cap = 3 * c->nmux + 64;
	t->cells = malloc(sizeof(double complex) * t->cap);
	t->ilmem = calloc(5 * (size_t)c->nmux, sizeof(double complex));
	for (int i = 0; i < 5; i++) t->ilcur[i] = i;
	t->celltab = malloc(sizeof(int) * c->nmux);
	drm_block_table(t->celltab, c->nmux, 5);
	for (int l = 0; l < c->levels; l++)
		if (c->ilv[l]) {
			t->bittab[l] = malloc(sizeof(int) * 2 * c->nmux);
			drm_block_table(t->bittab[l], 2 * c->nmux, c->ilv[l]);
		}
	t->rot = malloc(sizeof(double complex) * c->nfft);
	for (int n = 0; n < c->nfft; n++) t->rot[n] = cexp(I * 2.0 * M_PI * n / c->nfft);
	double w = -2.0 * M_PI * (DRM_CAR_OFFSET - DRM_IF) / DRM_FS;
	t->shift = 1.0;
	t->shift_step = cos(w) + I * sin(w);
	return 0;
}

void drm_tx_free(drm_tx *t)
{
	free(t->cells); free(t->ilmem); free(t->celltab); free(t->rot);
	for (int l = 0; l < 3; l++) free(t->bittab[l]);
	memset(t, 0, sizeof *t);
}

int drm_tx_frame_len(const drm_tx *t) { return t->c.symlen * t->c.nsym; }

/* encode one MSC logical frame and append its interleaved cells to the FIFO */
static void msc_frame(drm_tx *t)
{
	drm_cfg *c = &t->c;
	int n2 = 2 * c->nmux;
	uint8_t *in = calloc(c->nbits, 1), *enc[3] = {0}, *tmp = malloc(n2);
	t->src(t->user, in);
	drm_energy_dispersal(in, c->nbits);
	int pos = 0;
	for (int l = 0; l < c->levels; l++) {
		enc[l] = malloc(n2 + 8);
		int no = drm_conv_encode(in + pos, c->m[l], c->rate[l], c->tailpat[l], 0, enc[l]);
		(void)no;
		pos += c->m[l];
		if (t->bittab[l]) {
			for (int i = 0; i < n2; i++) tmp[i] = enc[l][t->bittab[l][i]];
			memcpy(enc[l], tmp, n2);
		}
	}
	const double *tab; int npt;
	drm_qam_points(c->p.qam, &tab, &npt);
	double complex *q = malloc(sizeof(double complex) * c->nmux);
	for (int i = 0; i < c->nmux; i++) {
		int re = 0, im = 0;
		for (int l = 0; l < c->levels; l++) {
			re = re << 1 | enc[l][2 * i];
			im = im << 1 | enc[l][2 * i + 1];
		}
		q[i] = tab[re] + I * tab[im];
	}
	/* cell interleaver (depth 5 frames for long, 1 for short) */
	int D = c->p.ilong ? 5 : 1;
	memcpy(t->ilmem + (size_t)t->ilcur[0] * c->nmux, q, sizeof(double complex) * c->nmux);
	for (int i = 0; i < c->nmux; i++)
		t->cells[t->ncells++] = t->ilmem[(size_t)t->ilcur[i % D] * c->nmux + t->celltab[i]];
	for (int j = 0; j < D; j++)
		if (--t->ilcur[j] < 0) t->ilcur[j] = D - 1;
	for (int l = 0; l < 3; l++) free(enc[l]);
	free(q); free(tmp); free(in);
}

static void fac_cells(drm_tx *t, double complex *out)
{
	uint8_t bits[DRM_FAC_BITS], enc[2 * DRM_FAC_CELLS + 8], il[2 * DRM_FAC_CELLS];
	int tab[2 * DRM_FAC_CELLS];
	drm_fac_bits(&t->c, t->frame, bits);
	drm_energy_dispersal(bits, DRM_FAC_BITS);
	drm_conv_encode(bits, DRM_FAC_BITS, 6, 0, 1, enc);
	drm_block_table(tab, 2 * DRM_FAC_CELLS, 21);
	for (int i = 0; i < 2 * DRM_FAC_CELLS; i++) il[i] = enc[tab[i]];
	for (int i = 0; i < DRM_FAC_CELLS; i++)
		out[i] = (il[2 * i] ? -0.7071067811 : 0.7071067811) + I * (il[2 * i + 1] ? -0.7071067811 : 0.7071067811);
}

void drm_tx_frame(drm_tx *t, double *out)
{
	drm_cfg *c = &t->c;
	double complex fac[DRM_FAC_CELLS], car[DRM_MAX_CAR], x[2048];
	const double complex *dummy = c->p.qam == DRM_QAM64 ? dummy64 : dummy16;
	fac_cells(t, fac);
	int nf = 0;
	for (int s = 0; s < c->nsym; s++) {
		int sa = t->frame * c->nsym + s;
		while (t->ncells < c->nmsc_sym[sa]) msc_frame(t);
		int nm = 0, nd = 0;
		for (int ci = 0; ci < c->ncar; ci++) {
			uint8_t ty = c->type[sa][ci];
			if (ty == CT_MSC)
				car[ci] = nm < c->nmsc_sym[sa] ? t->cells[nm++] : dummy[nd++ & 1];
			else if (ty == CT_FAC) car[ci] = fac[nf++];
			else if (ty & CT_PILOT) car[ci] = c->pilot[sa][ci];
			else car[ci] = 0;
		}
		memmove(t->cells, t->cells + nm, sizeof(double complex) * (t->ncells - nm));
		t->ncells -= nm;
		/* inverse DFT of the few used bins */
		for (int n = 0; n < c->nfft; n++) {
			double complex acc = 0;
			for (int ci = 0; ci < c->ncar; ci++) {
				if (car[ci] == 0) continue;
				int bin = c->idc + c->kmin + ci;
				acc += car[ci] * t->rot[(int)(((long)bin * n) % c->nfft)];
			}
			x[n] = acc;
		}
		double *o = out + s * c->symlen;
		for (int n = 0; n < c->symlen; n++) {
			double complex v = n < c->nguard ? x[c->nfft - c->nguard + n] : x[n - c->nguard];
			o[n] = creal(v * conj(t->shift));
			t->shift *= t->shift_step;
		}
		t->nsamp += c->symlen;
	}
	if (++t->frame == DRM_FRAMES_SF) t->frame = 0;
}

/* ---- complete transmission -------------------------------------------------- */
#include "mot.h"

static void mot_src(void *u, uint8_t *bits) { mot_tx_frame((mot_tx *)u, bits); }

/* windowed sinc resampler (offline) */
static float *resample(const double *x, long n, double fin, double fout, long *nout)
{
	double step = fin / fout, fc = 0.45 * (fin < fout ? fin : fout) / fin;   /* cutoff, cycles per input sample */
	int hw = (int)ceil(8.0 / (2 * fc));
	long no = (long)(n / step);
	float *y = malloc(sizeof(float) * (no + 1));
	for (long i = 0; i < no; i++) {
		double t = i * step, acc = 0;
		long c = (long)floor(t);
		for (long k = c - hw + 1; k <= c + hw; k++) {
			if (k < 0 || k >= n) continue;
			double u = t - k, a = 2 * fc * u;
			double s = fabs(a) < 1e-9 ? 1 : sin(M_PI * a) / (M_PI * a);
			double w = 0.5 + 0.5 * cos(M_PI * u / hw);
			acc += x[k] * s * w;
		}
		y[i] = (float)(acc * 2 * fc);
	}
	*nout = no;
	return y;
}

float *drm_tx_render(const drm_params *p, const uint8_t *data, int len, const char *name,
                     const int *fix, int nfix, double out_fs, long *nout, double *seconds)
{
	drm_tx t;
	mot_tx m;
	if (drm_tx_init(&t, p, mot_src, &m)) return NULL;
	int frames = mot_tx_init(&m, data, len, name, t.c.nbits, fix, nfix);
	int fl = drm_tx_frame_len(&t);
	long n = (long)fl * frames;
	double *x = malloc(sizeof(double) * n);
	for (int f = 0; f < frames; f++) drm_tx_frame(&t, x + (long)f * fl);
	drm_tx_free(&t);
	mot_tx_free(&m);
	/* level: RMS about -15 dBFS, the rare OFDM peaks limited softly */
	double e = 0;
	for (long i = 0; i < n; i++) e += x[i] * x[i];
	double g = 0.18 / sqrt(e / n + 1e-20);
	for (long i = 0; i < n; i++) {
		double v = x[i] * g;
		x[i] = v > 0.8 ? 0.8 + 0.15 * tanh((v - 0.8) / 0.15) : v < -0.8 ? -0.8 - 0.15 * tanh((-v - 0.8) / 0.15) : v;
	}
	float *y;
	if (fabs(out_fs - DRM_FS) < 0.5) {
		y = malloc(sizeof(float) * n);
		for (long i = 0; i < n; i++) y[i] = (float)x[i];
		*nout = n;
	} else y = resample(x, n, DRM_FS, out_fs, nout);
	free(x);
	if (seconds) *seconds = (double)n / DRM_FS;
	return y;
}

double drm_tx_duration(const drm_params *p, int len)
{
	drm_cfg c;
	if (drm_setup(&c, p)) return 0;
	int seg = c.nbytes - 14, nseg = (len + seg - 1) / seg;
	int frames = MOT_RUN_IN + nseg + (nseg + 49) / 50 * 2 + MOT_RUN_OUT;   /* + header repeats */
	return frames * drm_frame_seconds(&c);
}

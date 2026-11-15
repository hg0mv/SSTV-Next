/* drm_rx.c - HamDRM receiver
 *
 *  audio -> resample to 12 kHz -> complex baseband (carrier 0 at 0 Hz)
 *  search: guard interval correlation (mode, timing, fractional frequency),
 *          frequency pilots (integer frequency), pilot pattern (frame start)
 *  sync:   per symbol DFT, pilot based channel estimate (time + frequency
 *          interpolation), frequency and timing tracking, FAC decoding,
 *          MSC cell deinterleaving, multistage soft demapping + Viterbi,
 *          packets -> MOT objects.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "drm_rx.h"

int drm_rx_debug;

#define FS        DRM_RX_FS
#define ACQ_LEN   (FS * 8 / 5)          /* 1.6 s analysed per search */
#define ACQ_STEP  (FS * 2 / 5)
#define MIX1      1600.0                /* band centre (350 .. 2850 Hz) */
#define MIX2      1250.0                /* back to carrier 0 at 0 Hz */
#define MAXOFF    150.0                 /* frequency search range (Hz) */
#define RS_HW     6                     /* resampler half width (zero crossings) */
#define RS_FC     4000.0
#define RS_TAB    512

static float rs_kernel[RS_HW * RS_TAB + 2];

static double sinc(double x) { return fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x); }

static void cfg_scaled(drm_rx *r)
{
	r->nfft = r->c.nfft / 4;
	r->nguard = r->c.nguard / 4;
	r->symlen = r->nfft + r->nguard;
	free(r->dft);
	r->dft = malloc(sizeof(double complex) * r->nfft);
	for (int n = 0; n < r->nfft; n++) r->dft[n] = cexp(-I * 2.0 * M_PI * n / r->nfft);
}

static void msc_free(drm_rx *r)
{
	free(r->fr_y); free(r->fr_w); free(r->sf_y); free(r->sf_w); free(r->celltab);
	r->fr_y = NULL; r->fr_w = NULL; r->sf_y = NULL; r->sf_w = NULL; r->celltab = NULL;
	for (int i = 0; i < 5; i++) { free(r->dl_y[i]); free(r->dl_w[i]); r->dl_y[i] = NULL; r->dl_w[i] = NULL; r->dl_id[i] = -100; }
	for (int l = 0; l < 3; l++) { free(r->bittab[l]); r->bittab[l] = NULL; }
}

/* allocate everything that depends on the MSC parameters */
static void msc_setup(drm_rx *r)
{
	msc_free(r);
	drm_cfg *c = &r->c;
	int nfr = 0;
	for (int s = 0; s < c->nsym; s++) nfr += c->nmsc_sym[s];
	r->fr_y = calloc(nfr + 8, sizeof(double complex));
	r->fr_w = calloc(nfr + 8, sizeof(float));
	r->sf_y = calloc(3 * (size_t)nfr + 8, sizeof(double complex));
	r->sf_w = calloc(3 * (size_t)nfr + 8, sizeof(float));
	r->celltab = malloc(sizeof(int) * c->nmux);
	drm_block_table(r->celltab, c->nmux, 5);
	for (int i = 0; i < 5; i++) {
		r->dl_y[i] = calloc(c->nmux, sizeof(double complex));
		r->dl_w[i] = calloc(c->nmux, sizeof(float));
		r->dl_id[i] = -100;
	}
	for (int l = 0; l < c->levels; l++)
		if (c->ilv[l]) {
			r->bittab[l] = malloc(sizeof(int) * 2 * c->nmux);
			drm_block_table(r->bittab[l], 2 * c->nmux, c->ilv[l]);
		}
	r->sf_pos = 0;
	r->sf_done = 0;
}

int drm_rx_init(drm_rx *r, int infs, mot_done_fn done, void *user)
{
	memset(r, 0, sizeof *r);
	r->infs = infs;
	if (!rs_kernel[0]) {
		for (int i = 0; i <= RS_HW * RS_TAB + 1; i++) {
			double t = (double)i / RS_TAB;           /* in units of output zero crossings */
			double win = t < RS_HW ? 0.5 + 0.5 * cos(M_PI * t / RS_HW) : 0;
			rs_kernel[i] = (float)(sinc(t) * win);
		}
	}
	r->rs_step = (double)infs / FS;
	r->rs_nh = 4096;
	r->rs_hist = calloc(r->rs_nh, sizeof(float));
	/* band filter: low pass +-1650 Hz at 12 kHz */
	r->nfir = 97;
	r->fir = malloc(sizeof(double) * r->nfir);
	for (int i = 0; i < r->nfir; i++) {
		double t = i - (r->nfir - 1) / 2.0;
		double w = 0.42 - 0.5 * cos(2 * M_PI * i / (r->nfir - 1)) + 0.08 * cos(4 * M_PI * i / (r->nfir - 1));
		r->fir[i] = 2.0 * 1650.0 / FS * sinc(2.0 * 1650.0 / FS * t) * w;
	}
	r->firbuf = calloc(r->nfir, sizeof(double complex));
	r->w1 = 2 * M_PI * MIX1 / FS;
	r->w2 = 2 * M_PI * MIX2 / FS;
	r->zcap = FS * 6;
	r->z = malloc(sizeof(double complex) * r->zcap);
	mot_rx_init(&r->mot, done, user);
	drm_rx_reset(r);
	return 0;
}

void drm_rx_free(drm_rx *r)
{
	msc_free(r);
	free(r->rs_hist); free(r->fir); free(r->firbuf); free(r->z); free(r->dft);
	mot_rx_free(&r->mot);
	memset(r, 0, sizeof *r);
}

void drm_rx_reset(drm_rx *r)
{
	r->state = DRM_RX_SEARCH;
	r->params_ok = 0;
	r->acq_next = r->zbase + r->zlen;
	r->frame_id = -1;
	r->bad_frames = 0;
}

/* ---- search ------------------------------------------------------------- */

static double complex zat(drm_rx *r, long n) { return r->z[n - r->zbase]; }

/* demodulate bins kfrom..kto of a window starting at absolute index w */
static void demod(drm_rx *r, long w, double foff, double ph0, int kfrom, int kto, double complex *out)
{
	int N = r->nfft;
	double complex u[512];
	double dph = -2 * M_PI * foff / FS;
	double complex rot = cexp(I * ph0), st = cexp(I * dph);
	for (int n = 0; n < N; n++) { u[n] = zat(r, w + n) * rot; rot *= st; }
	for (int k = kfrom; k <= kto; k++) {
		double complex acc = 0;
		int kk = ((k % N) + N) % N, idx = 0;
		for (int n = 0; n < N; n++) { acc += u[n] * r->dft[idx]; idx += kk; if (idx >= N) idx -= N; }
		out[k - kfrom] = acc / N;
	}
}


static int acquire(drm_rx *r, long a0)
{
	int len = ACQ_LEN;
	double best = 0; int bm = -1; long bd = 0; double complex bg = 0;
	double complex *cc = malloc(sizeof(double complex) * (len + 1));
	double *ee = malloc(sizeof(double) * (len + 1));
	for (int m = 0; m < 3; m++) {
		drm_params p = {m, 1, DRM_QAM4, 0, 1, ""};
		drm_cfg c;
		drm_setup(&c, &p);
		int N = c.nfft / 4, G = c.nguard / 4, L = N + G;
		int nn = len - N;
		cc[0] = 0; ee[0] = 0;
		for (int n = 0; n < nn; n++) {
			double complex x0 = zat(r, a0 + n), x1 = zat(r, a0 + n + N);
			cc[n + 1] = cc[n] + x0 * conj(x1);
			ee[n + 1] = ee[n] + 0.5 * (creal(x0 * conj(x0)) + creal(x1 * conj(x1)));
		}
		for (int d = 0; d < L; d++) {
			double complex g = 0; double e = 0;
			for (int s = d; s + G < nn; s += L) { g += cc[s + G] - cc[s]; e += ee[s + G] - ee[s]; }
			double rho = e > 0 ? cabs(g) / e : 0;
			if (rho > best) { best = rho; bm = m; bd = d; bg = g; }
		}
	}
	free(cc); free(ee);
	if (bm < 0 || best < 0.2) return 0;

	drm_params p = {bm, 1, DRM_QAM4, 0, 1, ""};
	drm_setup(&r->c, &p);
	cfg_scaled(r);
	drm_cfg *c = &r->c;
	int N = r->nfft, G = r->nguard, L = r->symlen;
	double df = (double)FS / N;
	double eps = -carg(bg) * FS / (2 * M_PI * N);
	int B = (int)ceil(MAXOFF / df) + 1;
	int back = G / 4;
	long w0 = a0 + bd + G - back;
	int S = (int)((len - (w0 - a0) - N) / L);
	if (S > 3 * c->nsym) S = 3 * c->nsym;
	if (S < c->nsym + 5) return 0;
	int kf = c->kmin - B, kt = c->kmax + B, nb = kt - kf + 1;
	double complex (*Y)[DRM_MAX_CAR + 32] = malloc(sizeof *Y * S);
	for (int s = 0; s < S; s++)
		demod(r, w0 + (long)s * L, eps, -2 * M_PI * eps * s * L / FS, kf, kt, Y[s]);
	(void)nb;
	/* integer frequency offset: continuity of the frequency pilots */
	static const int fp[3][3] = {{9, 27, 36}, {8, 24, 32}, {5, 15, 20}};
	double bestb = -1; int bb = 0;
	for (int b = -B; b <= B; b++) {
		double m = 0, nrm = 0;
		for (int i = 0; i < 3; i++) {
			double complex acc = 0;
			for (int s = 1; s < S; s++) {
				double complex v = Y[s][fp[bm][i] + b - kf] * conj(Y[s - 1][fp[bm][i] + b - kf]);
				acc += v; nrm += cabs(v);
			}
			m += cabs(acc);
		}
		m = nrm > 0 ? m / nrm : 0;
		if (m > bestb) { bestb = m; bb = b; }
	}
	if (bestb < 0.3) { free(Y); return 0; }
	/* demodulate again with the full correction (the guard interval makes a
	   whole-bin offset rotate from symbol to symbol) */
	double ftot = eps + bb * df;
	for (int s = 0; s < S; s++)
		demod(r, w0 + (long)s * L, ftot, -2 * M_PI * ftot * s * L / FS, kf, kt, Y[s]);
	bb = 0;
	/* spectrum occupancy: energy above the 2.3 kHz edge */
	drm_params p0 = {bm, 0, DRM_QAM4, 0, 1, ""};
	drm_cfg c0;
	drm_setup(&c0, &p0);
	double ein = 0, eout = 0; int nin = 0, nout = 0;
	for (int s = 0; s < S; s++)
		for (int k = c->kmin; k <= c->kmax; k++) {
			double e = creal(Y[s][k + bb - kf] * conj(Y[s][k + bb - kf]));
			if (k <= c0.kmax) ein += e, nin++; else eout += e, nout++;
		}
	int so = (nout && eout / nout > 0.25 * ein / nin) ? 1 : 0;
	p.so = so;
	drm_setup(&r->c, &p);
	c = &r->c;
	/* frame start: pilots on the same carrier tint symbols apart */
	static const int tint[3] = {5, 3, 4};
	int T = tint[bm];
	double sc[DRM_MAX_SYM] = {0}, scmax = -1, sc2 = -1; int bo = 0;
	for (int o = 0; o < c->nsym; o++) {
		double sum = 0, nrm = 0;
		for (int s = 0; s + T < S; s++) {
			int f1 = (s + o) % c->nsym, f2 = (s + T + o) % c->nsym;
			for (int k = c->kmin; k <= c->kmax; k++) {
				int ci = k - c->kmin;
				if (!(c->type[f1][ci] & CT_SCAT) || !(c->type[f2][ci] & CT_SCAT)) continue;
				double complex a = Y[s][k + bb - kf] / c->pilot[f1][ci];
				double complex d = Y[s + T][k + bb - kf] / c->pilot[f2][ci];
				sum += creal(a * conj(d));
				nrm += cabs(a) * cabs(d);
			}
		}
		sc[o] = nrm > 0 ? sum / nrm : 0;
	}
	for (int o = 0; o < c->nsym; o++) {
		if (sc[o] > scmax) { sc2 = scmax; scmax = sc[o]; bo = o; }
		else if (sc[o] > sc2) sc2 = sc[o];
	}
	/* start tracking at the first symbol that carries signal */
	double emax = 0, es[3 * DRM_MAX_SYM + 2];
	for (int s = 0; s < S; s++) {
		es[s] = 0;
		for (int k = c->kmin; k <= c->kmax; k++) es[s] += creal(Y[s][k + bb - kf] * conj(Y[s][k + bb - kf]));
		if (es[s] > emax) emax = es[s];
	}
	int sfirst = 0;
	while (sfirst < S - 1 && es[sfirst] < 0.2 * emax) sfirst++;
	free(Y);
	if (scmax < 0.45 || scmax < sc2 + 0.3) return 0;     /* analog SSTV/voice give < 0.2 margin */
	w0 += (long)sfirst * L;
	bo = (bo + sfirst) % c->nsym;

	if (drm_rx_debug) fprintf(stderr, "acq mode %d rho %.2f fp %.2f d %ld w0%%L %ld eps %.2f o %d sc %.2f/%.2f\n", bm, best, bestb, (a0 + bd) % L, w0 % L, eps, bo, scmax, sc2);
	r->foff = ftot;
	r->wpos = w0;
	r->fs_next = bo % c->nsym;
	r->phase_ref = 0;
	r->n_ref = w0;
	r->nsym_rx = 0;
	r->tacc = 0;
	r->tslope0 = back;
	r->tinit = 0;
	r->fr_full = 0;
	r->nfac = 0;
	r->nfr = 0;
	r->frame_id = -1;
	r->bad_frames = 0;
	r->fac_since_sync = 0;
	r->params_ok = 0;
	r->mode = bm;
	r->so = so;
	r->state = DRM_RX_SYNC;
	msc_setup(r);
	return 1;
}

/* ---- MSC ------------------------------------------------------------------ */

static void llr_axis(int qam, int levels, int l, double v, float w, const uint8_t *known, float *out)
{
	const double *tab; int npt;
	drm_qam_points(qam, &tab, &npt);
	double d0 = 1e30, d1 = 1e30;
	int shift = levels - 1 - l;
	for (int idx = 0; idx < npt; idx++) {
		int ok = 1;
		for (int j = 0; j < l; j++)
			if (((idx >> (levels - 1 - j)) & 1) != known[j]) { ok = 0; break; }
		if (!ok) continue;
		double d = (v - tab[idx]) * (v - tab[idx]);
		if ((idx >> shift) & 1) { if (d < d1) d1 = d; } else if (d < d0) d0 = d;
	}
	*out = (float)(w * (d1 - d0));
}

static void msc_decode(drm_rx *r, const double complex *q, const float *w)
{
	drm_cfg *c = &r->c;
	int n2 = 2 * c->nmux;
	float *llr = malloc(sizeof(float) * n2), *dl = malloc(sizeof(float) * n2);
	uint8_t *dec[3] = {0}, *info = calloc(c->nbits + 8, 1), *tmp = malloc(n2 + 16);
	int pos = 0;
	for (int l = 0; l < c->levels; l++) {
		for (int i = 0; i < c->nmux; i++) {
			uint8_t kr[3], ki[3];
			for (int j = 0; j < l; j++) { kr[j] = dec[j][2 * i]; ki[j] = dec[j][2 * i + 1]; }
			llr_axis(c->p.qam, c->levels, l, creal(q[i]), w[i], kr, &llr[2 * i]);
			llr_axis(c->p.qam, c->levels, l, cimag(q[i]), w[i], ki, &llr[2 * i + 1]);
		}
		if (r->bittab[l]) for (int i = 0; i < n2; i++) dl[r->bittab[l][i]] = llr[i];
		else memcpy(dl, llr, sizeof(float) * n2);
		drm_viterbi(dl, c->m[l], c->rate[l], c->tailpat[l], 0, info + pos);
		if (l + 1 < c->levels) {
			dec[l] = malloc(n2 + 16);
			drm_conv_encode(info + pos, c->m[l], c->rate[l], c->tailpat[l], 0, tmp);
			if (r->bittab[l]) for (int i = 0; i < n2; i++) dec[l][i] = tmp[r->bittab[l][i]];
			else memcpy(dec[l], tmp, n2);
		}
		pos += c->m[l];
	}
	drm_energy_dispersal(info, c->nbits);
	uint8_t *bytes = calloc(c->nbytes + 1, 1);
	for (int i = 0; i < c->nbytes * 8; i++) bytes[i >> 3] |= (uint8_t)(info[i] << (7 - (i & 7)));
	long before = r->mot.pk_ok;
	mot_rx_packet(&r->mot, bytes, c->nbytes);
	if (r->mot.pk_ok > before) r->msc_ok++; else r->msc_bad++;
	for (int l = 0; l < 3; l++) free(dec[l]);
	free(bytes); free(info); free(tmp); free(llr); free(dl);
}

/* one logical frame of received (interleaved) cells */
static void logical_frame(drm_rx *r, long g, const double complex *y, const float *w)
{
	drm_cfg *c = &r->c;
	int slot = (int)(((g % 5) + 5) % 5);
	memcpy(r->dl_y[slot], y, sizeof(double complex) * c->nmux);
	memcpy(r->dl_w[slot], w, sizeof(float) * c->nmux);
	r->dl_id[slot] = g;
	double complex *q = malloc(sizeof(double complex) * c->nmux);
	float *qw = malloc(sizeof(float) * c->nmux);
	if (c->p.ilong) {
		long h = g - 4;
		for (int i = 0; i < c->nmux; i++) {
			long src = h + i % 5;
			int sl = (int)(((src % 5) + 5) % 5);
			if (r->dl_id[sl] == src) { q[r->celltab[i]] = r->dl_y[sl][i]; qw[r->celltab[i]] = r->dl_w[sl][i]; }
			else { q[r->celltab[i]] = 0; qw[r->celltab[i]] = 0; }
		}
	} else
		for (int i = 0; i < c->nmux; i++) { q[r->celltab[i]] = y[i]; qw[r->celltab[i]] = w[i]; }
	int have = 0;
	for (int i = 0; i < c->nmux; i++) if (qw[i] > 0) have++;
	if (have > c->nmux / 2) msc_decode(r, q, qw);
	free(q); free(qw);
}

static void frame_msc(drm_rx *r, int id)
{
	drm_cfg *c = &r->c;
	int nfull = 0;
	for (int s = 0; s < c->nsym; s++) nfull += c->nmsc_sym[s];
	int nfr = r->nfr;
	if (nfr != nfull) return;
	if (id == 2) nfr -= c->ndummy;
	int start = id * nfull;
	if (r->sf_pos != start) {
		if (id == 0 || r->sf_pos > start) { r->sf_pos = 0; r->sf_done = 0; }
		while (r->sf_pos < start) { r->sf_y[r->sf_pos] = 0; r->sf_w[r->sf_pos] = 0; r->sf_pos++; }
	}
	memcpy(r->sf_y + r->sf_pos, r->fr_y, sizeof(double complex) * nfr);
	memcpy(r->sf_w + r->sf_pos, r->fr_w, sizeof(float) * nfr);
	r->sf_pos += nfr;
	long sfstart = r->frames_abs - id;
	while (r->sf_done < 3 && r->sf_pos >= (r->sf_done + 1) * c->nmux) {
		logical_frame(r, sfstart + r->sf_done, r->sf_y + (size_t)r->sf_done * c->nmux,
		              r->sf_w + (size_t)r->sf_done * c->nmux);
		r->sf_done++;
	}
}

/* ---- FAC ------------------------------------------------------------------ */

static int fac_decode(drm_rx *r, int *id)
{
	float llr[2 * DRM_FAC_CELLS], dl[2 * DRM_FAC_CELLS];
	int tab[2 * DRM_FAC_CELLS];
	uint8_t bits[DRM_FAC_BITS];
	for (int i = 0; i < DRM_FAC_CELLS; i++) {
		llr[2 * i] = (float)(r->fac_w[i] * creal(r->fac_y[i]));
		llr[2 * i + 1] = (float)(r->fac_w[i] * cimag(r->fac_y[i]));
	}
	drm_block_table(tab, 2 * DRM_FAC_CELLS, 21);
	for (int i = 0; i < 2 * DRM_FAC_CELLS; i++) dl[tab[i]] = llr[i];
	drm_viterbi(dl, DRM_FAC_BITS, 6, 0, 1, bits);
	drm_energy_dispersal(bits, DRM_FAC_BITS);
	uint8_t by[6] = {0};
	for (int i = 0; i < 48; i++) by[i >> 3] |= (uint8_t)(bits[i] << (7 - (i & 7)));
	if (drm_crc(by, 5, 8) != by[5]) return 0;
	int v = by[0] >> 6;
	static const int idmap[4] = {-1, 1, 2, 0};
	*id = idmap[v];
	if (*id < 0) return 0;
	int so = by[0] >> 5 & 1, ilong = !(by[0] >> 4 & 1), msc = by[0] >> 3 & 1, prot = by[0] >> 2 & 1;
	/* layout: id2 so1 il1 msc1 prot1 data1 pid2 ext1 label21 */
	int ext = bits[9];
	if (!bits[6]) return 0;                 /* HamDRM always signals a data service */
	int qam = msc ? (ext ? DRM_QAM4 : DRM_QAM16) : DRM_QAM64;
	char lab[4];
	for (int i = 0; i < 3; i++) {
		int ch = 0;
		for (int b = 0; b < 7; b++) ch = ch << 1 | bits[10 + i * 7 + b];
		lab[i] = (char)ch;
	}
	for (int i = 0; i < 3; i++) {
		int at = *id * 3 + i;
		r->label[at] = (lab[i] >= 32 && lab[i] < 127) ? lab[i] : 0;
	}
	r->label[9] = 0;
	if (!r->params_ok || so != r->c.p.so || ilong != r->c.p.ilong || qam != r->c.p.qam || prot != r->c.p.prot) {
		drm_params p = r->c.p;
		p.so = so; p.ilong = ilong; p.qam = qam; p.prot = prot;
		int map_changed = so != r->c.p.so;
		drm_setup(&r->c, &p);
		msc_setup(r);
		r->so = so; r->qam = qam; r->prot = prot; r->ilong = ilong;
		r->params_ok = 1;
		if (map_changed) return 2;      /* this frame was collected with the wrong map */
	}
	/* SNR from the FAC cells */
	double num = 0, den = 0;
	for (int i = 0; i < DRM_FAC_CELLS; i++) {
		double complex y = r->fac_y[i];
		double complex d = (creal(y) > 0 ? 0.7071 : -0.7071) + I * (cimag(y) > 0 ? 0.7071 : -0.7071);
		num += r->fac_w[i];
		den += r->fac_w[i] * creal((y - d) * conj(y - d));
	}
	double snr = den > 0 ? 10 * log10(num / den) : 40;
	if (drm_rx_debug) fprintf(stderr, "frame id%d snr %.1f foff %.2f tau0 %.2f tacc %.2f wpos%%L %ld\n", *id, snr, r->foff, r->tslope0, r->tacc, r->wpos % r->symlen);
	r->snr = r->fac_ok ? 0.8 * r->snr + 0.2 * snr : snr;
	return 1;
}

static void frame_end(drm_rx *r)
{
	int id = -1, ok = 0;
	if (!r->fr_full) { r->nfac = 0; r->nfr = 0; return; }
	r->fr_full = 0;
	if (r->nfac == DRM_FAC_CELLS) ok = fac_decode(r, &id);
	r->frames_abs++;
	if (ok) {
		r->fac_ok++;
		r->fac_since_sync++;
		r->bad_frames = 0;
	} else {
		r->fac_bad++;
		r->bad_frames++;
		id = r->frame_id >= 0 ? (r->frame_id + 1) % 3 : -1;
		if (r->bad_frames >= 8 || (r->fac_since_sync == 0 && r->bad_frames >= 4)) {   /* lost: search again */
			r->state = DRM_RX_SEARCH;
			r->params_ok = 0;
			r->acq_next = r->wpos;
			mot_rx_flush(&r->mot);
		}
	}
	r->frame_id = id;
	if (ok == 1 && r->params_ok && id >= 0 && r->state == DRM_RX_SYNC) frame_msc(r, id);
	else if (ok != 1 && r->params_ok && id >= 0 && r->bad_frames < 3) frame_msc(r, id);
	r->nfac = 0;
	r->nfr = 0;
}

/* ---- tracking --------------------------------------------------------------- */

static void equalize(drm_rx *r, long j)
{
	drm_cfg *c = &r->c;
	drm_rx_sym *sj = &r->ring[j % DRM_RX_RING];
	static const int fint_t[3] = {4, 2, 1}, tint_t[3] = {5, 3, 4};
	int fint = fint_t[c->p.mode], T = tint_t[c->p.mode];
	int kbase = (int)(fint / 2.0 + 0.5);
	double complex H[DRM_MAX_CAR];
	/* time interpolation on the pilot carriers */
	for (int k = kbase; k <= c->kmax; k += fint) {
		if (k < c->kmin) continue;
		int ci = k - c->kmin;
		int rk = ((k - kbase) / fint) % T;
		int back = ((sj->fs - rk) % T + T) % T;
		long j1 = j - back, j2 = j1 + T;
		double complex h1 = 0, h2 = 0;
		int have1 = j1 >= 0 && j1 > r->nsym_rx - DRM_RX_RING, have2 = j2 < r->nsym_rx;
		if (have1) h1 = r->ring[j1 % DRM_RX_RING].h[ci];
		if (have2) h2 = r->ring[j2 % DRM_RX_RING].h[ci];
		if (have1 && have2) H[ci] = h1 + (h2 - h1) * (double)back / T;
		else H[ci] = have1 ? h1 : h2;
	}
	/* light smoothing along frequency on the pilot grid (less pilot noise) */
	{
		static const double sw = 0.25;
		double complex tmp[DRM_MAX_CAR];
		int first = kbase < c->kmin ? kbase + fint * ((c->kmin - kbase + fint - 1) / fint) : kbase;
		double complex g = 0;
		for (int k = first; k + fint <= c->kmax; k += fint) g += H[k + fint - c->kmin] * conj(H[k - c->kmin]);
		double complex rot = cabs(g) > 0 ? g / cabs(g) : 1;     /* phase step between grid points */
		for (int k = first; k <= c->kmax; k += fint) {
			int ci = k - c->kmin;
			double complex lo = k - fint >= first ? H[ci - fint] * rot : H[ci];
			double complex hi = k + fint <= c->kmax ? H[ci + fint] * conj(rot) : H[ci];
			tmp[ci] = (1 - 2 * sw) * H[ci] + sw * (lo + hi);
		}
		for (int k = first; k <= c->kmax; k += fint) H[k - c->kmin] = tmp[k - c->kmin];
	}
	/* frequency interpolation */
	for (int k = c->kmin; k <= c->kmax; k++) {
		int ci = k - c->kmin;
		if (k >= kbase && (k - kbase) % fint == 0) continue;
		int ka = kbase + ((k - kbase) / fint) * fint;
		if (k < kbase) ka = kbase;
		int kb = ka + fint;
		if (ka < c->kmin) ka = kb;
		if (kb > c->kmax) kb = ka;
		double complex ha = H[ka - c->kmin], hb = H[kb - c->kmin];
		H[ci] = kb == ka ? ha : ha + (hb - ha) * (double)(k - ka) / (kb - ka);
	}
	/* timing error from the phase slope of the channel */
	double complex sl = 0;
	for (int k = kbase; k + fint <= c->kmax; k += fint)
		if (k >= c->kmin) sl += H[k + fint - c->kmin] * conj(H[k - c->kmin]);
	double tau = -carg(sl) / fint * r->nfft / (2 * M_PI);
	if (r->tinit < 3 * T) r->tinit++;           /* first symbols: one-sided pilots, ignore */
	else {
		double e = tau - r->tslope0;
		if (e > 3) e = 3; else if (e < -3) e = -3;
		r->tacc += 0.05 * (e - r->tacc);
	}

	int fs = sj->fs;
	int ns = 0;
	if (fs == 0) { r->nfac = 0; r->nfr = 0; r->fr_full = 1; }
	for (int ci = 0; ci < c->ncar; ci++) {
		uint8_t ty = c->type[fs][ci];
		if (ty != CT_MSC && ty != CT_FAC) continue;
		double hh = creal(H[ci] * conj(H[ci]));
		double complex y = hh > 1e-12 ? sj->y[ci] / H[ci] : 0;
		float w = (float)hh;
		if (ty == CT_FAC) {
			if (r->nfac < DRM_FAC_CELLS) { r->fac_y[r->nfac] = y; r->fac_w[r->nfac] = w; r->nfac++; }
		} else if (r->fr_y) {
			r->fr_y[r->nfr] = y; r->fr_w[r->nfr] = w; r->nfr++;
			if (ns < 4 && r->nscope < 64) r->scope[r->nscope++] = y, ns++;
		}
	}
	if (r->nscope >= 64) r->nscope = 0;
	if (fs == c->nsym - 1) frame_end(r);
}

static void track_symbol(drm_rx *r)
{
	drm_cfg *c = &r->c;
	long j = r->nsym_rx;
	drm_rx_sym *s = &r->ring[j % DRM_RX_RING];
	double ph = r->phase_ref - 2 * M_PI * r->foff * (double)(r->wpos - r->n_ref) / FS;
	demod(r, r->wpos, r->foff, ph, c->kmin, c->kmax, s->y);
	s->fs = r->fs_next;
	for (int ci = 0; ci < c->ncar; ci++)
		s->h[ci] = (c->type[s->fs][ci] & CT_PILOT) ? s->y[ci] / c->pilot[s->fs][ci] : 0;
	r->nsym_rx++;
	/* frequency tracking from the frequency pilots */
	if (j > 0) {
		drm_rx_sym *p = &r->ring[(j - 1) % DRM_RX_RING];
		double complex acc = 0;
		for (int ci = 0; ci < c->ncar; ci++)
			if ((c->type[s->fs][ci] & CT_FREQ) && (c->type[p->fs][ci] & CT_FREQ)) acc += s->h[ci] * conj(p->h[ci]);
		double dfr = carg(acc) / (2 * M_PI * r->symlen / (double)FS);
		if (fabs(dfr) < 20) {
			double phn = r->phase_ref - 2 * M_PI * r->foff * (double)(r->wpos - r->n_ref) / FS;
			r->foff += 0.05 * dfr;
			r->phase_ref = phn;
			r->n_ref = r->wpos;
		}
	}
	/* equalize the symbol that now has pilots on both sides */
	static const int tint_t[3] = {5, 3, 4};
	int T = tint_t[c->p.mode];
	if (j - T >= 0) equalize(r, j - T);
	if (r->state != DRM_RX_SYNC) return;
	/* next window; timing correction in whole samples */
	r->wpos += r->symlen;
	r->fs_next = (r->fs_next + 1) % c->nsym;
	int d = 0;
	if (r->tacc > 0.6) d = 1; else if (r->tacc < -0.6) d = -1;
	if (d) {
		if (drm_rx_debug > 1) fprintf(stderr, "shift %d at sym %ld tau0 %.2f\n", d, j, r->tslope0);
		r->wpos += d;
		r->tacc -= d;
		for (int i = 0; i < DRM_RX_RING; i++)
			for (int ci = 0; ci < c->ncar; ci++) {
				int k = c->kmin + ci;
				double complex rt = cexp(I * 2 * M_PI * k * d / r->nfft);
				r->ring[i].h[ci] *= rt;
				r->ring[i].y[ci] *= rt;
			}
	}
}

/* ---- input ---------------------------------------------------------------- */

static void push_z(drm_rx *r, double complex v)
{
	if (r->zlen == r->zcap) {
		long keep_from = r->state == DRM_RX_SYNC ? r->wpos - 64 : r->acq_next;
		long drop = keep_from - r->zbase;
		if (drop < r->zcap / 4) drop = r->zcap / 4;
		if (drop > r->zlen) drop = r->zlen;
		memmove(r->z, r->z + drop, sizeof(double complex) * (r->zlen - drop));
		r->zlen -= (int)drop;
		r->zbase += drop;
		if (r->state == DRM_RX_SEARCH && r->acq_next < r->zbase) r->acq_next = r->zbase;
		if (r->state == DRM_RX_SYNC && r->wpos < r->zbase) drm_rx_reset(r);
	}
	r->z[r->zlen++] = v;
}

static void at12k(drm_rx *r, double x)
{
	double complex m = x * cexp(-I * r->ph1);
	r->ph1 += r->w1;
	if (r->ph1 > 2 * M_PI) r->ph1 -= 2 * M_PI;
	r->firbuf[r->firpos] = m;
	double complex acc = 0;
	int p = r->firpos;
	for (int i = 0; i < r->nfir; i++) {
		acc += r->firbuf[p] * r->fir[i];
		if (--p < 0) p = r->nfir - 1;
	}
	if (++r->firpos == r->nfir) r->firpos = 0;
	push_z(r, acc * cexp(I * r->ph2));
	r->ph2 += r->w2;
	if (r->ph2 > 2 * M_PI) r->ph2 -= 2 * M_PI;
	r->level = 0.9995 * r->level + 0.0005 * fabs(x);

	long have = r->zbase + r->zlen;
	if (r->state == DRM_RX_SEARCH) {
		if (have >= r->acq_next + ACQ_LEN) {
			if (!acquire(r, r->acq_next)) r->acq_next += ACQ_STEP;
		}
	}
	while (r->state == DRM_RX_SYNC && r->wpos + r->nfft + 2 <= have) track_symbol(r);
}

void drm_rx_process(drm_rx *r, const float *x, int n)
{
	if (r->infs == FS) { for (int i = 0; i < n; i++) at12k(r, x[i]); return; }
	double scale = r->infs > FS ? (double)FS / r->infs : 1.0;      /* anti-alias */
	double fc = RS_FC / r->infs * (r->infs > FS ? 1.0 : 1.0);
	(void)scale;
	double tstep = 2 * fc;                    /* kernel argument per input sample */
	int hw = (int)ceil(RS_HW / tstep) + 1;
	for (int i = 0; i < n; i++) {
		r->rs_hist[r->rs_count & (r->rs_nh - 1)] = x[i];
		r->rs_count++;
		while (r->rs_pos + hw < r->rs_count) {
			long c0 = (long)floor(r->rs_pos);
			double acc = 0;
			for (long k = c0 - hw + 1; k <= c0 + hw; k++) {
				if (k < 0) continue;
				double t = fabs((r->rs_pos - k) * tstep) * RS_TAB;
				int ti = (int)t;
				if (ti >= RS_HW * RS_TAB) continue;
				double fr = t - ti;
				double kv = rs_kernel[ti] * (1 - fr) + rs_kernel[ti + 1] * fr;
				acc += kv * r->rs_hist[k & (r->rs_nh - 1)];
			}
			at12k(r, acc * tstep);
			r->rs_pos += r->rs_step;
		}
	}
}

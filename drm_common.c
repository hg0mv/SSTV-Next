/* drm_common.c - HamDRM mode tables, cell map, channel coding helpers */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "drm.h"

/* ---- mode tables (HamDRM) ----------------------------------------------- */

typedef struct {
	int nfft, gnum, gden, nsym;
	int kmin[2], kmax[2];
	int ntime; const int (*time)[2];
	const int (*freq)[2];
	const int (*fac)[2];
	int fint, tint;            /* scattered pilot spacing in frequency / time */
	int x, y, k0;              /* phase constants */
	int wzcols; const int *w, *z; int q;
	int boost[2][4];
} mode_tab;

static const int timeA[16][2] = {
	{6,973},{7,205},{11,717},{12,264},{15,357},{16,357},{23,952},{29,440},
	{30,856},{33,88},{34,88},{38,68},{39,836},{41,836},{45,836},{46,1008}};
static const int timeB[15][2] = {
	{6,304},{10,331},{11,108},{14,620},{17,192},{18,704},{27,44},{28,432},
	{30,588},{33,844},{34,651},{38,651},{40,651},{41,460},{44,944}};
static const int timeE[8][2] = {
	{7,432},{8,331},{13,108},{14,620},{21,192},{22,704},{26,44},{27,304}};

static const int freqA[3][2] = {{9,205},{27,836},{36,215}};
static const int freqB[3][2] = {{8,331},{24,651},{32,555}};
static const int freqE[3][2] = {{5,788},{15,1014},{20,332}};

static const int facA[45][2] = {
	{1,10},{1,22},{1,30},{1,50},{2,14},{2,26},{2,34},{3,18},{3,30},{3,38},
	{4,22},{4,34},{4,42},{5,18},{5,26},{5,38},{5,46},{6,22},{6,30},{6,42},
	{6,50},{7,26},{7,34},{7,46},{8,10},{8,30},{8,38},{8,50},{9,14},{9,34},
	{9,42},{10,18},{10,38},{10,46},{11,10},{11,22},{11,42},{11,50},{12,14},
	{12,26},{12,46},{13,18},{13,30},{14,22},{14,34}};
static const int facB[45][2] = {
	{0,21},{1,11},{1,23},{1,35},{2,13},{2,25},{2,37},{3,15},{3,27},{3,39},
	{4,5},{4,17},{4,29},{4,41},{5,7},{5,19},{5,31},{6,9},{6,21},{6,33},
	{7,11},{7,23},{7,35},{8,13},{8,25},{8,37},{9,15},{9,27},{9,39},{10,5},
	{10,17},{10,29},{10,41},{11,7},{11,19},{11,31},{12,9},{12,21},{12,33},
	{13,11},{13,23},{13,35},{14,13},{14,25},{14,37}};
static const int facE[45][2] = {
	{1,7},{1,23},{2,8},{2,16},{2,24},{3,9},{3,17},{4,10},{4,18},{5,11},
	{5,19},{6,4},{6,12},{7,13},{7,21},{8,6},{8,14},{8,22},{9,7},{9,23},
	{10,8},{10,16},{10,24},{11,9},{11,13},{11,17},{12,10},{12,18},{13,11},
	{13,19},{14,4},{14,12},{14,16},{15,13},{15,21},{16,6},{16,14},{16,22},
	{17,7},{17,23},{18,8},{18,16},{18,24},{19,9},{19,17}};

static const int wA[15] = {228,341,455, 455,569,683, 683,796,910, 910,0,114, 114,228,341};
static const int zA[15] = {0,81,248, 18,106,106, 122,116,31, 129,129,39, 33,32,111};
static const int wB[15] = {512,0,512,0,512, 0,512,0,512,0, 512,0,512,0,512};
static const int zB[15] = {0,57,164,64,12, 168,255,161,106,118, 25,232,132,233,38};
static const int wE[20] = {512,0,512,0,512, 0,512,0,512,0, 512,0,512,0,512, 0,512,0,512,0};
static const int zE[20] = {0,57,164,64,12, 168,255,161,106,118, 25,232,132,233,38, 168,255,161,106,118};

static const mode_tab modes[3] = {
	{ 1152, 1, 9, 15, {2, 2}, {54, 58}, 16, timeA, freqA, facA, 4, 5, 4, 5, 2, 3, wA, zA, 36,
	  {{2, 4, 50, 54}, {2, 6, 54, 58}} },
	{ 1024, 1, 4, 15, {1, 1}, {45, 51}, 15, timeB, freqB, facB, 2, 3, 2, 3, 1, 5, wB, zB, 12,
	  {{1, 3, 43, 45}, {1, 3, 49, 51}} },
	{  640, 1, 2, 20, {1, 1}, {29, 31},  8, timeE, freqE, facE, 1, 4, 1, 4, 1, 5, wE, zE, 10,
	  {{1, 29, 0, 0}, {1, 31, 0, 0}} },
};

/* ---- channel coding tables ---------------------------------------------- */

/* puncturing patterns: {groups, ones, pattern...}; pattern bit mask over the
   four mother code outputs: 1 = g0, 2 = g1, 4 = g2, 8 = g3 */
enum { P0001 = 1, P0011 = 3, P0101 = 5, P0111 = 7, P1111 = 15 };
const int drm_punct[13][10] = {
	{1, 4,  P1111},
	{3, 10, P1111, P0111, P0111},
	{1, 3,  P0111},
	{4, 11, P0111, P0111, P0111, P0011},
	{1, 2,  P0011},
	{4, 7,  P0011, P0101, P0011, P0001},
	{3, 5,  P0011, P0001, P0011},
	{2, 3,  P0011, P0001},
	{8, 11, P0011, P0001, P0001, P0011, P0001, P0001, P0011, P0001},
	{3, 4,  P0011, P0001, P0001},
	{4, 5,  P0011, P0001, P0001, P0001},
	{7, 8,  P0011, P0001, P0001, P0001, P0001, P0001, P0001},
	{8, 9,  P0011, P0001, P0001, P0001, P0001, P0001, P0001, P0001},
};
const int drm_punct_tail[12][6] = {
	{P0011, P0011, P0011, P0011, P0011, P0011},
	{P0111, P0011, P0011, P0011, P0011, P0011},
	{P0111, P0011, P0011, P0111, P0011, P0011},
	{P0111, P0111, P0011, P0111, P0011, P0011},
	{P0111, P0111, P0011, P0111, P0111, P0011},
	{P0111, P0111, P0111, P0111, P0111, P0011},
	{P0111, P0111, P0111, P0111, P0111, P0111},
	{P1111, P0111, P0111, P0111, P0111, P0111},
	{P1111, P0111, P0111, P1111, P0111, P0111},
	{P1111, P1111, P0111, P1111, P0111, P0111},
	{P1111, P1111, P0111, P1111, P0111, P1111},
	{P1111, P1111, P1111, P1111, P0111, P1111},
};

#define RATE_FAC   6
#define RATE_QAM4  6
static const int rate16[2][2] = {{2, 7}, {4, 9}};
static const int rate64[2][3] = {{0, 4, 9}, {2, 7, 10}};

static const double qam4[2]  = {0.7071067811, -0.7071067811};
static const double qam16[4] = {0.9486832980, -0.3162277660, 0.3162277660, -0.9486832980};
static const double qam64[8] = {1.0801234497, -0.1543033499, 0.4629100498, -0.7715167498,
                                0.7715167498, -0.4629100498, 0.1543033499, -1.0801234497};

void drm_qam_points(int qam, const double **tab, int *n)
{
	if (qam == DRM_QAM4)       { *tab = qam4;  *n = 2; }
	else if (qam == DRM_QAM16) { *tab = qam16; *n = 4; }
	else                       { *tab = qam64; *n = 8; }
}

static double complex polar(double a, int ph1024)
{
	double ph = 2.0 * M_PI * ph1024 / 1024.0;
	return a * cos(ph) + I * a * sin(ph);
}

static int imod(int a, int b) { return a < 0 ? a % b + b : a % b; }

/* ---- setup ---------------------------------------------------------------- */

int drm_setup(drm_cfg *c, const drm_params *p)
{
	if (p->mode < 0 || p->mode > 2 || p->so < 0 || p->so > 1 || p->qam < 0 || p->qam > 2 ||
	    p->prot < 0 || p->prot > 1)
		return -1;
	memset(c, 0, sizeof *c);
	c->p = *p;
	const mode_tab *m = &modes[p->mode];
	c->nfft = m->nfft;
	c->nguard = m->nfft * m->gnum / m->gden;
	c->symlen = c->nfft + c->nguard;
	c->nsym = m->nsym;
	c->kmin = m->kmin[p->so];
	c->kmax = m->kmax[p->so];
	c->ncar = c->kmax - c->kmin + 1;
	c->idc = DRM_IF * c->nfft / DRM_FS;
	const int *boost = m->boost[p->so];
	int nsf = c->nsym * DRM_FRAMES_SF;

	for (int s = 0; s < nsf; s++) {
		int fs = s % c->nsym;
		int k0 = (int)(m->fint / 2.0 + 0.5) + m->fint * (fs % m->tint);
		for (int k = c->kmin; k <= c->kmax; k++) {
			int ci = k - c->kmin;
			uint8_t t = CT_MSC;
			double complex v = 0;
			for (int i = 0; i < DRM_FAC_CELLS; i++)
				if (m->fac[i][0] == fs && m->fac[i][1] == k) { t = CT_FAC; break; }
			/* gain references (scattered pilots) */
			if (k >= k0 && (k - k0) % (m->fint * m->tint) == 0) {
				int n = fs % m->y, mm = fs / m->y;
				int pp = (k - m->k0 - n * m->x) / (m->x * m->y);
				int ph = imod(4 * m->z[n * m->wzcols + mm] + pp * m->w[n * m->wzcols + mm] +
				              pp * pp * (1 + fs) * m->q, 1024);
				int b = 0;
				for (int i = 0; i < 4; i++) if (boost[i] == k) b = 1;
				t = CT_SCAT | (b ? CT_BOOST : 0);
				v = polar(b ? 2.0 : sqrt(2.0), ph);
			}
			/* time references in the first symbol of each frame */
			if (fs == 0)
				for (int i = 0; i < m->ntime; i++)
					if (m->time[i][0] == k) {
						t = (t & CT_SCAT) ? (t | CT_TIME) : CT_TIME;
						v = polar(sqrt(2.0), m->time[i][1]);
					}
			/* frequency references in every symbol */
			for (int i = 0; i < 3; i++)
				if (m->freq[i][0] == k) {
					t = (t & (CT_SCAT | CT_TIME)) ? (t | CT_FREQ) : CT_FREQ;
					int ph = m->freq[i][1];
					if (p->mode == DRM_MODE_E && i != 2 && (fs & 1)) ph = (ph + 512) % 1024;
					v = polar(sqrt(2.0), ph);
				}
			if (k == 0 || (p->mode == DRM_MODE_A && (k == 1 || k == -1))) t = CT_DC, v = 0;
			c->type[s][ci] = t;
			c->pilot[s][ci] = (t & CT_PILOT) ? v : 0;
		}
	}
	int total = 0;
	for (int s = 0; s < nsf; s++) {
		c->nmsc_sym[s] = c->nfac_sym[s] = 0;
		for (int ci = 0; ci < c->ncar; ci++) {
			if (c->type[s][ci] == CT_MSC) c->nmsc_sym[s]++, total++;
			if (c->type[s][ci] == CT_FAC) c->nfac_sym[s]++;
		}
	}
	c->nmux = total / DRM_FRAMES_SF;
	c->ndummy = total - c->nmux * DRM_FRAMES_SF;
	c->nmsc_sym[nsf - 1] -= c->ndummy;

	/* MSC multilevel coding: equal error protection, standard mapping */
	int n2 = 2 * c->nmux - 12;
	if (p->qam == DRM_QAM4) {
		c->levels = 1;
		c->rate[0] = RATE_QAM4;
		c->ilv[0] = 21;
	} else if (p->qam == DRM_QAM16) {
		c->levels = 2;
		c->rate[0] = rate16[p->prot][0]; c->rate[1] = rate16[p->prot][1];
		c->ilv[0] = 13; c->ilv[1] = 21;
	} else {
		c->levels = 3;
		for (int i = 0; i < 3; i++) c->rate[i] = rate64[p->prot][i];
		c->ilv[0] = 0; c->ilv[1] = 13; c->ilv[2] = 21;
	}
	c->nbits = 0;
	for (int i = 0; i < c->levels; i++) {
		const int *pt = drm_punct[c->rate[i]];
		c->m[i] = pt[0] * (n2 / pt[1]);
		c->tailpat[i] = n2 - pt[1] * (n2 / pt[1]);
		c->nbits += c->m[i];
	}
	c->nbytes = c->nbits / 8;
	c->paclen = c->nbytes - 3;
	return 0;
}

double drm_frame_seconds(const drm_cfg *c)
{
	return (double)c->symlen * c->nsym / DRM_FS;
}

/* ---- coding helpers -------------------------------------------------------- */

/* DRM block interleaver permutation (used for bit and cell interleaving) */
void drm_block_table(int *t, int size, int t0)
{
	int hi = size, s = 1 << 17;
	while (!(hi & (1 << 16))) { hi <<= 1; s >>= 1; }
	int q = s / 4 - 1;
	t[0] = 0;
	for (int i = 1; i < size; i++) {
		t[i] = (t0 * t[i - 1] + q) % s;
		while (t[i] >= size) t[i] = (t0 * t[i] + q) % s;
	}
}

/* PRBS x^9 + x^5 + 1, register initialised to all ones */
void drm_energy_dispersal(uint8_t *bits, int n)
{
	unsigned r = 0x1ff;
	for (int i = 0; i < n; i++) {
		unsigned b = ((r >> 4) ^ (r >> 8)) & 1;
		r = ((r << 1) | b) & 0x1ff;
		bits[i] ^= b;
	}
}

/* per input bit: which mother code outputs are sent */
void drm_punct_table(int *tab, int nin, int rate, int tailpat, int fac)
{
	const int *pt = drm_punct[rate];
	int cnt = 0;
	for (int i = 0; i < nin + 6; i++) {
		if (i < nin || fac) {
			tab[i] = pt[2 + cnt];
			if (++cnt == pt[0]) cnt = 0;
		} else {
			if (i == nin) cnt = 0;
			tab[i] = drm_punct_tail[tailpat][cnt++];
		}
	}
}

static const uint8_t gen[4] = {0155, 0117, 0123, 0155};   /* 133 171 145 133 (bit reversed) */

static int parity(unsigned v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return v & 1; }

/* rate 1/4 mother code (K = 7) with puncturing, 6 tail bits; returns number of output bits */
int drm_conv_encode(const uint8_t *in, int nin, int rate, int tailpat, int fac, uint8_t *out)
{
	int tab[nin + 6];
	drm_punct_table(tab, nin, rate, tailpat, fac);
	unsigned st = 0;
	int no = 0;
	for (int i = 0; i < nin + 6; i++) {
		st = (st << 1) & 0xff;
		if (i < nin && in[i]) st |= 1;
		for (int g = 0; g < 4; g++)
			if (tab[i] & (1 << g)) out[no++] = parity(st & gen[g]);
	}
	return no;
}

/* CRC as used by DRM: registers preset to ones, result inverted.
   degree 8: x^8+x^4+x^3+x^2+1, degree 16: x^16+x^12+x^5+1 (X.25) */
unsigned drm_crc(const uint8_t *d, int n, int degree)
{
	unsigned poly = degree == 8 ? 0x1d : 0x1021, top = 1u << (degree - 1), mask = (1u << degree) - 1;
	unsigned r = mask;
	for (int i = 0; i < n; i++)
		for (int b = 7; b >= 0; b--) {
			unsigned in = (d[i] >> b) & 1, fb = ((r & top) ? 1 : 0) ^ in;
			r = (r << 1) & mask;
			if (fb) r ^= poly;
		}
	return ~r & mask;
}

static void put_bits(uint8_t *bits, int *pos, unsigned v, int n)
{
	for (int i = n - 1; i >= 0; i--) bits[(*pos)++] = (v >> i) & 1;
}

/* fast access channel block for frame 0..2 of the super frame */
void drm_fac_bits(const drm_cfg *c, int frame, uint8_t *bits)
{
	static const int id[3] = {3, 1, 2};
	int pos = 0;
	memset(bits, 0, DRM_FAC_BITS);
	put_bits(bits, &pos, id[frame], 2);
	put_bits(bits, &pos, c->p.so, 1);
	put_bits(bits, &pos, c->p.ilong ? 0 : 1, 1);
	put_bits(bits, &pos, c->p.qam == DRM_QAM64 ? 0 : 1, 1);
	put_bits(bits, &pos, c->p.prot, 1);
	put_bits(bits, &pos, 1, 1);                  /* data service */
	put_bits(bits, &pos, 0, 2);                  /* packet id */
	put_bits(bits, &pos, c->p.qam == DRM_QAM4 ? 1 : 0, 1);
	int len = (int)strlen(c->p.call);
	if (len > 9) len = 9;
	for (int i = 3 * frame; i < 3 * frame + 3; i++)
		put_bits(bits, &pos, i < len ? (c->p.call[i] & 127) : 0, 7);
	uint8_t by[5];
	for (int i = 0; i < 5; i++) {
		by[i] = 0;
		for (int b = 0; b < 8; b++) by[i] = (uint8_t)(by[i] << 1 | bits[i * 8 + b]);
	}
	pos = 40;
	put_bits(bits, &pos, drm_crc(by, 5, 8), 8);
}

/* ---- soft decision Viterbi decoder ---------------------------------------- */

/* llr: one value per transmitted (punctured) code bit, > 0 means "0".
   Returns the decoded information bits in out (nin bits). */
void drm_viterbi(const float *llr, int nin, int rate, int tailpat, int fac, uint8_t *out)
{
	int nst = nin + 6;
	int *tab = malloc(sizeof(int) * nst);
	drm_punct_table(tab, nin, rate, tailpat, fac);
	uint64_t *dec = malloc(sizeof(uint64_t) * nst);
	float pm[64], npm[64];
	/* output bits for each 7-bit register value */
	static uint8_t outb[128][4];
	static int init;
	if (!init) {
		for (int r = 0; r < 128; r++)
			for (int g = 0; g < 4; g++) outb[r][g] = (uint8_t)parity(r & gen[g]);
		init = 1;
	}
	for (int s = 0; s < 64; s++) pm[s] = s ? -1e30f : 0;
	int pos = 0;
	for (int i = 0; i < nst; i++) {
		float m[4] = {0, 0, 0, 0};
		for (int g = 0; g < 4; g++) if (tab[i] & (1 << g)) m[g] = llr[pos++];
		uint64_t d = 0;
		int only0 = i >= nin;                /* tail: input bit is 0 */
		for (int ns = 0; ns < 64; ns++) {
			int b = ns & 1;
			if (only0 && b) { npm[ns] = -1e30f; continue; }
			/* predecessors: old state os, register = os << 1 | b, ns = reg & 63 */
			int os0 = ns >> 1, os1 = (ns >> 1) | 32;
			int r0 = (os0 << 1 | b) & 127, r1 = (os1 << 1 | b) & 127;
			float a = pm[os0], c = pm[os1];
			for (int g = 0; g < 4; g++) {
				a += outb[r0][g] ? -m[g] : m[g];
				c += outb[r1][g] ? -m[g] : m[g];
			}
			if (c > a) { npm[ns] = c; d |= 1ull << ns; } else npm[ns] = a;
		}
		dec[i] = d;
		float mx = -1e30f;
		for (int s = 0; s < 64; s++) if (npm[s] > mx) mx = npm[s];
		for (int s = 0; s < 64; s++) pm[s] = npm[s] - mx;
	}
	int st = 0;
	for (int i = nst - 1; i >= 0; i--) {
		int b = st & 1;
		if (i < nin) out[i] = (uint8_t)b;
		st = (dec[i] >> st & 1) ? (st >> 1) | 32 : st >> 1;
	}
	free(dec);
	free(tab);
}

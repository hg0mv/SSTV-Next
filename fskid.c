/* fskid.c - FSK ID decoder / encoder (see fskid.h) */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fskid.h"

#define BIT_S 0.022

void fskid_init(fskid_rx *r, double fs, fskid_fn cb, void *user)
{
	memset(r, 0, sizeof *r);
	r->fs = fs;
	r->cb = cb;
	r->user = user;
	r->nwin = (int)(fs * 0.008) | 1;               /* 8 ms average */
	r->win = calloc(r->nwin, sizeof(float));
}

void fskid_free(fskid_rx *r) { free(r->win); r->win = NULL; }

int fskid_valid_call(const char *s)
{
	int n = (int)strlen(s), digit = 0, alpha = 0;
	if (n < 3 || n > 10) return 0;
	for (int i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (isdigit(c)) digit = 1;
		else if (isupper(c)) alpha = 1;
		else if (c != '/' && c != '-') return 0;
	}
	return digit && alpha;
}

static void reset(fskid_rx *r) { r->state = 0; r->run = 0; r->run_sum = 0; }

/* one 6 bit symbol arrived */
static void symbol(fskid_rx *r, int s)
{
	if (r->stage == 0) {
		if (s != 0x2A) { reset(r); return; }
		r->stage = 1; r->chk = 0; r->nt = 0;
	} else if (r->stage == 1) {
		if (s == 0x01) { r->stage = 2; return; }
		if (r->nt >= 12) { reset(r); return; }
		r->chk ^= s;
		r->text[r->nt++] = (char)(s + 0x20);
	} else {
		r->text[r->nt] = 0;
		if ((r->chk & 0x3F) == s && fskid_valid_call(r->text) && r->cb) r->cb(r->user, r->text);
		reset(r);
	}
}

void fskid_process(fskid_rx *r, float f)
{
	if (f < 1500) f = 1500; else if (f > 2500) f = 2500;   /* clip FM noise clicks */
	/* 3 ms moving average for the lead tone and the start bit edge */
	r->wsum += f - r->win[r->wpos];
	r->win[r->wpos] = f;
	if (++r->wpos == r->nwin) r->wpos = 0;
	double fa = r->wsum / r->nwin;
	long n = r->n++;

	if (r->state == 0) {
		double mean = r->run ? r->run_sum / r->run : 0;
		if (r->run && fabs(fa - mean) < 60) {          /* steady tone continues */
			r->run++; r->run_sum += fa;
			if (r->run > r->fs * 0.07 && mean > 1950 && mean < 2250) { r->lead = mean; r->lead_end = n; }
			return;
		}
		/* edge into the 1900 Hz start bit shortly after a >= 70 ms 2100 Hz lead (midpoint crossing) */
		if (r->lead > 0 && n - r->lead_end < r->fs * 0.012 && fa < r->lead - 100 && fa > r->lead - 300) {
			r->offset = r->lead - 2100;
			long edge = n - r->nwin / 2;
			r->t0 = edge + (long)(BIT_S * r->fs + 0.5);
			r->state = 1; r->slot = 0; r->acc = 0; r->cnt = 0;
			r->bits = 0; r->nbits = 0; r->stage = 0;
			r->run = 0; r->run_sum = 0; r->lead = 0;
			return;
		}
		if (r->lead > 0 && n - r->lead_end >= r->fs * 0.012) r->lead = 0;
		r->run = 1; r->run_sum = fa;                 /* start a new tone measurement */
		return;
	}
	/* data bits: average the middle half of each 22 ms slot */
	double pos = (double)(n - r->t0) / (BIT_S * r->fs);
	if (pos < 0) return;
	int slot = (int)pos;
	double frac = pos - slot;
	if (slot != r->slot) {
		if (r->cnt == 0) { reset(r); return; }
		double av = r->acc / r->cnt - r->offset;
		if (av < 1700 || av > 2350) { reset(r); return; }      /* not FSK any more */
		int bit = av < 2000;                                     /* 1900 Hz = 1 */
		r->bits |= bit << r->nbits;
		if (++r->nbits == 6) { int s = r->bits; r->bits = 0; r->nbits = 0; symbol(r, s); if (r->state == 0) return; }
		r->slot = slot; r->acc = 0; r->cnt = 0;
	}
	if (frac > 0.25 && frac < 0.75) { r->acc += f; r->cnt++; }
}

/* ---- transmit ------------------------------------------------------------- */

typedef struct { float *x; long n, cap; double ph, fs; } gen;

static void tone(gen *g, double hz, double sec)
{
	long k = (long)(sec * g->fs + 0.5);
	if (g->n + k > g->cap) { g->cap = (g->n + k) * 2; g->x = realloc(g->x, sizeof(float) * g->cap); }
	for (long i = 0; i < k; i++) {
		g->x[g->n++] = (float)(0.7 * sin(g->ph));
		g->ph += 2 * M_PI * hz / g->fs;
		if (g->ph > 2 * M_PI) g->ph -= 2 * M_PI;
	}
}

static void tchar(gen *g, int c)
{
	for (int i = 0; i < 6; i++) tone(g, (c >> i) & 1 ? 1900.0 : 2100.0, BIT_S);
}

float *fskid_tx(const char *call, double fs, long *n)
{
	gen g = {0};
	g.fs = fs;
	tone(&g, 1500, 0.3);
	tone(&g, 2100, 0.1);
	tone(&g, 1900, BIT_S);                       /* start bit */
	tchar(&g, 0x2A);
	int chk = 0;
	for (const char *p = call; *p && p - call < 12; p++) {
		int c = toupper((unsigned char)*p);
		if (c < 0x20 || c > 0x5F) continue;
		c -= 0x20;
		chk ^= c;
		tchar(&g, c);
	}
	tchar(&g, 0x01);
	tchar(&g, chk & 0x3F);
	tone(&g, 1900, 0.1);
	/* short fade out against clicks */
	for (long i = 0; i < 64 && i < g.n; i++) g.x[g.n - 1 - i] *= (float)i / 64;
	*n = g.n;
	return g.x;
}

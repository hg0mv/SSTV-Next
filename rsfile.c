/* rsfile.c - Reed-Solomon protected files (.rs1 .. .rs4) as used with HamDRM
 *
 * File layout (EasyPal / QSSTV compatible): 7 byte header (padding, number of
 * blocks lo/hi, 3 character extension, 0) + file data, cut into blocks of KK
 * bytes, each block extended to RS(255,KK) over GF(256) (x^8+x^4+x^3+x^2+1,
 * roots alpha^1..alpha^(255-KK)), then interleaved byte by byte across the
 * blocks so that a lost transmission segment becomes a few erasures in every
 * block.  Own implementation (Berlekamp-Massey with erasures, Chien, Forney).
 */
#include <stdlib.h>
#include <string.h>
#include "rsfile.h"

#define NN 255
static uint8_t alog[512];
static int lg[256];
static int ready;

static void gf_init(void)
{
	if (ready) return;
	int x = 1;
	for (int i = 0; i < 255; i++) {
		alog[i] = alog[i + 255] = (uint8_t)x;
		lg[x] = i;
		x <<= 1;
		if (x & 256) x ^= 0x11d;
	}
	lg[0] = -1;
	ready = 1;
}
static inline uint8_t gmul(uint8_t a, uint8_t b) { return (a && b) ? alog[lg[a] + lg[b]] : 0; }
static inline uint8_t gdiv(uint8_t a, uint8_t b) { return a ? alog[(lg[a] - lg[b] + 255) % 255] : 0; }
static inline uint8_t gpow(int e) { e %= 255; if (e < 0) e += 255; return alog[e]; }

int rs_kk(int type)
{
	static const int kk[5] = {0, 224, 192, 160, 128};
	return type >= 1 && type <= 4 ? kk[type] : 0;
}

/* memory index <-> power of x */
static int mem2pow(int m, int kk) { return m < kk ? m + NN - kk : m - kk; }

void rs_encode_block(uint8_t *blk, int kk)
{
	gf_init();
	int np = NN - kk;
	uint8_t g[NN + 1] = {0};          /* generator, g[0] = constant term */
	g[0] = 1;
	for (int i = 1; i <= np; i++) {    /* multiply by (x - alpha^i) */
		uint8_t r = gpow(i);
		for (int j = i; j > 0; j--) g[j] = g[j - 1] ^ gmul(g[j], r);
		g[0] = gmul(g[0], r);
	}
	/* parity = (data(x) * x^np) mod g(x); data byte m is the coefficient of x^(m+np) */
	uint8_t rem[NN] = {0};
	for (int m = kk - 1; m >= 0; m--) {
		uint8_t fb = blk[m] ^ rem[np - 1];
		for (int j = np - 1; j > 0; j--) rem[j] = rem[j - 1] ^ gmul(fb, g[j]);
		rem[0] = gmul(fb, g[0]);
	}
	memcpy(blk + kk, rem, np);         /* parity byte j is the coefficient of x^j */
}

int rs_decode_block(uint8_t *blk, int kk, const int *eras, int ne)
{
	gf_init();
	int np = NN - kk;
	if (ne > np) return -1;
	uint8_t S[NN], lam[NN + 1] = {0}, b[NN + 1] = {0}, t[NN + 1], om[NN + 1] = {0};
	int synz = 1;
	for (int j = 1; j <= np; j++) {
		uint8_t s = 0;
		for (int m = 0; m < NN; m++)
			if (blk[m]) s ^= gmul(blk[m], gpow(mem2pow(m, kk) * j));
		S[j - 1] = s;
		if (s) synz = 0;
	}
	if (synz) return 0;
	/* erasure locator */
	lam[0] = 1;
	for (int i = 0; i < ne; i++) {
		uint8_t X = gpow(mem2pow(eras[i], kk));
		for (int j = i + 1; j > 0; j--) lam[j] ^= gmul(lam[j - 1], X);
	}
	memcpy(b, lam, sizeof b);
	int L = ne;
	for (int r = ne + 1; r <= np; r++) {           /* Berlekamp-Massey */
		uint8_t d = 0;
		for (int i = 0; i < r; i++) if (lam[i]) d ^= gmul(lam[i], S[r - 1 - i]);
		/* b <- x b */
		memmove(b + 1, b, NN);
		b[0] = 0;
		if (d) {
			for (int i = 0; i <= NN; i++) t[i] = lam[i] ^ gmul(d, b[i]);
			if (2 * L <= r + ne - 1) {
				L = r + ne - L;
				for (int i = 0; i <= NN; i++) b[i] = gdiv(lam[i], d);
			}
			memcpy(lam, t, sizeof t);
		}
	}
	int deg = 0;
	for (int i = 0; i <= NN; i++) if (lam[i]) deg = i;
	if (deg > np) return -1;
	/* Chien search over the valid positions */
	int pos[NN], np2 = 0;
	for (int m = 0; m < NN; m++) {
		int e = mem2pow(m, kk);
		uint8_t v = 0;
		for (int i = 0; i <= deg; i++) if (lam[i]) v ^= gmul(lam[i], gpow(-e * i));
		if (!v) pos[np2++] = m;
	}
	if (np2 != deg) return -1;
	/* omega = S(x) lambda(x) mod x^np */
	for (int i = 0; i < np; i++) {
		uint8_t v = 0;
		for (int j = 0; j <= i && j <= deg; j++) v ^= gmul(lam[j], S[i - j]);
		om[i] = v;
	}
	/* Forney (first consecutive root 1) */
	for (int k = 0; k < np2; k++) {
		int e = mem2pow(pos[k], kk);
		uint8_t xi = gpow(-e), num = 0, den = 0;
		for (int i = 0; i < np; i++) if (om[i]) num ^= gmul(om[i], gpow(-e * i));
		for (int i = 1; i <= deg; i += 2) if (lam[i]) den ^= gmul(lam[i], gpow(-e * (i - 1)));
		if (!den) return -1;
		(void)xi;
		blk[pos[k]] ^= gdiv(num, den);
	}
	return np2;
}

/* ---- file wrapper ----------------------------------------------------------- */

uint8_t *rs_file_encode(const uint8_t *data, long len, const char *ext, int type, long *outlen)
{
	int kk = rs_kk(type);
	if (!kk) return NULL;
	long chunks = (len + 7) / kk + (((len + 7) % kk) ? 1 : 0);
	uint8_t *ec = calloc((size_t)chunks * NN, 1);
	uint8_t *plain = calloc((size_t)chunks * kk, 1);
	plain[0] = (uint8_t)(kk - (len % kk));
	plain[1] = (uint8_t)(chunks % 256);
	plain[2] = (uint8_t)(chunks / 256);
	for (int i = 0; i < 3; i++) plain[3 + i] = (uint8_t)(ext && i < (int)strlen(ext) ? ext[i] : 0);
	plain[6] = 0;
	memcpy(plain + 7, data, len);
	for (long b = 0; b < chunks; b++) {
		memcpy(ec + b * NN, plain + b * kk, kk);
		rs_encode_block(ec + b * NN, kk);
	}
	free(plain);
	long rc = chunks * NN;
	uint8_t *out = malloc(rc);
	for (long i = 0; i < rc; i++) {            /* block b, position p -> p * chunks + b */
		long b = i / NN, p = i % NN;
		out[p * chunks + b] = ec[i];
	}
	free(ec);
	*outlen = rc;
	return out;
}

uint8_t *rs_file_decode(const uint8_t *tr, long got, int type, const uint8_t *lost,
                        long *outlen, char ext[4], int *fixed)
{
	int kk = rs_kk(type);
	if (!kk || got <= 0) return NULL;
	long bep = (got + NN - 1) / NN, rc = bep * NN;
	uint8_t *ec = calloc(rc, 1), *er = calloc(rc, 1);
	for (long t = 0; t < rc; t++) {
		long dst = (t % bep) * NN + t / bep;
		ec[dst] = t < got ? tr[t] : 0;
		er[dst] = t >= got || (lost && lost[t]);
	}
	uint8_t *plain = malloc((size_t)bep * kk);
	int corr = 0, ok = 1;
	for (long b = 0; b < bep && ok; b++) {
		int eras[NN], ne = 0;
		for (int p = 0; p < NN; p++) if (er[b * NN + p]) eras[ne++] = p;
		if (ne > NN - kk) { ok = 0; break; }
		int r = rs_decode_block(ec + b * NN, kk, eras, ne);
		if (r < 0) ok = 0; else corr += r;
		memcpy(plain + b * kk, ec + b * NN, kk);
	}
	free(ec); free(er);
	if (!ok || bep != plain[1] + 256L * plain[2]) { free(plain); return NULL; }
	long size = bep * kk - plain[0];
	if (size > bep * kk - 7) size = bep * kk - 7;
	if (size < 0) size = 0;
	uint8_t *out = malloc(size ? size : 1);
	memcpy(out, plain + 7, size);
	for (int i = 0; i < 3; i++) ext[i] = (char)plain[3 + i];
	ext[3] = 0;
	free(plain);
	*outlen = size;
	if (fixed) *fixed = corr;
	return out;
}

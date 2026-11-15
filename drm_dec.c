/* drm_dec - command line HamDRM decoder: WAV -> received files */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drm_rx.h"
#include "wav.h"

static const char *outdir = ".";
extern int drm_rx_debug;

static void done(void *u, const char *name, const uint8_t *data, long len, int partial)
{
	(void)u;
	char path[1024];
	snprintf(path, sizeof path, "%s/%s%s", outdir, partial ? "part_" : "", name);
	FILE *f = fopen(path, "wb");
	if (f) { fwrite(data, 1, len, f); fclose(f); }
	printf("received %s (%ld bytes)\n", path, len);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	int verbose = 0, opt = 1;
	const char *bsrout = NULL;
	while (opt < argc && argv[opt][0] == '-') {
		if (!strcmp(argv[opt], "-v")) verbose = 1;
		else if (!strcmp(argv[opt], "-d")) drm_rx_debug++;
		else if (!strcmp(argv[opt], "-o") && opt + 1 < argc) outdir = argv[++opt];
		else if (!strcmp(argv[opt], "-b") && opt + 1 < argc) bsrout = argv[++opt];
		opt++;
	}
	if (opt >= argc) { fprintf(stderr, "usage: drm_dec [-v] [-o dir] [-b bsr.txt] in.wav [more.wav ...]\n"); return 1; }
	double fs = 0; long n = 0;
	float *x = NULL;
	for (int a = opt; a < argc; a++) {            /* several files: one after the other */
		double f2; long n2;
		float *y = wav_read(argv[a], &f2, &n2);
		if (!y) { fprintf(stderr, "cannot read %s\n", argv[a]); return 1; }
		if (fs && f2 != fs) { fprintf(stderr, "%s: different sample rate\n", argv[a]); return 1; }
		fs = f2;
		x = realloc(x, sizeof(float) * (n + n2));
		memcpy(x + n, y, sizeof(float) * n2);
		n += n2;
		free(y);
	}
	drm_rx r;
	drm_rx_init(&r, (int)fs, done, NULL);
	int blk = (int)fs / 10, st = -1;
	long fo = -1;
	for (long i = 0; i < n; i += blk) {
		int m = (int)(n - i < blk ? n - i : blk);
		drm_rx_process(&r, x + i, m);
		if (verbose && (r.state != st || r.fac_ok + r.fac_bad != fo)) {
			if (r.state != st || (r.fac_ok + r.fac_bad) % 10 == 0) {
				int have, tot; const char *nm;
				mot_rx_progress(&r.mot, &have, &tot, &nm);
				printf("%6.1fs %s mode %c so%d qam%d prot%d il%d '%s' foff %+.1f snr %.1f fac %ld/%ld msc %ld/%ld seg %d/%d\n",
				       (double)i / fs, r.state ? "SYNC" : "srch", "ABE"[r.mode], r.so,
				       r.qam == 0 ? 4 : r.qam == 1 ? 16 : 64, r.prot, r.ilong, r.label, r.foff, r.snr,
				       r.fac_ok, r.fac_bad, r.msc_ok, r.msc_bad, have, tot);
			}
			st = r.state; fo = r.fac_ok + r.fac_bad;
		}
	}
	if (bsrout) {
		char t[8192];
		int bn = mot_rx_bsr(&r.mot, t, sizeof t);
		FILE *f = fopen(bsrout, "wb");
		if (f) { fwrite(t, 1, bn, f); fclose(f); }
		printf("repeat request: %d bytes -> %s\n", bn, bsrout);
	}
	mot_rx_flush(&r.mot);
	int have, tot; const char *nm;
	mot_rx_progress(&r.mot, &have, &tot, &nm);
	printf("end: fac %ld ok / %ld bad, msc %ld ok / %ld bad, segments %d/%d %s\n",
	       r.fac_ok, r.fac_bad, r.msc_ok, r.msc_bad, have, tot, nm);
	drm_rx_free(&r);
	free(x);
	return 0;
}

/* drm_enc - command line HamDRM encoder: file -> WAV (48 kHz mono) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drm_tx.h"
#include "mot.h"
#include "rsfile.h"

static int write_wav16(const char *path, const short *x, long n, int fs)
{
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	unsigned v; unsigned short h;
	fwrite("RIFF", 1, 4, f); v = 36 + (unsigned)n * 2; fwrite(&v, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
	h = 1; fwrite(&h, 2, 1, f); fwrite(&h, 2, 1, f);
	v = (unsigned)fs; fwrite(&v, 4, 1, f); v = (unsigned)fs * 2; fwrite(&v, 4, 1, f);
	h = 2; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
	fwrite("data", 1, 4, f); v = (unsigned)n * 2; fwrite(&v, 4, 1, f);
	fwrite(x, 2, (size_t)n, f);
	return fclose(f);
}

static void src(void *u, uint8_t *bits) { mot_tx_frame((mot_tx *)u, bits); }

int main(int argc, char **argv)
{
	drm_params p = {DRM_MODE_B, 1, DRM_QAM16, 1, 1, "N0CALL"};
	const char *name = NULL;
	int extra = 0, opt = 1, rs = 0, nfix = 0;
	static int fix[8192];
	double gain = 1000.0;
	while (opt < argc && argv[opt][0] == '-') {
		const char *o = argv[opt];
		if (opt + 1 >= argc) break;
		if (!strcmp(o, "-m")) p.mode = argv[++opt][0] == 'A' ? DRM_MODE_A : argv[opt][0] == 'B' ? DRM_MODE_B : DRM_MODE_E;
		else if (!strcmp(o, "-b")) p.so = atoi(argv[++opt]);
		else if (!strcmp(o, "-q")) { int q = atoi(argv[++opt]); p.qam = q == 4 ? DRM_QAM4 : q == 16 ? DRM_QAM16 : DRM_QAM64; }
		else if (!strcmp(o, "-p")) p.prot = atoi(argv[++opt]);
		else if (!strcmp(o, "-i")) p.ilong = atoi(argv[++opt]);
		else if (!strcmp(o, "-c")) snprintf(p.call, sizeof p.call, "%s", argv[++opt]);
		else if (!strcmp(o, "-n")) name = argv[++opt];
		else if (!strcmp(o, "-x")) extra = atoi(argv[++opt]);
		else if (!strcmp(o, "-g")) gain = atof(argv[++opt]);
		else if (!strcmp(o, "-r")) rs = atoi(argv[++opt]);
		else if (!strcmp(o, "-F")) {          /* answer a repeat request (bsr.bin) */
			FILE *bf = fopen(argv[++opt], "rb");
			char t[16384]; long bl = bf ? (long)fread(t, 1, sizeof t, bf) : 0;
			if (bf) fclose(bf);
			unsigned tid;
			nfix = mot_bsr_parse(t, bl, &tid, fix, 8192);
			fprintf(stderr, "repeat request for id %u: %d segments\n", tid, nfix);
			if (nfix < 0) nfix = 0;
		}
		else break;
		opt++;
	}
	if (argc - opt < 2) {
		fprintf(stderr, "usage: drm_enc [-m A|B|E] [-b 0|1] [-q 4|16|64] [-p 0|1] [-i 1|0]\n"
		                "               [-c CALL] [-n name.jpg] [-x extra_frames] [-r 1..4 (Reed-Solomon)] in.jpg out.wav\n");
		return 1;
	}
	FILE *f = fopen(argv[opt], "rb");
	if (!f) { perror(argv[opt]); return 1; }
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = malloc(len);
	if (fread(data, 1, len, f) != (size_t)len) { fclose(f); return 1; }
	fclose(f);
	if (!name) {
		name = strrchr(argv[opt], '/');
		name = name ? name + 1 : argv[opt];
	}
	char rsname[256];
	if (rs >= 1 && rs <= 4) {             /* Reed-Solomon protected file: name.rsN */
		const char *dot = strrchr(name, '.');
		long ol;
		uint8_t *enc = rs_file_encode(data, len, dot ? dot + 1 : "jpg", rs, &ol);
		free(data); data = enc; len = ol;
		snprintf(rsname, sizeof rsname, "%.*s.rs%d", dot ? (int)(dot - name) : (int)strlen(name), name, rs);
		name = rsname;
	}
	drm_tx t;
	mot_tx m;
	if (drm_tx_init(&t, &p, src, &m)) { fprintf(stderr, "bad parameters\n"); return 1; }
	int frames = mot_tx_init(&m, data, (int)len, name, t.c.nbits, nfix ? fix : NULL, nfix) + extra;
	int fl = drm_tx_frame_len(&t);
	fprintf(stderr, "mode %c, %d carriers, %d MSC cells, %d bytes/frame, %d frames (%.1f s), id %u\n",
	        "ABE"[p.mode], t.c.ncar, t.c.nmux, t.c.nbytes, frames, frames * drm_frame_seconds(&t.c), m.tid);
	short *pcm = malloc(sizeof(short) * (size_t)fl * frames);
	double *buf = malloc(sizeof(double) * fl);
	for (int fr = 0; fr < frames; fr++) {
		drm_tx_frame(&t, buf);
		for (int i = 0; i < fl; i++) pcm[(size_t)fr * fl + i] = (short)(buf[i] * gain);
	}
	if (write_wav16(argv[opt + 1], pcm, (long)fl * frames, DRM_FS)) { perror(argv[opt + 1]); return 1; }
	return 0;
}

/* fskid.h - FSK ID after an SSTV picture (MMSSTV / QSSTV compatible)
 *
 * 2100 Hz lead, 1900 Hz start bit, then 6 bit characters at 45.45 baud
 * (22 ms per bit, least significant bit first, 1900 Hz = 1, 2100 Hz = 0):
 * 0x2A header, the callsign (ASCII - 0x20), 0x01 end, XOR checksum.
 */
#ifndef FSKID_H
#define FSKID_H

typedef void (*fskid_fn)(void *user, const char *call);

typedef struct {
	double fs;
	fskid_fn cb; void *user;
	/* smoothing */
	float *win; int nwin, wpos; double wsum;
	long n;                       /* samples seen */
	/* lead tone */
	long run; double run_sum;
	double lead; long lead_end;   /* last long 2100 Hz tone */
	double offset;                /* measured mistuning (Hz) */
	/* bits */
	int state;                    /* 0 search, 1 bits */
	long t0;                      /* first sample of the first data bit */
	int slot; double acc; int cnt;
	int bits, nbits;
	int stage;                    /* 0 header, 1 text, 2 checksum */
	int chk;
	char text[16]; int nt;
} fskid_rx;

void fskid_init(fskid_rx *r, double fs, fskid_fn cb, void *user);
void fskid_free(fskid_rx *r);
void fskid_process(fskid_rx *r, float freq);        /* instantaneous frequency (Hz) */

int  fskid_valid_call(const char *s);                /* looks like a callsign */

/* tones for "call" at fs (mono, amplitude 0.7); returns malloc'd samples */
float *fskid_tx(const char *call, double fs, long *n);

#endif

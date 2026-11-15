/* drm.h - digital SSTV (HamDRM) common definitions
 *
 * HamDRM is the narrow-band amateur variant of Digital Radio Mondiale used by
 * EasyPal, QSSTV and others: OFDM on a 350 Hz carrier offset, robustness
 * modes A/B/E, 2.3 or 2.5 kHz bandwidth, FAC + MSC (no SDC), the MSC carries
 * one packet-mode data unit per 400 ms frame with an MOT object (the picture).
 *
 * Own implementation written from the DRM standard (ETSI ES 201 980) and the
 * observed HamDRM signal format.
 */
#ifndef DRM_H
#define DRM_H

#include <stdint.h>
#include <complex.h>

#define DRM_FS          48000    /* internal sample rate */
#define DRM_IF          6000     /* virtual IF used to place the bins */
#define DRM_CAR_OFFSET  350.0    /* carrier 0 frequency on air (Hz) */
#define DRM_FRAMES_SF   3        /* frames per super frame */
#define DRM_FAC_CELLS   45
#define DRM_FAC_BITS    48
#define DRM_MAX_SYM     20       /* symbols per frame (max) */
#define DRM_MAX_CAR     64

enum { DRM_MODE_A, DRM_MODE_B, DRM_MODE_E };
enum { DRM_QAM4, DRM_QAM16, DRM_QAM64 };

/* cell types */
enum { CT_MSC = 1, CT_FAC = 2, CT_SCAT = 4, CT_TIME = 8, CT_FREQ = 16, CT_BOOST = 32, CT_DC = 64 };
#define CT_PILOT (CT_SCAT | CT_TIME | CT_FREQ)

typedef struct {
	int mode;          /* DRM_MODE_A/B/E */
	int so;            /* 0 = 2.3 kHz, 1 = 2.5 kHz */
	int qam;           /* DRM_QAM4/16/64 */
	int prot;          /* 0 = high, 1 = normal */
	int ilong;         /* 1 = long interleaver */
	char call[10];     /* service label (callsign), max 9 chars */
} drm_params;

/* everything derived from the parameters */
typedef struct {
	drm_params p;
	int nfft, nguard, nsym, kmin, kmax, ncar, idc;
	int symlen;                                    /* nfft + nguard */
	uint8_t  type[DRM_FRAMES_SF * DRM_MAX_SYM][DRM_MAX_CAR];
	double complex pilot[DRM_FRAMES_SF * DRM_MAX_SYM][DRM_MAX_CAR];
	int nmsc_sym[DRM_FRAMES_SF * DRM_MAX_SYM];     /* MSC cells per symbol (dummies excluded) */
	int nfac_sym[DRM_FRAMES_SF * DRM_MAX_SYM];
	int nmux;                                      /* useful MSC cells per frame */
	int ndummy;                                    /* dummy cells at the end of a super frame */

	/* MSC multilevel coding */
	int levels;
	int rate[3];          /* puncturing pattern index per level */
	int m[3];             /* information bits per level */
	int tailpat[3];       /* tail puncturing pattern index per level */
	int ilv[3];           /* bit interleaver t0 per level (0 = none) */
	int nbits;            /* decoded MSC bits per frame (sum of m) */
	int nbytes;           /* nbits / 8 */
	int paclen;           /* packet data field length (bytes) */
} drm_cfg;

int  drm_setup(drm_cfg *c, const drm_params *p);    /* 0 = ok */
double drm_frame_seconds(const drm_cfg *c);

/* building blocks shared by TX and RX */
extern const int drm_punct[13][10];
extern const int drm_punct_tail[12][6];
void drm_block_table(int *t, int size, int t0);
void drm_energy_dispersal(uint8_t *bits, int n);
int  drm_conv_encode(const uint8_t *in, int nin, int rate, int tailpat, int fac, uint8_t *out);
void drm_punct_table(int *tab, int nin, int rate, int tailpat, int fac);   /* nin + 6 entries */
unsigned drm_crc(const uint8_t *data, int nbytes, int degree);           /* degree 8 or 16 */
void drm_fac_bits(const drm_cfg *c, int frame, uint8_t *bits);           /* 48 bits */
void drm_qam_points(int qam, const double **tab, int *n);
void drm_viterbi(const float *llr, int nin, int rate, int tailpat, int fac, uint8_t *out);

#endif

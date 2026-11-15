/* drm_tx.h - HamDRM OFDM transmitter (bits -> audio samples) */
#ifndef DRM_TX_H
#define DRM_TX_H

#include "drm.h"

/* supplies the next MSC logical frame: c->nbits bits (one per byte) */
typedef void (*drm_src_fn)(void *user, uint8_t *bits);

typedef struct {
	drm_cfg c;
	drm_src_fn src;
	void *user;
	int frame;                   /* frame index in the super frame */
	long nsamp;                  /* samples generated (for the IF shift) */
	/* MSC cell stream */
	double complex *cells;       /* FIFO of interleaved MSC cells */
	int ncells, cap;
	double complex *ilmem;       /* cell interleaver memory: 5 x nmux */
	int ilcur[5];
	int *celltab, *bittab[3];
	double complex *rot;         /* e^{j 2 pi n / N} */
	double complex shift, shift_step;
} drm_tx;

int  drm_tx_init(drm_tx *t, const drm_params *p, drm_src_fn src, void *user);
void drm_tx_free(drm_tx *t);
int  drm_tx_frame_len(const drm_tx *t);                 /* samples per frame at 48 kHz */
void drm_tx_frame(drm_tx *t, double *out);              /* one transmission frame, real, unit ~ 1 */

/* the whole transmission of one file: audio at out_fs (mono, -1..1) */
/* fix/nfix: only these body segments (answer to a repeat request), NULL for all */
float *drm_tx_render(const drm_params *p, const uint8_t *data, int len, const char *name,
                     const int *fix, int nfix, double out_fs, long *nout, double *seconds);
double drm_tx_duration(const drm_params *p, int len);    /* estimate in seconds */

#endif

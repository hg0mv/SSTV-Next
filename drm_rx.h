/* drm_rx.h - HamDRM receiver (audio samples -> MSC packets -> MOT pictures) */
#ifndef DRM_RX_H
#define DRM_RX_H

#include "drm.h"
#include "mot.h"

#define DRM_RX_FS   12000          /* internal processing rate */
#define DRM_RX_RING 16

enum { DRM_RX_SEARCH, DRM_RX_SYNC };

typedef struct {
	double complex y[DRM_MAX_CAR];  /* demodulated cells kmin..kmax */
	double complex h[DRM_MAX_CAR];  /* raw pilot estimates (0 where no pilot) */
	int fs;                         /* symbol index in the frame */
} drm_rx_sym;

typedef struct {
	/* ---- status for the user interface (read only) ---- */
	int state;                      /* DRM_RX_SEARCH / DRM_RX_SYNC */
	int mode, so, qam, prot, ilong; /* valid when params_ok */
	int params_ok;
	char label[10];
	double foff;                    /* frequency offset (Hz) */
	double snr;                     /* dB, from the FAC cells */
	long fac_ok, fac_bad, msc_ok, msc_bad;
	double level;                   /* input level 0..1 */
	double complex scope[64]; int nscope;   /* equalized MSC cells for a constellation view */

	/* ---- internals ---- */
	int infs;
	double rs_pos, rs_step;         /* resampler */
	float *rs_hist; int rs_nh;
	double *fir; int nfir;
	double complex *firbuf; int firpos;
	double ph1, ph2, w1, w2;        /* mixers */
	double complex *z;              /* complex baseband at 12 kHz */
	long zbase; int zlen, zcap;
	long acq_next;                  /* next acquisition attempt (absolute index) */

	drm_cfg c;
	int nfft, nguard, symlen;
	long wpos;                      /* absolute index of the next FFT window */
	int fs_next;
	double phase_ref; long n_ref;   /* frequency correction phase */
	double complex *dft;            /* exp table, nfft */
	drm_rx_sym ring[DRM_RX_RING];
	long nsym_rx;                   /* symbols demodulated since sync */
	double tslope0;                 /* timing reference (phase slope) */
	double tacc;                    /* timing loop */
	int bad_frames, tinit, fr_full, fac_since_sync;
	long frames_abs, rs_count;

	/* frame collection */
	double complex fac_y[DRM_FAC_CELLS]; float fac_w[DRM_FAC_CELLS]; int nfac;
	double complex *fr_y; float *fr_w; int nfr;
	int frame_id;                   /* id of the last frame, -1 unknown */
	/* super frame cell stream */
	double complex *sf_y; float *sf_w; int sf_pos, sf_done;
	long lframe;                    /* logical frame counter */
	/* cell deinterleaver: last 5 logical frames */
	double complex *dl_y[5]; float *dl_w[5]; long dl_id[5];
	int *celltab, *bittab[3];

	mot_rx mot;
} drm_rx;

int  drm_rx_init(drm_rx *r, int input_fs, mot_done_fn done, void *user);
void drm_rx_free(drm_rx *r);
void drm_rx_reset(drm_rx *r);
void drm_rx_process(drm_rx *r, const float *x, int n);

#endif

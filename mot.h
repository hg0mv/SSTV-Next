/* mot.h - MOT object transfer over DRM packet mode (HamDRM picture layer) */
#ifndef MOT_H
#define MOT_H

#include <stdint.h>

#define MOT_HDR_SEG   98     /* header segment size (bytes) */
#define MOT_RUN_IN    24
#define MOT_RUN_OUT   10

typedef struct {
	uint8_t *hdr; int hdrlen;          /* MOT header (core + extension) */
	const uint8_t *body; int bodylen;
	int segsize, nseg, nhseg;
	unsigned tid;                      /* transport id */
	int *list; int nlist, idx;         /* transmission order: <0 header segment -1-k, else body */
	int nbits, nbytes, paclen;         /* per DRM frame */
	/* packet state */
	uint8_t du[4096]; int dulen, dupos;
	int ci_hdr, ci_body, ci_pkt;
} mot_tx;

unsigned mot_transport_id(const char *name);
/* name is the file name with extension ("pic.jpg"); nbits = DRM MSC bits per frame.
   fix/nfix: optional list of body segments to resend (BSR), NULL for a full picture.
   Returns the number of frames of the transmission. */
int  mot_tx_init(mot_tx *m, const uint8_t *data, int len, const char *name, int nbits,
                 const int *fix, int nfix);
void mot_tx_free(mot_tx *m);
void mot_tx_frame(mot_tx *m, uint8_t *bits);   /* nbits bits, one per byte */

/* ---- receiver ---- */
#define MOT_RX_OBJ 4
typedef struct {
	unsigned tid; int used, age;
	uint8_t *hdr; int hdrlen, hdrok;
	long bodysize; char name[96];
	uint8_t **seg; int *seglen; int nseg, last, nhave, segsize;
	int done, flushed, rs_tried;
} mot_obj;

/* partial = 1: the object is incomplete (signal lost / a new object started); data is the
   contiguous beginning of the body */
typedef void (*mot_done_fn)(void *user, const char *name, const uint8_t *data, long len, int partial);

typedef struct {
	int paclen;
	uint8_t du[4096]; int dulen, duok;
	mot_obj obj[MOT_RX_OBJ];
	int cur;                       /* object that got the latest segment, -1 none */
	long pk_ok, pk_bad, dg_ok, dg_bad;
	mot_done_fn done; void *user;
	int clock;
	int rs_fixed;                  /* bytes repaired by Reed-Solomon in the last object */
} mot_rx;

void mot_rx_init(mot_rx *r, mot_done_fn done, void *user);
void mot_rx_free(mot_rx *r);
void mot_rx_packet(mot_rx *r, const uint8_t *pk, int nbytes);   /* nbytes = paclen + 3 */
/* progress of the current object: segments received / total (total 0 = unknown) */
void mot_rx_progress(const mot_rx *r, int *have, int *total, const char **name);
/* list of missing body segments of the current object (for a BSR request) */
int  mot_rx_missing(const mot_rx *r, int *list, int max);
/* contiguous beginning of the current object's body (malloc'd), 0 if nothing */
long mot_rx_prefix(const mot_rx *r, uint8_t **data);
/* report the current object as partial if it is incomplete (call when the signal is lost) */
void mot_rx_flush(mot_rx *r);
/* repeat request text ("bsr.bin" content) for the current incomplete object, 0 if none */
int  mot_rx_bsr(const mot_rx *r, char *out, int max);
/* parse a received bsr.bin: transport id and the requested segments; -1 if not a request */
int  mot_bsr_parse(const char *txt, long len, unsigned *tid, int *list, int max);

#endif

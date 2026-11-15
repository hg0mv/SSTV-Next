/* tx.h - SSTV encoder (image -> audio) */
#ifndef TX_H
#define TX_H

#include "sstv.h"
#include "image.h"

typedef struct {
    double fs;            /* output sample rate */
    double clock_ppm;     /* simulated TX sound card clock error (test) */
    double freq_offset;   /* simulated mistuning in Hz (test) */
    double snr_db;        /* add white noise, SNR in 3 kHz bandwidth; <= -100: none */
    double lead_ms;       /* silence before the VIS header */
    double tail_ms;       /* silence after the image */
    unsigned seed;
} sstv_tx_opts;

void   sstv_tx_defaults(sstv_tx_opts *o);

/* The image is resized to the mode's resolution. Returns malloc'd samples. */
float *sstv_encode(const sstv_mode *m, const image *img, const sstv_tx_opts *o, long *nout);

#endif

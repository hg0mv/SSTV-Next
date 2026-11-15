/* tones.h - narrow-band tone powers for weak-signal detection
 *
 * A sliding 20 ms DFT over 1000..2400 Hz (25 Hz steps), one row of powers
 * per millisecond. The FM discriminator breaks down below ~8 dB SNR (clicks),
 * while a 20 ms tone measurement still works far below 0 dB, so the VIS
 * header is looked for in these rows too.
 */
#ifndef TONES_H
#define TONES_H

#define TS_NB  57          /* bins */
#define TS_F0  1000.0      /* first bin, Hz */
#define TS_DF  25.0        /* bin spacing, Hz */
#define TS_WIN 0.020       /* window, s */

typedef struct tonespec tonespec;

/* delay: the row times are given in the index of the FM stream, which runs
   'delay' samples behind the audio (fmdemod_delay) */
tonespec *ts_new(double fs, int delay);
void      ts_free(tonespec *t);
/* one audio sample; when a new row is ready in row[TS_NB] its index j is returned
   (-1: none). Row j is centred on FM-stream sample j * fs / 1000; rows start at the
   first full window, so the first index returned is not 0. */
long      ts_process(tonespec *t, float x, float *row);

#endif

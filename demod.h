/* demod.h - FM discriminator for SSTV (instantaneous frequency in Hz)
 *
 * Streaming: fmdemod_process() takes one audio sample and returns the
 * frequency estimate, delayed by fmdemod_delay() samples. This same object
 * will be used by the live (ALSA) receiver.
 */
#ifndef DEMOD_H
#define DEMOD_H

#define FM_CENTER 1750.0     /* mixing frequency: freq = FM_CENTER + angle * fs / 2pi */

typedef struct fmdemod fmdemod;

fmdemod *fmdemod_new(double fs);
void     fmdemod_free(fmdemod *d);
float    fmdemod_process(fmdemod *d, float x);
int      fmdemod_delay(const fmdemod *d);
/* filtered complex baseband (around FM_CENTER) of the sample just processed */
void     fmdemod_baseband(const fmdemod *d, float *re, float *im);

/* Offline helper: whole buffer -> frequency array, delay compensated */
float   *fm_demod_buffer(const float *x, long n, double fs);
/* the same, also giving the complex baseband (2 floats per sample, malloc'd) */
float   *fm_demod_buffer_ex(const float *x, long n, double fs, float **baseband);

#endif

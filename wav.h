/* wav.h - minimal WAV I/O (mono float samples, -1..1) */
#ifndef WAV_H
#define WAV_H

/* Reads PCM 8/16/24/32-bit or float32 WAV, any channel count (mixed to mono).
 * Returns malloc'd buffer, NULL on error. */
float *wav_read(const char *path, double *fs, long *n);

/* Writes 16-bit mono PCM. Returns 0 on success. */
int wav_write(const char *path, const float *x, long n, int fs);

#endif

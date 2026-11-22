/* audio.h - ALSA capture/playback (capture thread taken over from DigiPan Next)
 *
 * Capture runs in its own thread and hands every block to a callback.
 * A WAV file can stand in for the sound card (testing without a radio).
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>

typedef void (*audio_cb)(const float *x, int n, void *user);

int   audio_list(char devs[][128], int max);       /* "default", "plughw:N,0" ... */

/* Input channel: 0 both mixed (mono), 1 left, 2 right - e.g. two receivers on the two
   channels of one sound card, or an SDR with two outputs. Takes effect at the next
   audio_start / audio_start_file. */
void  audio_set_channel(int ch);

/* Starts (or restarts) capture from an ALSA device. status gets a message. */
int   audio_start(const char *device, unsigned rate, audio_cb cb, void *user,
                  char *status, size_t ns);
/* Feeds a WAV file instead of a sound card, at 'speed' x real time. */
int   audio_start_file(const char *path, unsigned rate, double speed, audio_cb cb, void *user,
                       char *status, size_t ns);
void  audio_stop(void);
float audio_level(void);                           /* RMS of the last block, 0..1 */
int   audio_running(void);

/* Playback (TX). If wav_path is set, writes a WAV file instead of playing. */
typedef struct audio_out audio_out;
audio_out *audio_out_open(const char *device, const char *wav_path, unsigned rate, char *err, size_t ne);
int        audio_out_write(audio_out *o, const float *x, int n);   /* 0 = ok */
void       audio_out_close(audio_out *o, int drain);

#endif

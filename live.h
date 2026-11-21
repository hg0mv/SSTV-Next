/* live.h - real-time SSTV receiver engine
 *
 * live_push() is called from the audio capture thread; everything else
 * (FM demodulation, VIS search, sync tracking, image building, saving) runs
 * in the engine's own thread. The UI polls live_snapshot().
 */
#ifndef LIVE_H
#define LIVE_H

#include <stddef.h>
#include "sstv.h"
#include "image.h"

typedef enum { LIVE_IDLE = 0, LIVE_RX = 1 } live_state;

typedef struct {
    live_state state;
    const sstv_mode *mode;      /* image being received (or last one) */
    int    vis;                 /* VIS code, -1: started from sync pulses */
    double offset;              /* measured mistuning, Hz */
    int    lines, lines_total;  /* progress in transmitted lines */
    int    syncs;               /* sync pulses used for slant correction */
    double clock_ppm;
    double snr;                 /* signal/noise in 2.5 kHz, dB (NAN: unknown) */
    int    combined;            /* the picture shown is combined from this many receptions (0: no) */
    char   msg[120];            /* last event for the status line */
    /* digital (HamDRM) receiver */
    int    drm_sync;            /* 1: locked to a digital signal */
    char   drm_info[40];        /* "Mode B  16-QAM  Norm  2.5 kHz" */
    char   drm_call[10];        /* sender's label (callsign) */
    double drm_snr, drm_foff;
    int    drm_have, drm_total; /* picture segments received / total */
    char   drm_name[64];        /* file being received */
    unsigned drm_new;           /* counts finished digital pictures */
    int    drm_can_bsr;         /* the last digital picture is incomplete: a repeat request is possible */
    unsigned bsr_new;           /* counts repeat requests (BSR) received from other stations */
    /* callsign heard from the other station (FSK ID after an analog picture, or the DRM label) */
    char   heard_call[12];
    int    heard_src;           /* 1 FSK ID, 2 digital */
    unsigned heard_new;
} live_status;

typedef struct live live;

live *live_new(double fs, const char *save_dir);
void  live_free(live *l);
void  live_push(live *l, const float *x, int n);      /* capture thread */
void  live_mute(live *l, int on);                     /* while transmitting */
void  live_set_nr(live *l, int on);                   /* QRM speck removal (default on) */
void  live_set_force(live *l, const sstv_mode *m);    /* NULL: VIS only */
void  live_set_dir(live *l, const char *save_dir);
void  live_stop(live *l);                             /* finish + save now */
void  live_reset(live *l);                            /* new audio stream */

/* Copies the current image if it changed since *gen. Returns 1 if *img was set
 * (caller frees it). The status is always filled in. */
int   live_snapshot(live *l, unsigned *gen, image **img, live_status *st);
/* Pops the path of a newly saved image. Returns 1 if there was one. */
int   live_take_saved(live *l, char *path, size_t n);
/* text of a repeat request (bsr.bin) for the last incomplete digital picture; 0 if none */
int   live_bsr_request(live *l, char *out, int max);
/* the last repeat request received from another station: transport id, segments, sender */
int   live_last_bsr(live *l, unsigned *tid, int *list, int max, char *from, size_t fn);

#endif

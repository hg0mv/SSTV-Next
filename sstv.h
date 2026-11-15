/* sstv.h - SSTV mode definitions (common to TX and RX) */
#ifndef SSTV_H
#define SSTV_H

#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SSTV_F_SYNC    1200.0
#define SSTV_F_BLACK   1500.0
#define SSTV_F_WHITE   2300.0
#define SSTV_F_LEADER  1900.0

typedef enum { SEG_TONE, SEG_SYNC, SEG_SCAN } sstv_seg_type;
typedef enum { CH_R = 0, CH_G = 1, CH_B = 2,   /* RGB modes */
               CH_Y, CH_CR, CH_CB } sstv_channel;  /* YCrCb modes (PD) */

/* One segment of a scan line: a fixed tone, the sync pulse, or a colour scan */
typedef struct {
    sstv_seg_type type;
    double        ms;
    double        freq;     /* SEG_TONE / SEG_SYNC */
    sstv_channel  ch;       /* SEG_SCAN */
    int           row;      /* SEG_SCAN: which image row of the line (Y in PD) */
} sstv_seg;

#define SSTV_MAX_SEGS 16

typedef struct {
    const char *name;        /* "Scottie S2" */
    const char *id;          /* "s2" (command line) */
    int         vis;         /* VIS code */
    int         width, height;   /* image size in pixels */
    int         rows;            /* image rows per transmitted line (PD: 2) */
    double      pre_sync_ms; /* extra sync pulse before line 0 (Scottie) */
    int         nseg;
    sstv_seg    seg[SSTV_MAX_SEGS];
} sstv_mode;

const sstv_mode *sstv_mode_by_vis(int vis);
const sstv_mode *sstv_mode_by_id(const char *id);
void   sstv_list_modes(FILE *fp);
double sstv_line_ms(const sstv_mode *m);
double sstv_seg_start_ms(const sstv_mode *m, int idx);   /* from line start */
int    sstv_sync_seg(const sstv_mode *m);                /* index of sync segment */
int    sstv_nlines(const sstv_mode *m);                  /* transmitted lines */
int    sstv_sync_count(const sstv_mode *m);              /* sync pulses per line (Robot 36: 2) */
int    sstv_mode_count(void);
const sstv_mode *sstv_mode_at(int i);
int    sstv_is_ycc(const sstv_mode *m);

/* YCrCb (full range, as used by PD modes) <-> RGB */
void   sstv_ycc_to_rgb(double y, double cr, double cb, unsigned char rgb[3]);
void   sstv_rgb_to_ycc(const unsigned char rgb[3], double *y, double *cr, double *cb);

#endif

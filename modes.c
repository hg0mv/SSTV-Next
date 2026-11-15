/* modes.c - SSTV mode table
 *
 * Every line is described as a list of segments, so the encoder and the
 * decoder need no mode-specific code. Adding a new mode of an existing
 * family means adding one line to this table.
 *
 * Two-row modes (PD, Robot 36) are described as one "line" that carries two
 * image rows; the colour difference scans are shared by both rows.
 */
#include <string.h>
#include "sstv.h"

#define TONE(ms, f)        { SEG_TONE, (ms), (f),         CH_R, 0 }
#define SYNC(ms)           { SEG_SYNC, (ms), SSTV_F_SYNC, CH_R, 0 }
#define SCAN(c, ms)        { SEG_SCAN, (ms), 0.0,         (c),  0 }
#define SCANR(c, ms, row)  { SEG_SCAN, (ms), 0.0,         (c),  (row) }

/* Scottie: sep, G, sep, B, sync, porch, R  (+ one extra sync before line 0) */
#define SCOTTIE(t) 7, { TONE(1.5, 1500.0), SCAN(CH_G, t), TONE(1.5, 1500.0), \
                        SCAN(CH_B, t), SYNC(9.0), TONE(1.5, 1500.0), SCAN(CH_R, t) }

/* Martin: sync, porch, G, sep, B, sep, R, sep */
#define MARTIN(t)  8, { SYNC(4.862), TONE(0.572, 1500.0), SCAN(CH_G, t), \
                        TONE(0.572, 1500.0), SCAN(CH_B, t), TONE(0.572, 1500.0), \
                        SCAN(CH_R, t), TONE(0.572, 1500.0) }

/* Wraase SC2: sync, porch, R, G, B */
#define WRAASE(t)  5, { SYNC(5.5225), TONE(0.5, 1500.0), SCAN(CH_R, t), SCAN(CH_G, t), SCAN(CH_B, t) }

/* Pasokon: sync, porch, R, porch, G, porch, B, porch */
#define PASOKON(s, p, t) 8, { SYNC(s), TONE(p, 1500.0), SCAN(CH_R, t), TONE(p, 1500.0), \
                              SCAN(CH_G, t), TONE(p, 1500.0), SCAN(CH_B, t), TONE(p, 1500.0) }

/* PD: sync, porch, Y(row 0), R-Y, B-Y, Y(row 1) */
#define PD(t)      6, { SYNC(20.0), TONE(2.080, 1500.0), SCANR(CH_Y, t, 0), \
                        SCAN(CH_CR, t), SCAN(CH_CB, t), SCANR(CH_Y, t, 1) }

/* Robot 72: sync, porch, Y, sep 1500 + porch 1900, R-Y, sep 2300 + porch 1900, B-Y */
#define ROBOT72    9, { SYNC(9.0), TONE(3.0, 1500.0), SCAN(CH_Y, 138.0), \
                        TONE(4.5, 1500.0), TONE(1.5, 1900.0), SCAN(CH_CR, 69.0), \
                        TONE(4.5, 2300.0), TONE(1.5, 1900.0), SCAN(CH_CB, 69.0) }

/* Robot 36: two 150 ms lines; the even one carries R-Y (separator 1500 Hz),
 * the odd one B-Y (separator 2300 Hz) */
#define ROBOT36   12, { SYNC(9.0), TONE(3.0, 1500.0), SCANR(CH_Y, 88.0, 0), \
                        TONE(4.5, 1500.0), TONE(1.5, 1900.0), SCAN(CH_CR, 44.0), \
                        SYNC(9.0), TONE(3.0, 1500.0), SCANR(CH_Y, 88.0, 1), \
                        TONE(4.5, 2300.0), TONE(1.5, 1900.0), SCAN(CH_CB, 44.0) }

/* FAX480: black and white */
#define FAX480     2, { SYNC(5.12), SCAN(CH_Y, 262.144) }

static const sstv_mode modes[] = {
    /* name          id      VIS  w    h   rows pre   segments */
    { "Robot 36",    "r36",    8, 320, 240, 2, 0.0, ROBOT36          },
    { "Robot 72",    "r72",   12, 320, 240, 1, 0.0, ROBOT72          },
    { "Scottie S1",  "s1",    60, 320, 256, 1, 9.0, SCOTTIE(138.240) },
    { "Scottie S2",  "s2",    56, 320, 256, 1, 9.0, SCOTTIE(88.064)  },
    { "Scottie S3",  "s3",    52, 320, 128, 1, 9.0, SCOTTIE(138.240) },
    { "Scottie S4",  "s4",    48, 320, 128, 1, 9.0, SCOTTIE(88.064)  },
    { "Scottie DX",  "sdx",   76, 320, 256, 1, 9.0, SCOTTIE(345.600) },
    { "Martin M1",   "m1",    44, 320, 256, 1, 0.0, MARTIN(146.432)  },
    { "Martin M2",   "m2",    40, 320, 256, 1, 0.0, MARTIN(73.216)   },
    { "Martin M3",   "m3",    36, 320, 128, 1, 0.0, MARTIN(146.432)  },
    { "Martin M4",   "m4",    32, 320, 128, 1, 0.0, MARTIN(73.216)   },
    { "PD50",        "pd50",  93, 320, 256, 2, 0.0, PD(91.520)       },
    { "PD90",        "pd90",  99, 320, 256, 2, 0.0, PD(170.240)      },
    { "PD120",       "pd120", 95, 640, 496, 2, 0.0, PD(121.600)      },
    { "PD160",       "pd160", 98, 512, 400, 2, 0.0, PD(195.584)      },
    { "PD180",       "pd180", 96, 640, 496, 2, 0.0, PD(183.040)      },
    { "PD240",       "pd240", 97, 640, 496, 2, 0.0, PD(244.480)      },
    { "PD290",       "pd290", 94, 800, 616, 2, 0.0, PD(228.800)      },
    { "Wraase SC2-60", "sc60",  59, 320, 256, 1, 0.0, WRAASE(78.1207) },
    { "Wraase SC2-120","sc120", 63, 320, 256, 1, 0.0, WRAASE(156.5025)},
    { "Wraase SC2-180","sc180", 55, 320, 256, 1, 0.0, WRAASE(235.000) },
    { "Pasokon P3",  "p3",   113, 640, 496, 1, 0.0, PASOKON(5.208, 1.042, 133.333) },
    { "Pasokon P5",  "p5",   114, 640, 496, 1, 0.0, PASOKON(7.813, 1.563, 200.000) },
    { "Pasokon P7",  "p7",   115, 640, 496, 1, 0.0, PASOKON(10.417, 2.083, 266.667) },
    { "FAX480",      "fax480", 85, 512, 480, 1, 0.0, FAX480          },
};
#define NMODES (int)(sizeof modes / sizeof modes[0])

int sstv_mode_count(void) { return NMODES; }
const sstv_mode *sstv_mode_at(int i) { return i >= 0 && i < NMODES ? &modes[i] : NULL; }

const sstv_mode *sstv_mode_by_vis(int vis)
{
    for (int i = 0; i < NMODES; i++)
        if (modes[i].vis == vis) return &modes[i];
    return NULL;
}

const sstv_mode *sstv_mode_by_id(const char *id)
{
    for (int i = 0; i < NMODES; i++)
        if (!strcmp(modes[i].id, id)) return &modes[i];
    return NULL;
}

void sstv_list_modes(FILE *fp)
{
    for (int i = 0; i < NMODES; i++)
        fprintf(fp, "  %-7s %-15s VIS %3d  %dx%d  %.1f s\n",
                modes[i].id, modes[i].name, modes[i].vis,
                modes[i].width, modes[i].height,
                (modes[i].pre_sync_ms + sstv_nlines(&modes[i]) * sstv_line_ms(&modes[i])) / 1000.0);
}

double sstv_line_ms(const sstv_mode *m)
{
    double t = 0;
    for (int i = 0; i < m->nseg; i++) t += m->seg[i].ms;
    return t;
}

double sstv_seg_start_ms(const sstv_mode *m, int idx)
{
    double t = 0;
    for (int i = 0; i < idx; i++) t += m->seg[i].ms;
    return t;
}

int sstv_sync_seg(const sstv_mode *m)
{
    for (int i = 0; i < m->nseg; i++)
        if (m->seg[i].type == SEG_SYNC) return i;
    return -1;
}

int sstv_sync_count(const sstv_mode *m)
{
    int n = 0;
    for (int i = 0; i < m->nseg; i++) n += m->seg[i].type == SEG_SYNC;
    return n;
}

int sstv_nlines(const sstv_mode *m) { return m->height / m->rows; }

int sstv_is_ycc(const sstv_mode *m)
{
    for (int i = 0; i < m->nseg; i++)
        if (m->seg[i].type == SEG_SCAN && m->seg[i].ch >= CH_Y) return 1;
    return 0;
}

static unsigned char clamp8(double v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (unsigned char)(v + 0.5);
}

/* Full-range YCrCb, chroma centred on 128 (same as MMSSTV / slowrx) */
void sstv_ycc_to_rgb(double y, double cr, double cb, unsigned char rgb[3])
{
    rgb[0] = clamp8(y + 1.402 * (cr - 128));
    rgb[1] = clamp8(y - 0.714 * (cr - 128) - 0.344 * (cb - 128));
    rgb[2] = clamp8(y + 1.772 * (cb - 128));
}

void sstv_rgb_to_ycc(const unsigned char rgb[3], double *y, double *cr, double *cb)
{
    double Y = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
    *y  = Y;
    *cr = 128 + (rgb[0] - Y) / 1.402;
    *cb = 128 + (rgb[2] - Y) / 1.772;
}

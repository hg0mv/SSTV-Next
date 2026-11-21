/* combine.h - several receptions of the same picture -> one cleaner picture
 *
 * ISS events and SSTV nets send the same picture again and again, and every copy
 * is damaged somewhere else (noise, QSB, QRM). The copies are recognised, aligned
 * (a picture started without VIS, or a partial one, sits a few lines or pixels
 * away) and merged row by row, each row weighted by how noisy it is in that copy,
 * with specks (FM clicks) rejected pixel by pixel.
 */
#ifndef COMBINE_H
#define COMBINE_H

#include "image.h"

/* Is b the same picture as a? Then 1, with b's offset: b(x, y) shows a(x + dx, y + dy).
 * score: agreement 0..1. Rows that were not received (all black) are ignored. */
int    cmb_match(const image *a, const image *b, int *dx, int *dy, double *score);

/* Merges n aligned copies into the frame of ims[0]: ims[i](x, y) shows the
 * picture at (x + dx[i], y + dy[i]); dx[0] = dy[0] = 0. */
image *cmb_merge(image *const *ims, const int *dx, const int *dy, int n);
/* the same with a weight for every row of every copy (rowq[i] NULL: estimated from the
 * picture), e.g. from the signal/noise of each line's sync pulse */
image *cmb_merge_q(image *const *ims, const int *dx, const int *dy, const float *const *rowq, int n);

/* per-line signal/noise (sstv_rxinfo.lq) <-> compact text for the PNG data ("q=") */
void   cmb_q_encode(const unsigned char *lq, int n, char *out, size_t size);
/* row weights from that text (rows_per_line 0: from the length); returns the number of lines with a value */
int    cmb_q_rows(const char *q, int rows, int rows_per_line, float *w);

/* number of received (not all black) rows */
int    cmb_rows(const image *im);

#endif

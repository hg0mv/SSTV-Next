/* sstv_combine - merge several receptions of the same SSTV picture
 *
 *   sstv_combine [-o out.png] a.png b.png [c.png ...]
 *
 * The copy with the most received rows is the reference; every other copy that
 * shows the same picture is aligned to it and merged (combine.h). Copies of a
 * different picture are left out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "image.h"
#include "imgio.h"
#include "combine.h"

int main(int argc, char **argv)
{
    const char *out = "combined.png";
    int o;
    while ((o = getopt(argc, argv, "o:h")) != -1) {
        if (o == 'o') out = optarg;
        else {
            fprintf(stderr, "usage: %s [-o out.png] picture.png picture.png [...]\n", argv[0]);
            return o == 'h' ? 0 : 1;
        }
    }
    int n = argc - optind;
    if (n < 2) { fprintf(stderr, "give at least two pictures (-h for help)\n"); return 1; }
    image **im = calloc(n, sizeof *im);
    int ref = -1, best = -1;
    for (int i = 0; i < n; i++) {
        im[i] = img_load(argv[optind + i]);
        if (!im[i]) { fprintf(stderr, "%s: cannot read\n", argv[optind + i]); continue; }
        int r = cmb_rows(im[i]);
        if (r > best) { best = r; ref = i; }
    }
    if (ref < 0) return 1;
    /* row weights from the reception data (per-line signal/noise), if the files have it */
    float **rq = calloc(n, sizeof *rq);
    for (int i = 0; i < n; i++) {
        char meta[2048], q[1100];
        if (!im[i] || !img_png_meta(argv[optind + i], meta, sizeof meta) || !img_meta_get(meta, "q", q, sizeof q)) continue;
        rq[i] = malloc(im[i]->h * sizeof(float));
        if (!cmb_q_rows(q, im[i]->h, 0, rq[i])) { free(rq[i]); rq[i] = NULL; }
    }
    const float **uq = calloc(n, sizeof *uq);
    image **use = calloc(n, sizeof *use);
    int *dx = calloc(n, sizeof(int)), *dy = calloc(n, sizeof(int)), m = 0;
    uq[m] = rq[ref];
    use[m++] = im[ref];
    printf("%s: reference (%d rows)\n", argv[optind + ref], best);
    for (int i = 0; i < n; i++) {
        if (i == ref || !im[i]) continue;
        int x, y;
        double sc;
        if (cmb_match(im[ref], im[i], &x, &y, &sc)) {
            use[m] = im[i]; uq[m] = rq[i]; dx[m] = x; dy[m] = y; m++;
            printf("%s: same picture, offset %+d px %+d lines (match %.2f)\n", argv[optind + i], x, y, sc);
        } else {
            printf("%s: not the same picture - left out\n", argv[optind + i]);
        }
    }
    if (m < 2) { printf("nothing to combine\n"); return 2; }
    int haveq = 0;
    for (int i = 0; i < m; i++) haveq += uq[i] != NULL;
    image *r = cmb_merge_q(use, dx, dy, haveq == m ? uq : NULL, m);
    char meta[64];
    snprintf(meta, sizeof meta, "combined=%d", m);
    int err = img_save_png_meta(out, r, meta);
    printf("%d copies combined -> %s\n", m, err ? "(cannot save)" : out);
    image_free(r);
    for (int i = 0; i < n; i++) image_free(im[i]);
    for (int i = 0; i < n; i++) free(rq[i]);
    free(im); free(use); free(dx); free(dy); free(rq); free(uq);
    return err ? 1 : 0;
}

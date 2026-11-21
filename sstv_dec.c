/* sstv_dec.c - offline decoder: WAV -> PPM image(s)
 *
 * Decodes every image found in the recording. Without VIS (-m given) it
 * falls back to finding the periodic sync pulses of the chosen mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "sstv.h"
#include "image.h"
#include "wav.h"
#include "demod.h"
#include "rx.h"
#include "tones.h"
#include "imgio.h"
#include "combine.h"

static void usage(void)
{
    fprintf(stderr,
        "Usage: sstv_dec [options] recording.wav\n"
        "  -m mode    force a mode (reception without VIS)\n"
        "  -o prefix  output file name prefix (default: sstv) -> sstv_1.ppm ...\n"
        "  -r         raw picture: QRM speck filter off\n"
        "  -P         PNG output with the reception data (for sstv_combine)\n"

        "Modes:\n");
    sstv_list_modes(stderr);
}

int main(int argc, char **argv)
{
    const sstv_mode *forced = NULL;
    const char *prefix = "sstv";
    int c, flags = SSTV_NR, png = 0;
    while ((c = getopt(argc, argv, "m:o:rPh")) != -1) {
        switch (c) {
        case 'm': forced = sstv_mode_by_id(optarg);
                  if (!forced) { fprintf(stderr, "Unknown mode: %s\n", optarg); usage(); return 1; }
                  break;
        case 'o': prefix = optarg; break;
        case 'r': flags &= ~SSTV_NR; break;
        case 'P': png = 1; break;
        default:  usage(); return 1;
        }
    }
    if (argc - optind != 1) { usage(); return 1; }

    double fs;
    long n;
    float *x = wav_read(argv[optind], &fs, &n);
    if (!x) return 1;
    printf("%s: %.0f Hz, %.1f s\n", argv[optind], fs, n / fs);

    float *y;
    float *f = fm_demod_buffer_ex(x, n, fs, &y);
    sstv_fbuf fb;
    if (sstv_fbuf_init(&fb, f, n, fs)) { fprintf(stderr, "out of memory\n"); return 1; }
    /* weak-signal helpers: baseband for the sync matched filter, tone rows for the VIS */
    long nrow = (long)(n * 1000.0 / fs) + 2, first = -1;
    float *T = calloc((size_t)nrow * TS_NB, sizeof(float));
    tonespec *ts = ts_new(fs, 0);
    float row[TS_NB];
    for (long i = 0; i < n; i++) {
        long j = ts_process(ts, x[i], row);
        if (j < 0 || j >= nrow) continue;
        if (first < 0) first = j;
        memcpy(T + (size_t)j * TS_NB, row, sizeof row);
        fb.tn = j + 1;
    }
    ts_free(ts);
    fb.y = y;
    fb.T = T;
    fb.t0f = 0;
    fb.tstep = fs / 1000.0;

    long from = 0;
    int count = 0;
    for (;;) {
        sstv_vis v;
        int have_vis = sstv_vis_find(&fb, from, &v);
        double t0 = 0, off = NAN, ts;
        const sstv_mode *m = NULL;

        const sstv_mode *am = NULL;
        if (forced && sstv_sync_find(&fb, forced, from, 0, &ts) &&
            (!have_vis || ts < v.img_start - fs)) {
            m = forced;
            t0 = ts;
            printf("\n[%6.1f s] no VIS, from the sync pulses: %s\n", t0 / fs, m->name);
        } else if (!forced && sstv_auto_find(&fb, from, 0, &am, &ts) &&
                   (!have_vis || ts < v.img_start - fs)) {
            m = am;
            t0 = ts;
            printf("\n[%6.1f s] no VIS, recognised from the sync pulses: %s\n", t0 / fs, m->name);
        } else if (have_vis) {
            const sstv_mode *chk = NULL;
            if (forced) m = forced;
            else if (v.complete) {
                m = sstv_mode_by_vis(v.code);
                /* two flipped bits pass the parity: confirm with the sync pulses */
                if (sstv_vis_resolve(&fb, &v, 1, &chk) == 1 && chk != m) {
                    printf("\n[%6.1f s] VIS %d (%s) damaged by QRM, by the sync pulses: %s\n",
                           v.img_start / fs, v.code, m->name, chk->name);
                    m = chk;
                    v.complete = 0;
                    v.code = -3;
                }
            }
            else if (sstv_vis_resolve(&fb, &v, 1, &m) != 1) m = NULL;
            if (v.complete)
                printf("\n[%6.1f s] VIS %d -> %s, offset %+.0f Hz\n",
                       v.img_start / fs, v.code, m ? m->name : "unknown mode", v.offset);
            else if (v.code != -3)
                printf("\n[%6.1f s] damaged VIS (QRM) -> %s, offset %+.0f Hz\n",
                       v.img_start / fs, m ? m->name : "not identified", v.offset);
            if (!m) { from = (long)v.img_start; continue; }
            t0 = v.img_start;
            off = v.offset;
        } else {
            break;
        }

        sstv_rxinfo info;
        image *img = sstv_decode_ex(&fb, m, t0, off, flags, &info);
        char name[512];
        snprintf(name, sizeof name, "%s_%d.%s", prefix, ++count, png ? "png" : "ppm");
        if (png) {
            char meta[1400], q[1100] = "";
            cmb_q_encode(info.lq, info.nlq, q, sizeof q);
            snprintf(meta, sizeof meta, "mode=%s;sync=%d;offset=%.0f;ppm=%.0f;snr=%.1f;q=%s",
                     m->id, info.syncs, info.offset, info.clock_ppm, isnan(info.snr) ? -99.0 : info.snr, q);
            img_save_png_meta(name, img, meta);
        } else ppm_write(name, img);
        image_free(img);
        char sn[24] = "";
        if (!isnan(info.snr)) snprintf(sn, sizeof sn, ", SNR %.0f dB", info.snr);
        printf("  sync: %d/%d lines, line %.3f ms, clock error %+.0f ppm, offset %+.0f Hz%s -> %s\n",
               info.syncs, sstv_nlines(m), info.line_ms, info.clock_ppm, info.offset, sn, name);
        from = (long)info.end;
        if (from >= n) break;
    }
    if (!count) printf("No SSTV picture found.\n");

    sstv_fbuf_free(&fb);
    free(f);
    free(y);
    free(T);
    free(x);
    return 0;
}

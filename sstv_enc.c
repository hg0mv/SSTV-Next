/* sstv_enc.c - test encoder: PPM image -> SSTV WAV */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "sstv.h"
#include "image.h"
#include "wav.h"
#include "tx.h"

static void usage(void)
{
    fprintf(stderr,
        "Usage: sstv_enc [options] picture.ppm output.wav\n"
        "  -m mode    mode (default: s2)\n"
        "  -r Hz      sample rate (11025)\n"
        "  -p ppm     simulated transmitter clock error (slant test)\n"
        "  -f Hz      simulated mistuning\n"
        "  -n dB      white noise, SNR in 3 kHz\n"
        "Modes:\n");
    sstv_list_modes(stderr);
}

int main(int argc, char **argv)
{
    sstv_tx_opts o;
    sstv_tx_defaults(&o);
    const sstv_mode *m = sstv_mode_by_id("s2");
    int c;
    while ((c = getopt(argc, argv, "m:r:p:f:n:h")) != -1) {
        switch (c) {
        case 'm': m = sstv_mode_by_id(optarg);
                  if (!m) { fprintf(stderr, "Unknown mode: %s\n", optarg); usage(); return 1; }
                  break;
        case 'r': o.fs = atof(optarg); break;
        case 'p': o.clock_ppm = atof(optarg); break;
        case 'f': o.freq_offset = atof(optarg); break;
        case 'n': o.snr_db = atof(optarg); break;
        default:  usage(); return 1;
        }
    }
    if (argc - optind != 2) { usage(); return 1; }

    image *img = ppm_read(argv[optind]);
    if (!img) return 1;
    long n;
    float *x = sstv_encode(m, img, &o, &n);
    if (wav_write(argv[optind + 1], x, n, (int)o.fs)) return 1;
    printf("%s, %.0f Hz, %.1f s -> %s\n", m->name, o.fs, n / o.fs, argv[optind + 1]);
    free(x);
    image_free(img);
    return 0;
}

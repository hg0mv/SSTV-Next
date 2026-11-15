/* wav.c - minimal WAV I/O */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wav.h"

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static double sample_at(const unsigned char *p, int fmt, int bits)
{
    if (fmt == 3 && bits == 32) { float f; memcpy(&f, p, 4); return f; }
    switch (bits) {
    case 8:  return (p[0] - 128) / 128.0;
    case 16: return (int16_t)rd16(p) / 32768.0;
    case 24: { int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
               return v / 2147483648.0; }
    case 32: return (int32_t)rd32(p) / 2147483648.0;
    }
    return 0;
}

float *wav_read(const char *path, double *fs, long *n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }

    unsigned char hdr[12], ck[8];
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV file\n", path);
        fclose(fp);
        return NULL;
    }

    int fmt = 0, ch = 0, bits = 0, have_fmt = 0;
    uint32_t rate = 0;
    float *out = NULL;

    while (fread(ck, 1, 8, fp) == 8) {
        uint32_t sz = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char b[40] = {0};
            uint32_t take = sz < 40 ? sz : 40;
            if (fread(b, 1, take, fp) != take) break;
            if (sz > take) fseek(fp, (long)(sz - take), SEEK_CUR);
            fmt  = rd16(b);
            ch   = rd16(b + 2);
            rate = rd32(b + 4);
            bits = rd16(b + 14);
            if (fmt == 0xFFFE && take >= 26) fmt = rd16(b + 24);  /* WAVE_FORMAT_EXTENSIBLE */
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            if (!have_fmt || (fmt != 1 && fmt != 3) || ch < 1 ||
                (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
                fprintf(stderr, "%s: unsupported WAV format (fmt=%d, %d bit)\n", path, fmt, bits);
                break;
            }
            /* read the data chunk (or to EOF if the size is bogus) */
            size_t cap = (sz && sz != 0xFFFFFFFFu) ? sz : 1 << 20, len = 0;
            unsigned char *raw = malloc(cap);
            size_t r;
            while ((r = fread(raw + len, 1, cap - len, fp)) > 0) {
                len += r;
                if (sz && sz != 0xFFFFFFFFu && len >= sz) break;
                if (len == cap) { cap *= 2; raw = realloc(raw, cap); }
            }
            int bps = bits / 8, frame = bps * ch;
            long frames = (long)(len / (size_t)frame);
            out = malloc((size_t)(frames ? frames : 1) * sizeof(float));
            for (long i = 0; i < frames; i++) {
                double acc = 0;
                for (int c = 0; c < ch; c++)
                    acc += sample_at(raw + (size_t)i * frame + (size_t)c * bps, fmt, bits);
                out[i] = (float)(acc / ch);
            }
            free(raw);
            *fs = rate;
            *n = frames;
            break;
        } else {
            fseek(fp, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    fclose(fp);
    if (!out) fprintf(stderr, "%s: no readable audio data\n", path);
    return out;
}

static void wr32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

int wav_write(const char *path, const float *x, long n, int fs)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror(path); return -1; }
    unsigned char h[44];
    memcpy(h, "RIFF", 4); wr32(h + 4, (uint32_t)(36 + n * 2));
    memcpy(h + 8, "WAVEfmt ", 8); wr32(h + 16, 16);
    wr16(h + 20, 1); wr16(h + 22, 1); wr32(h + 24, (uint32_t)fs);
    wr32(h + 28, (uint32_t)fs * 2); wr16(h + 32, 2); wr16(h + 34, 16);
    memcpy(h + 36, "data", 4); wr32(h + 40, (uint32_t)(n * 2));
    fwrite(h, 1, 44, fp);
    for (long i = 0; i < n; i++) {
        double v = x[i] * 32767.0;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        unsigned char s[2];
        wr16(s, (uint16_t)(int16_t)(v < 0 ? v - 0.5 : v + 0.5));
        fwrite(s, 1, 2, fp);
    }
    return fclose(fp);
}

/* image.c - simple RGB image + PPM (P6) I/O */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "image.h"

image *image_new(int w, int h)
{
    image *img = malloc(sizeof *img);
    img->w = w;
    img->h = h;
    img->px = calloc((size_t)w * h * 3, 1);
    return img;
}

void image_free(image *img)
{
    if (!img) return;
    free(img->px);
    free(img);
}

static int ppm_int(FILE *fp)
{
    int c = fgetc(fp);
    for (;;) {
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(fp); }
        else if (c != EOF && isspace(c)) c = fgetc(fp);
        else break;
    }
    if (c == EOF || !isdigit(c)) return -1;
    int v = 0;
    while (c != EOF && isdigit(c)) { v = v * 10 + (c - '0'); c = fgetc(fp); }
    return v;   /* the single whitespace after the number is consumed */
}

image *ppm_read(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }
    char magic[2];
    if (fread(magic, 1, 2, fp) != 2 || magic[0] != 'P' || magic[1] != '6') {
        fprintf(stderr, "%s: only binary PPM (P6) is supported\n", path);
        fclose(fp);
        return NULL;
    }
    int w = ppm_int(fp), h = ppm_int(fp), max = ppm_int(fp);
    if (w <= 0 || h <= 0 || max <= 0 || max > 255) {
        fprintf(stderr, "%s: bad PPM header\n", path);
        fclose(fp);
        return NULL;
    }
    image *img = image_new(w, h);
    size_t want = (size_t)w * h * 3;
    if (fread(img->px, 1, want, fp) != want) {
        fprintf(stderr, "%s: csonka PPM\n", path);
        image_free(img);
        fclose(fp);
        return NULL;
    }
    if (max != 255)
        for (size_t i = 0; i < want; i++) img->px[i] = (unsigned char)(img->px[i] * 255 / max);
    fclose(fp);
    return img;
}

int ppm_write(const char *path, const image *img)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror(path); return -1; }
    fprintf(fp, "P6\n%d %d\n255\n", img->w, img->h);
    fwrite(img->px, 1, (size_t)img->w * img->h * 3, fp);
    return fclose(fp);
}

image *image_resize(const image *src, int w, int h)
{
    image *dst = image_new(w, h);
    if (src->w == w && src->h == h) {
        memcpy(dst->px, src->px, (size_t)w * h * 3);
        return dst;
    }
    for (int y = 0; y < h; y++) {
        double sy = (y + 0.5) * src->h / h - 0.5;
        if (sy < 0) sy = 0;
        int y0 = (int)sy, y1 = y0 + 1 < src->h ? y0 + 1 : y0;
        double fy = sy - y0;
        for (int x = 0; x < w; x++) {
            double sx = (x + 0.5) * src->w / w - 0.5;
            if (sx < 0) sx = 0;
            int x0 = (int)sx, x1 = x0 + 1 < src->w ? x0 + 1 : x0;
            double fx = sx - x0;
            for (int c = 0; c < 3; c++) {
                double a = src->px[(y0 * src->w + x0) * 3 + c], b = src->px[(y0 * src->w + x1) * 3 + c];
                double d = src->px[(y1 * src->w + x0) * 3 + c], e = src->px[(y1 * src->w + x1) * 3 + c];
                double v = (a * (1 - fx) + b * fx) * (1 - fy) + (d * (1 - fx) + e * fx) * fy;
                dst->px[(y * w + x) * 3 + c] = (unsigned char)(v + 0.5);
            }
        }
    }
    return dst;
}

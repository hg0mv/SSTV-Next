/* imgio.c - image files via stb_image / stb_image_write (third_party/) */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_PNM
#define STBI_ONLY_GIF
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "imgio.h"

image *img_load(const char *path)
{
    int w, h, n;
    unsigned char *d = stbi_load(path, &w, &h, &n, 3);
    if (!d) return NULL;
    image *img = image_new(w, h);
    memcpy(img->px, d, (size_t)w * h * 3);
    stbi_image_free(d);
    return img;
}

int img_save_png(const char *path, const image *img)
{
    return stbi_write_png(path, img->w, img->h, 3, img->px, img->w * 3) ? 0 : -1;
}

image *img_fit(const image *src, int w, int h)
{
    /* largest centred window of the source with the target aspect ratio */
    int cw = src->w, ch = (int)((double)src->w * h / w + 0.5);
    if (ch > src->h) { ch = src->h; cw = (int)((double)src->h * w / h + 0.5); }
    if (cw > src->w) cw = src->w;
    int x0 = (src->w - cw) / 2, y0 = (src->h - ch) / 2;
    image *crop = image_new(cw, ch);
    for (int y = 0; y < ch; y++)
        memcpy(crop->px + (size_t)y * cw * 3, src->px + ((size_t)(y + y0) * src->w + x0) * 3, (size_t)cw * 3);
    /* halve with a 2x2 box first while far larger: bilinear alone would alias */
    while (crop->w >= 2 * w && crop->h >= 2 * h) {
        image *hf = image_new(crop->w / 2, crop->h / 2);
        for (int y = 0; y < hf->h; y++)
            for (int x = 0; x < hf->w; x++)
                for (int c = 0; c < 3; c++) {
                    const unsigned char *a = crop->px + ((size_t)(2 * y) * crop->w + 2 * x) * 3 + c;
                    const unsigned char *b = a + (size_t)crop->w * 3;
                    hf->px[((size_t)y * hf->w + x) * 3 + c] = (unsigned char)((a[0] + a[3] + b[0] + b[3] + 2) / 4);
                }
        image_free(crop);
        crop = hf;
    }
    image *out = image_resize(crop, w, h);
    image_free(crop);
    return out;
}

int mkdir_p(const char *path)
{
    char t[512];
    snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    return (mkdir(t, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

image *img_load_mem(const unsigned char *data, int len)
{
    int w, h, ch;
    unsigned char *p = stbi_load_from_memory(data, len, &w, &h, &ch, 3);
    if (!p) return NULL;
    image *img = image_new(w, h);
    memcpy(img->px, p, (size_t)w * h * 3);
    stbi_image_free(p);
    return img;
}

typedef struct { unsigned char *b; int n, cap; } membuf;
static void mem_put(void *ctx, void *data, int size)
{
    membuf *m = ctx;
    if (m->n + size > m->cap) { m->cap = (m->n + size) * 2; m->b = realloc(m->b, m->cap); }
    memcpy(m->b + m->n, data, size);
    m->n += size;
}

unsigned char *img_jpeg(const image *img, int quality, int *len)
{
    membuf m = {0};
    if (!stbi_write_jpg_to_func(mem_put, &m, img->w, img->h, 3, img->px, quality)) { free(m.b); return NULL; }
    *len = m.n;
    return m.b;
}

/* best quality that fits into max_bytes (at least quality 5) */
unsigned char *img_jpeg_fit(const image *img, int max_bytes, int *len, int *quality)
{
    int lo = 5, hi = 92, bestq = 5;
    while (lo <= hi) {
        int q = (lo + hi) / 2, n;
        unsigned char *b = img_jpeg(img, q, &n);
        free(b);
        if (b && n <= max_bytes) { bestq = q; lo = q + 1; } else hi = q - 1;
    }
    if (quality) *quality = bestq;
    return img_jpeg(img, bestq, len);
}

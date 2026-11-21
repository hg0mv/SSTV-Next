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

/* ---- PNG text chunk (tEXt, keyword "SSTVNext") with the reception data ---- */

static unsigned long crc32_png(const unsigned char *p, size_t n)
{
    unsigned long c = 0xffffffffUL;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320UL & (0UL - (c & 1)));
    }
    return c ^ 0xffffffffUL;
}

static void put32(unsigned char *p, unsigned long v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int img_save_png_meta(const char *path, const image *img, const char *meta)
{
    if (!meta || !meta[0]) return img_save_png(path, img);
    int len = 0;
    unsigned char *png = stbi_write_png_to_mem(img->px, img->w * 3, img->w, img->h, 3, &len);
    if (!png || len < 12 + 8) { free(png); return -1; }
    static const char key[] = "SSTVNext";
    size_t ml = strlen(meta), dl = sizeof key + ml;          /* keyword, 0, text */
    unsigned char *ch = malloc(12 + dl);
    put32(ch, (unsigned long)dl);
    memcpy(ch + 4, "tEXt", 4);
    memcpy(ch + 8, key, sizeof key);
    memcpy(ch + 8 + sizeof key, meta, ml);
    put32(ch + 8 + dl, crc32_png(ch + 4, 4 + dl));
    int ret = -1;
    FILE *f = fopen(path, "wb");
    if (f) {
        /* everything up to IEND (the last 12 bytes), our chunk, then IEND */
        int ok = fwrite(png, 1, (size_t)len - 12, f) == (size_t)len - 12 &&
                 fwrite(ch, 1, 12 + dl, f) == 12 + dl &&
                 fwrite(png + len - 12, 1, 12, f) == 12;
        if (fclose(f) == 0 && ok) ret = 0;
    }
    free(ch);
    STBIW_FREE(png);
    return ret;
}

int img_png_meta(const char *path, char *out, size_t n)
{
    if (n) out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[8];
    int found = 0;
    if (fread(h, 1, 8, f) == 8 && !memcmp(h, "\x89PNG", 4)) {
        for (;;) {
            unsigned char c[8];
            if (fread(c, 1, 8, f) != 8) break;
            unsigned long l = ((unsigned long)c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
            if (!memcmp(c + 4, "IEND", 4) || l > (1u << 26)) break;
            if (!memcmp(c + 4, "tEXt", 4) && l > 9 && l < 4096) {
                char buf[4096];
                if (fread(buf, 1, l, f) != l) break;
                fseek(f, 4, SEEK_CUR);
                if (!memcmp(buf, "SSTVNext", 9)) {
                    size_t tl = l - 9;
                    if (tl >= n) tl = n - 1;
                    memcpy(out, buf + 9, tl);
                    out[tl] = 0;
                    found = 1;
                    break;
                }
                continue;
            }
            if (fseek(f, (long)l + 4, SEEK_CUR)) break;
        }
    }
    fclose(f);
    return found;
}

/* value of "key=value" in a meta string ("k1=v1;k2=v2"); 1 if found */
int img_meta_get(const char *meta, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = meta; p && *p; ) {
        const char *e = strchr(p, ';');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = l - kl - 1;
            if (vl >= n) vl = n - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            return 1;
        }
        p = e ? e + 1 : NULL;
    }
    if (n) out[0] = 0;
    return 0;
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

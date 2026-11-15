/* image.h - simple RGB image + PPM (P6) I/O */
#ifndef IMAGE_H
#define IMAGE_H

typedef struct {
    int w, h;
    unsigned char *px;   /* RGB, row-major, 3 bytes per pixel */
} image;

image *image_new(int w, int h);
void   image_free(image *img);
image *ppm_read(const char *path);
int    ppm_write(const char *path, const image *img);
image *image_resize(const image *src, int w, int h);   /* bilinear */

#endif

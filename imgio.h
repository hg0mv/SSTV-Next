/* imgio.h - image files (PNG/JPEG/BMP/PPM in, PNG out) via stb_image */
#ifndef IMGIO_H
#define IMGIO_H

#include "image.h"

image *img_load(const char *path);
image *img_load_mem(const unsigned char *data, int len);                  /* any format stb_image reads */
int    img_save_png(const char *path, const image *img);
/* crop the centre to the w:h aspect, then resize to w x h */
image *img_fit(const image *src, int w, int h);
int    mkdir_p(const char *path);
unsigned char *img_jpeg(const image *img, int quality, int *len);              /* malloc'd JPEG file */
unsigned char *img_jpeg_fit(const image *img, int max_bytes, int *len, int *quality);

#endif

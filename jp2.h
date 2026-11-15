/* jp2.h - JPEG 2000 loading via the system OpenJPEG library (optional, run time) */
#ifndef JP2_H
#define JP2_H
#include "image.h"
int    jp2_available(void);
image *jp2_load(const char *path);     /* NULL if not JPEG 2000 or no library */
#endif

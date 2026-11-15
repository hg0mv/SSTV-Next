/* ui.h - small software renderer for the X11 window
 *
 * Everything is drawn into a client-side RGB framebuffer (anti-aliased
 * rounded shapes, gradients, soft shadows, TrueType text via stb_truetype)
 * and changed regions are sent to the X server with XPutImage. Colours are
 * 0xRRGGBB.
 */
#ifndef UI_H
#define UI_H

#include <X11/Xlib.h>
#include <stdint.h>
#include "image.h"

typedef uint32_t col_t;
#ifdef RGB
#undef RGB              /* windows.h has one with the opposite byte order */
#endif
#define RGB(r, g, b) ((col_t)(((r) & 255) << 16 | ((g) & 255) << 8 | ((b) & 255)))

enum { F_REG, F_BOLD, F_SMALL, F_SMALLB, F_TITLE, F_BIG, F_MONO, F_COUNT };

int   ui_init(Display *d, Window w, Visual *v, int depth);   /* 0 = ok */
void  ui_resize(int w, int h);
void  ui_free(void);

/* clipping: drawing outside the rectangle is ignored */
void  ui_clip(int x, int y, int w, int h);
void  ui_noclip(void);

/* send a region / everything drawn since the last flush to the window */
void  ui_present(int x, int y, int w, int h);
void  ui_flush(void);

void  ui_fill(int x, int y, int w, int h, col_t c);
void  ui_fill_a(int x, int y, int w, int h, col_t c, double a);
void  ui_rect(int x, int y, int w, int h, col_t c);               /* like XDrawRectangle */
void  ui_line(double x1, double y1, double x2, double y2, col_t c, double a);   /* anti-aliased */
void  ui_hline_dash(int x1, int x2, int y, col_t c, double a);
void  ui_vline_dash(int x, int y1, int y2, col_t c, double a);
void  ui_rrect(int x, int y, int w, int h, int r, col_t c, double a);
void  ui_rrect_grad(int x, int y, int w, int h, int r, col_t top, col_t bottom);
void  ui_rrect_line(int x, int y, int w, int h, int r, col_t c, double a);
void  ui_shadow(int x, int y, int w, int h, int r, int size, double a);
void  ui_dim(int x, int y, int w, int h, double a);
void  ui_circle(double cx, double cy, double rad, col_t c, double a);
void  ui_image(const image *img, int x, int y, int dw, int dh);   /* scaled */
void  ui_round_corners(int x, int y, int w, int h, int r, col_t bg);
void  ui_vgrad_area(int x, int y, int w, const int *top, int bottom, col_t c, double a_top, double a_bot);

/* text: x = left, y = baseline; UTF-8 (Latin-1 bytes accepted) */
int   ui_text(int font, int x, int y, const char *s, col_t c);    /* returns the width */
int   ui_text_w(int font, const char *s);
int   ui_text_fit(int font, int x, int y, const char *s, col_t c, int maxw);   /* ... when too long */
int   ui_font_ascent(int font);

col_t ui_mix(col_t a, col_t b, double t);

#endif

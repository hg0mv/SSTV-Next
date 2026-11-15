/* ui.c - small software renderer for the X11 window (see ui.h) */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glob.h>
#include "ui.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "third_party/stb_truetype.h"     /* implementation is in tpl.c */
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static Display *dpy;
static Window win;
static Visual *vis;
static int depth;
static GC gc;
static XImage *xim;
static uint32_t *fb;
static int fw, fh;
static int cx0, cy0, cx1, cy1;                 /* clip, x1/y1 exclusive */
static int dx0, dy0, dx1, dy1, dirty;          /* drawn since the last flush */
static int fast32;                             /* XImage is 32 bpp 0x00RRGGBB */

static inline double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

col_t ui_mix(col_t a, col_t b, double t)
{
    t = clamp01(t);
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t + 0.5);
    int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t + 0.5);
    int bl = (int)((a & 255) * (1 - t) + (b & 255) * t + 0.5);
    return RGB(r, g, bl);
}

static void mark(int x0, int y0, int x1, int y1)
{
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x1 <= x0 || y1 <= y0) return;
    if (!dirty) { dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1; dirty = 1; return; }
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}

static inline void put(int x, int y, col_t c, double a)
{
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1 || a <= 0.002) return;
    uint32_t *p = &fb[(size_t)y * fw + x];
    if (a >= 0.998) { *p = c; return; }
    int ia = (int)(a * 256);
    uint32_t d = *p;
    int r = (int)((d >> 16) & 255), g = (int)((d >> 8) & 255), b = (int)(d & 255);
    r += ((int)((c >> 16) & 255) - r) * ia >> 8;
    g += ((int)((c >> 8) & 255) - g) * ia >> 8;
    b += ((int)(c & 255) - b) * ia >> 8;
    *p = RGB(r, g, b);
}

/* ------------------------------------------------------------- setup --- */

#ifdef _WIN32
/* Windows: the framebuffer is the window's DIB section (win/winx.c) */
static void alloc_fb(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    fw = w; fh = h;
    fb = winx_framebuffer(w, h);
    ui_noclip();
    dirty = 0;
    mark(0, 0, w, h);
}
static void fonts_load(void);
int ui_init(Display *d, Window w, Visual *v, int dep)
{
    dpy = d; win = w; vis = v; depth = dep; (void)gc; (void)xim; (void)fast32;
    alloc_fb(1280, 820);
    fonts_load();
    return fb ? 0 : -1;
}
void ui_resize(int w, int h) { if (w != fw || h != fh) alloc_fb(w, h); }
static void blit(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fw) w = fw - x;
    if (y + h > fh) h = fh - y;
    if (w > 0 && h > 0) winx_blit(x, y, w, h);
}
void ui_free(void) { fb = NULL; }
#else
static void alloc_fb(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (xim) { XDestroyImage(xim); xim = NULL; }     /* frees its data */
    free(fb);
    fw = w; fh = h;
    fb = calloc((size_t)w * h, sizeof *fb);
    xim = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, NULL, (unsigned)w, (unsigned)h, 32, 0);
    if (xim) xim->data = calloc((size_t)xim->bytes_per_line * h, 1);
    fast32 = xim && xim->bits_per_pixel == 32 && vis->red_mask == 0xFF0000 && vis->green_mask == 0xFF00 &&
             vis->blue_mask == 0xFF && xim->byte_order == (ImageByteOrder(dpy));
    ui_noclip();
    dirty = 0;
    mark(0, 0, w, h);
}

static void fonts_load(void);

int ui_init(Display *d, Window w, Visual *v, int dep)
{
    dpy = d; win = w; vis = v; depth = dep;
    gc = XCreateGC(dpy, win, 0, NULL);
    alloc_fb(1280, 820);
    fonts_load();
    return xim ? 0 : -1;
}

void ui_resize(int w, int h) { if (w != fw || h != fh) alloc_fb(w, h); }
#endif

void ui_clip(int x, int y, int w, int h)
{
    cx0 = x < 0 ? 0 : x; cy0 = y < 0 ? 0 : y;
    cx1 = x + w > fw ? fw : x + w; cy1 = y + h > fh ? fh : y + h;
}
void ui_noclip(void) { cx0 = cy0 = 0; cx1 = fw; cy1 = fh; }

#ifndef _WIN32
/* framebuffer -> XImage -> window */
static void blit(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fw) w = fw - x;
    if (y + h > fh) h = fh - y;
    if (w <= 0 || h <= 0 || !xim) return;
    if (fast32 && xim->bytes_per_line == fw * 4) {
        for (int j = y; j < y + h; j++) memcpy(xim->data + (size_t)j * xim->bytes_per_line + (size_t)x * 4, fb + (size_t)j * fw + x, (size_t)w * 4);
    } else {
        int sr = 0, sg = 0, sb = 0, br = 0, bgb = 0, bb = 0;
        unsigned long m;
        for (m = vis->red_mask; m && !(m & 1); m >>= 1) sr++;
        for (; m & 1; m >>= 1) br++;
        for (m = vis->green_mask; m && !(m & 1); m >>= 1) sg++;
        for (; m & 1; m >>= 1) bgb++;
        for (m = vis->blue_mask; m && !(m & 1); m >>= 1) sb++;
        for (; m & 1; m >>= 1) bb++;
        for (int j = y; j < y + h; j++)
            for (int i = x; i < x + w; i++) {
                uint32_t c = fb[(size_t)j * fw + i];
                unsigned long px = ((unsigned long)((c >> 16) & 255) >> (8 - br) << sr) |
                                   ((unsigned long)((c >> 8) & 255) >> (8 - bgb) << sg) |
                                   ((unsigned long)(c & 255) >> (8 - bb) << sb);
                XPutPixel(xim, i, j, px);
            }
    }
    XPutImage(dpy, win, gc, xim, x, y, x, y, (unsigned)w, (unsigned)h);
}
#endif

void ui_present(int x, int y, int w, int h)
{
    blit(x, y, w, h);
    if (dirty && dx0 >= x && dy0 >= y && dx1 <= x + w && dy1 <= y + h) dirty = 0;
}

void ui_flush(void)
{
    if (dirty) { blit(dx0, dy0, dx1 - dx0, dy1 - dy0); dirty = 0; }
    XFlush(dpy);
}

#ifndef _WIN32
void ui_free(void)
{
    if (xim) XDestroyImage(xim);
    xim = NULL;
    free(fb); fb = NULL;
    if (gc) XFreeGC(dpy, gc);
}
#endif

/* -------------------------------------------------------- primitives --- */

void ui_fill(int x, int y, int w, int h, col_t c)
{
    int x1 = x + w, y1 = y + h;
    mark(x, y, x1, y1);
    if (x < cx0) x = cx0;
    if (y < cy0) y = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    for (int j = y; j < y1; j++) { uint32_t *p = fb + (size_t)j * fw; for (int i = x; i < x1; i++) p[i] = c; }
}

void ui_fill_a(int x, int y, int w, int h, col_t c, double a)
{
    if (a >= 0.998) { ui_fill(x, y, w, h, c); return; }
    mark(x, y, x + w, y + h);
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) put(i, j, c, a);
}

void ui_rect(int x, int y, int w, int h, col_t c)
{
    ui_fill(x, y, w + 1, 1, c); ui_fill(x, y + h, w + 1, 1, c);
    ui_fill(x, y, 1, h + 1, c); ui_fill(x + w, y, 1, h + 1, c);
}

void ui_line(double x1, double y1, double x2, double y2, col_t c, double a)
{
    if (fabs(y1 - y2) < 1e-9) { int xa = (int)floor(fmin(x1, x2)), xb = (int)floor(fmax(x1, x2)); ui_fill_a(xa, (int)floor(y1), xb - xa + 1, 1, c, a); return; }
    if (fabs(x1 - x2) < 1e-9) { int ya = (int)floor(fmin(y1, y2)), yb = (int)floor(fmax(y1, y2)); ui_fill_a((int)floor(x1), ya, 1, yb - ya + 1, c, a); return; }
    double dx = x2 - x1, dy = y2 - y1, L2 = dx * dx + dy * dy;
    int xa = (int)floor(fmin(x1, x2)) - 1, xb = (int)ceil(fmax(x1, x2)) + 1;
    int ya = (int)floor(fmin(y1, y2)) - 1, yb = (int)ceil(fmax(y1, y2)) + 1;
    mark(xa, ya, xb + 1, yb + 1);
    for (int y = ya; y <= yb; y++)
        for (int x = xa; x <= xb; x++) {
            double px = x + 0.5, py = y + 0.5;
            double t = ((px - x1) * dx + (py - y1) * dy) / L2;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double ex = px - (x1 + t * dx), ey = py - (y1 + t * dy), d = sqrt(ex * ex + ey * ey);
            double cov = clamp01(1.1 - d);
            if (cov > 0) put(x, y, c, a * cov);
        }
}

void ui_hline_dash(int x1, int x2, int y, col_t c, double a)
{
    for (int x = x1; x <= x2; x += 6) ui_fill_a(x, y, x + 3 > x2 ? x2 - x + 1 : 3, 1, c, a);
}

void ui_vline_dash(int x, int y1, int y2, col_t c, double a)
{
    for (int y = y1; y <= y2; y += 6) ui_fill_a(x, y, 1, y + 3 > y2 ? y2 - y + 1 : 3, c, a);
}

/* signed distance to a rounded rectangle (pixel centre px,py); <0 inside */
static inline double sd_rrect(double px, double py, double x, double y, double w, double h, double r)
{
    double hx = w / 2, hy = h / 2, qx = fabs(px - (x + hx)) - (hx - r), qy = fabs(py - (y + hy)) - (hy - r);
    double mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    return sqrt(mx * mx + my * my) + fmin(fmax(qx, qy), 0.0) - r;
}

/* rounded rectangle row by row: only the corner rows need the distance */
static void rrect_rows(int x, int y, int w, int h, int r, col_t top, col_t bot, double a, int grad)
{
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    mark(x, y, x + w, y + h);
    for (int j = y; j < y + h; j++) {
        if (j < cy0 || j >= cy1) continue;
        col_t c = grad ? ui_mix(top, bot, h > 1 ? (double)(j - y) / (h - 1) : 0) : top;
        int corner = j < y + r || j >= y + h - r;
        if (!corner) {
            if (a >= 0.998) { int xa = x < cx0 ? cx0 : x, xb = x + w > cx1 ? cx1 : x + w; uint32_t *p = fb + (size_t)j * fw; for (int i = xa; i < xb; i++) p[i] = c; }
            else for (int i = x; i < x + w; i++) put(i, j, c, a);
            continue;
        }
        for (int i = x; i < x + w; i++) {
            if (i >= x + r && i < x + w - r) { put(i, j, c, a); continue; }
            double d = sd_rrect(i + 0.5, j + 0.5, x, y, w, h, r);
            double cov = clamp01(0.5 - d);
            if (cov > 0) put(i, j, c, a * cov);
        }
    }
}

void ui_rrect(int x, int y, int w, int h, int r, col_t c, double a) { rrect_rows(x, y, w, h, r, c, c, a, 0); }
void ui_rrect_grad(int x, int y, int w, int h, int r, col_t top, col_t bottom) { rrect_rows(x, y, w, h, r, top, bottom, 1, 1); }

void ui_rrect_line(int x, int y, int w, int h, int r, col_t c, double a)
{
    if (w <= 1 || h <= 1) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    mark(x, y, x + w, y + h);
    ui_fill_a(x + r, y, w - 2 * r, 1, c, a); ui_fill_a(x + r, y + h - 1, w - 2 * r, 1, c, a);
    ui_fill_a(x, y + r, 1, h - 2 * r, c, a); ui_fill_a(x + w - 1, y + r, 1, h - 2 * r, c, a);
    if (r <= 0) return;
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            /* distance from the corner arc of radius r-0.5 */
            double d = sqrt((r - i - 0.5) * (r - i - 0.5) + (r - j - 0.5) * (r - j - 0.5)) - (r - 0.5);
            double cov = clamp01(1 - fabs(d));
            if (cov <= 0) continue;
            put(x + i, y + j, c, a * cov); put(x + w - 1 - i, y + j, c, a * cov);
            put(x + i, y + h - 1 - j, c, a * cov); put(x + w - 1 - i, y + h - 1 - j, c, a * cov);
        }
}

void ui_shadow(int x, int y, int w, int h, int r, int size, double a)
{
    int oy = size / 3;
    mark(x - size, y - size + oy, x + w + size, y + h + size + oy);
    for (int j = y - size + oy; j < y + h + size + oy; j++)
        for (int i = x - size; i < x + w + size; i++) {
            double d = sd_rrect(i + 0.5, j + 0.5, x, y + oy, w, h, r);
            if (d <= 0) {                                         /* under the box: only where it is not covered */
                if (i >= x && i < x + w && j >= y && j < y + h) continue;
                put(i, j, 0, a);
                continue;
            }
            double t = 1 - d / size;
            if (t > 0) put(i, j, 0, a * t * t);
        }
}

void ui_dim(int x, int y, int w, int h, double a) { ui_fill_a(x, y, w, h, 0, a); }

void ui_circle(double cx, double cy, double rad, col_t c, double a)
{
    int xa = (int)floor(cx - rad - 1), xb = (int)ceil(cx + rad + 1), ya = (int)floor(cy - rad - 1), yb = (int)ceil(cy + rad + 1);
    mark(xa, ya, xb + 1, yb + 1);
    for (int j = ya; j <= yb; j++)
        for (int i = xa; i <= xb; i++) {
            double d = sqrt((i + 0.5 - cx) * (i + 0.5 - cx) + (j + 0.5 - cy) * (j + 0.5 - cy)) - rad;
            double cov = clamp01(0.5 - d);
            if (cov > 0) put(i, j, c, a * cov);
        }
}

void ui_image(const image *img, int x, int y, int dw, int dh)
{
    if (!img || dw < 1 || dh < 1) return;
    mark(x, y, x + dw, y + dh);
    int down = dw < img->w || dh < img->h;
    for (int j = 0; j < dh; j++) {
        int Y = y + j;
        if (Y < cy0 || Y >= cy1) continue;
        uint32_t *p = fb + (size_t)Y * fw;
        int sy0 = j * img->h / dh, sy1 = (j + 1) * img->h / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int i = 0; i < dw; i++) {
            int X = x + i;
            if (X < cx0 || X >= cx1) continue;
            if (!down) {                                          /* zoom: crisp pixels */
                const unsigned char *s = img->px + ((size_t)sy0 * img->w + (size_t)i * img->w / dw) * 3;
                p[X] = RGB(s[0], s[1], s[2]);
                continue;
            }
            int sx0 = i * img->w / dw, sx1 = (i + 1) * img->w / dw;   /* shrink: area average */
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const unsigned char *s = img->px + ((size_t)yy * img->w + sx0) * 3;
                for (int xx = sx0; xx < sx1; xx++, s += 3) { r += s[0]; g += s[1]; b += s[2]; n++; }
            }
            p[X] = RGB(r / n, g / n, b / n);
        }
    }
}

void ui_round_corners(int x, int y, int w, int h, int r, col_t bg)
{
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            double d = sd_rrect(x + i + 0.5, y + j + 0.5, x, y, w, h, r);
            double out = clamp01(0.5 + d);
            if (out <= 0) continue;
            put(x + i, y + j, bg, out); put(x + w - 1 - i, y + j, bg, out);
            put(x + i, y + h - 1 - j, bg, out); put(x + w - 1 - i, y + h - 1 - j, bg, out);
        }
}

/* filled area under a curve (top[i] for column x+i) down to 'bottom', alpha fading */
void ui_vgrad_area(int x, int y, int w, const int *top, int bottom, col_t c, double a_top, double a_bot)
{
    (void)y;
    for (int i = 0; i < w; i++) {
        int t = top[i];
        if (t >= bottom) continue;
        mark(x + i, t, x + i + 1, bottom);
        for (int j = t; j < bottom; j++) put(x + i, j, c, a_top + (a_bot - a_top) * (double)(j - t) / (bottom - t));
    }
}

/* -------------------------------------------------------------- text --- */

typedef struct {
    stbtt_fontinfo info;
    unsigned char *data;
    float px, scale;
    int ascent, ok;
} uifont;

static uifont fonts[F_COUNT];
static const float font_px[F_COUNT] = { 13, 13, 11.5f, 11.5f, 14, 21, 12.5f };

static unsigned char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = n > 0 && n < 40L * 1024 * 1024 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static int load_one(uifont *f, const char *const *names, float px)
{
    const char *env = getenv("SSTV_UI_FONT");
#ifdef _WIN32
    static char wdir[MAX_PATH + 8], edir[MAX_PATH + 8];
    if (!wdir[0]) {
        const char *w = getenv("WINDIR");
        snprintf(wdir, sizeof wdir, "%s/Fonts", w && *w ? w : "C:/Windows");
        char me[MAX_PATH]; DWORD l = GetModuleFileNameA(NULL, me, sizeof me);
        char *sl = l ? strrchr(me, '\\') : NULL; if (sl) *sl = 0;
        snprintf(edir, sizeof edir, "%s/fonts", l ? me : ".");      /* fonts shipped with the program */
    }
    const char *dirs[] = { wdir, edir };
#else
    static const char *dirs[] = { "/usr/share/fonts/truetype/ubuntu", "/usr/share/fonts/truetype/dejavu",
                                  "/usr/share/fonts/truetype/liberation2", "/usr/share/fonts/truetype/liberation",
                                  "/usr/share/fonts/truetype/freefont", "/usr/share/fonts/TTF", "/usr/share/fonts/dejavu" };
#endif
    for (int k = 0; names[k]; k++) {
        if (env && f == &fonts[F_REG] && k == 0) { if ((f->data = read_file(env))) goto init; }
        for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
            char p[300];
            snprintf(p, sizeof p, "%s/%s", dirs[d], names[k]);
            if ((f->data = read_file(p))) goto init;
        }
        char pat[300];
        snprintf(pat, sizeof pat, "/usr/share/fonts/truetype/*/%s", names[k]);
        glob_t g;
        if (glob(pat, 0, NULL, &g) == 0) {
            if (g.gl_pathc) f->data = read_file(g.gl_pathv[0]);
            globfree(&g);
            if (f->data) goto init;
        }
        continue;
init:
        if (stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
            int a, d, l;
            f->px = px;
            f->scale = stbtt_ScaleForPixelHeight(&f->info, px);
            stbtt_GetFontVMetrics(&f->info, &a, &d, &l);
            f->ascent = (int)lroundf(a * f->scale);
            f->ok = 1;
            return 1;
        }
        free(f->data); f->data = NULL;
    }
    return 0;
}

static void fonts_load(void)
{
#ifdef _WIN32
    static const char *reg[] = { "segoeui.ttf", "DejaVuSans.ttf", "arial.ttf", "tahoma.ttf", NULL };
    static const char *bold[] = { "seguisb.ttf", "segoeuib.ttf", "DejaVuSans-Bold.ttf", "arialbd.ttf", "tahomabd.ttf", NULL };
    static const char *mono[] = { "consola.ttf", "DejaVuSansMono.ttf", "cour.ttf", "lucon.ttf", NULL };
#else
    static const char *reg[] = { "Ubuntu-R.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf", "FreeSans.ttf", NULL };
    static const char *bold[] = { "Ubuntu-M.ttf", "Ubuntu-B.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf", "FreeSansBold.ttf", NULL };
    static const char *mono[] = { "UbuntuMono-R.ttf", "DejaVuSansMono.ttf", "LiberationMono-Regular.ttf", "FreeMono.ttf", NULL };
#endif
    for (int i = 0; i < F_COUNT; i++) {
        const char *const *n = (i == F_BOLD || i == F_SMALLB || i == F_TITLE || i == F_BIG) ? bold : i == F_MONO ? mono : reg;
        float px = font_px[i];
        if (!load_one(&fonts[i], n, px) && i != F_REG && fonts[F_REG].ok) {   /* no bold/mono: the regular one */
            fonts[i] = fonts[F_REG];
            fonts[i].px = px; fonts[i].scale = stbtt_ScaleForPixelHeight(&fonts[i].info, px);
            int a, d, l; stbtt_GetFontVMetrics(&fonts[i].info, &a, &d, &l); fonts[i].ascent = (int)lroundf(a * fonts[i].scale);
        }
    }
    if (!fonts[F_REG].ok) fprintf(stderr, "sstv_next: no TrueType font found (install fonts-dejavu-core); text uses the X core font\n");
}

/* glyph cache */
typedef struct { int key; int w, h, xo, yo; float adv; unsigned char *bmp; } glyph;
#define GCACHE 2048
static glyph gcache[GCACHE];

static glyph *get_glyph(int font, int cp)
{
    int key = font << 21 | (cp & 0x1FFFFF);
    unsigned hsh = (unsigned)key * 2654435761u % GCACHE;
    for (int k = 0; k < GCACHE; k++) {
        glyph *g = &gcache[(hsh + k) % GCACHE];
        if (g->bmp && g->key == key) return g;
        if (!g->bmp) {
            uifont *f = &fonts[font];
            int adv, lsb, x0, y0, x1, y1;
            stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
            stbtt_GetCodepointBitmapBox(&f->info, cp, f->scale, f->scale, &x0, &y0, &x1, &y1);
            g->key = key; g->w = x1 - x0; g->h = y1 - y0; g->xo = x0; g->yo = y0; g->adv = adv * f->scale;
            g->bmp = calloc((size_t)(g->w > 0 ? g->w : 1) * (g->h > 0 ? g->h : 1), 1);
            if (g->w > 0 && g->h > 0) stbtt_MakeCodepointBitmap(&f->info, g->bmp, g->w, g->h, g->w, f->scale, f->scale, cp);
            return g;
        }
    }
    return NULL;
}

static int next_cp(const unsigned char **s)
{
    const unsigned char *p = *s;
    int c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (p[0] & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p++; }
    else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    *s = p;
    return c;
}

/* gamma table: light text on dark panels looks too thin with linear coverage */
static unsigned char gam[256];
static void gamma_init(void) { if (gam[255]) return; for (int i = 0; i < 256; i++) gam[i] = (unsigned char)lround(255 * pow(i / 255.0, 0.78)); }

/* fallback without any TrueType font: the X core font, through a pixmap */
#ifdef _WIN32
static int xcore_text(int x, int y, const char *s, col_t c, int draw) { (void)x; (void)y; (void)s; (void)c; (void)draw; return 0; }
#else
static XFontStruct *xcore;
static int xcore_text(int x, int y, const char *s, col_t c, int draw)
{
    if (!xcore) xcore = XLoadQueryFont(dpy, "fixed");
    if (!xcore) return 0;
    int n = (int)strlen(s), w = XTextWidth(xcore, s, n), h = xcore->ascent + xcore->descent;
    if (!draw || w <= 0) return w;
    Pixmap pm = XCreatePixmap(dpy, win, (unsigned)w, (unsigned)h, (unsigned)depth);
    GC g2 = XCreateGC(dpy, pm, 0, NULL);
    XSetFont(dpy, g2, xcore->fid);
    XSetForeground(dpy, g2, BlackPixel(dpy, DefaultScreen(dpy))); XFillRectangle(dpy, pm, g2, 0, 0, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, g2, WhitePixel(dpy, DefaultScreen(dpy))); XDrawString(dpy, pm, g2, 0, xcore->ascent, s, n);
    XImage *im = XGetImage(dpy, pm, 0, 0, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
    if (im) {
        unsigned long blk = BlackPixel(dpy, DefaultScreen(dpy));
        mark(x, y - xcore->ascent, x + w, y - xcore->ascent + h);
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (XGetPixel(im, i, j) != blk) put(x + i, y - xcore->ascent + j, c, 1);
        XDestroyImage(im);
    }
    XFreeGC(dpy, g2); XFreePixmap(dpy, pm);
    return w;
}
#endif

int ui_text_w(int font, const char *s)
{
    if (!fonts[font].ok) return xcore_text(0, 0, s, 0, 0);
    double w = 0;
    int prev = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int cp = next_cp(&p);
        if (prev) w += fonts[font].scale * stbtt_GetCodepointKernAdvance(&fonts[font].info, prev, cp);
        glyph *g = get_glyph(font, cp);
        if (g) w += g->adv;
        prev = cp;
    }
    return (int)ceil(w);
}

int ui_text(int font, int x, int y, const char *s, col_t c)
{
    if (!fonts[font].ok) return xcore_text(x, y, s, c, 1);
    gamma_init();
    double pen = x;
    int prev = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int cp = next_cp(&p);
        if (prev) pen += fonts[font].scale * stbtt_GetCodepointKernAdvance(&fonts[font].info, prev, cp);
        glyph *g = get_glyph(font, cp);
        if (!g) continue;
        int gx = (int)lround(pen) + g->xo, gy = y + g->yo;
        if (g->w > 0 && g->h > 0) {
            mark(gx, gy, gx + g->w, gy + g->h);
            for (int j = 0; j < g->h; j++)
                for (int i = 0; i < g->w; i++) {
                    unsigned char v = g->bmp[j * g->w + i];
                    if (v) put(gx + i, gy + j, c, gam[v] / 255.0);
                }
        }
        pen += g->adv;
        prev = cp;
    }
    return (int)ceil(pen - x);
}

int ui_text_fit(int font, int x, int y, const char *s, col_t c, int maxw)
{
    if (ui_text_w(font, s) <= maxw) return ui_text(font, x, y, s, c);
    char b[512];
    size_t n = strlen(s);
    if (n > sizeof b - 4) n = sizeof b - 4;
    while (n > 0) {
        memcpy(b, s, n);
        while (n > 0 && ((unsigned char)b[n - 1] & 0xC0) == 0x80) n--;   /* not inside a UTF-8 sequence */
        memcpy(b + n, "...", 4);
        if (ui_text_w(font, b) <= maxw) break;
        n--;
    }
    if (n == 0) return 0;
    return ui_text(font, x, y, b, c);
}

#ifdef _WIN32
int ui_font_ascent(int font) { return fonts[font].ok ? fonts[font].ascent : 10; }
#else
int ui_font_ascent(int font) { return fonts[font].ok ? fonts[font].ascent : (xcore ? xcore->ascent : 10); }
#endif

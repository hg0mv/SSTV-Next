/* tpl.c - picture templates: model, drawing, files */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <glob.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "tpl.h"
#include "fileswap.h"
#include "imgio.h"

/* ------------------------------------------------------------- fonts --- */

#define MAXFONT 48
static char font_path[MAXFONT][300];
static const char *font_base[MAXFONT];
static int nfont = -1;
static unsigned char *font_data[MAXFONT];
static stbtt_fontinfo font_info[MAXFONT];
static int font_ok[MAXFONT];

static int cmp_base(const void *a, const void *b)
{
    const char *x = strrchr((const char *)a, '/'), *y = strrchr((const char *)b, '/');
    return strcasecmp(x ? x + 1 : (const char *)a, y ? y + 1 : (const char *)b);
}

static void font_scan(void)
{
    if (nfont >= 0) return;
    nfont = 0;
    static char found[400][300];
    int nf = 0;
    const char *home = getenv("HOME");
    char hp1[300], hp2[300];
    snprintf(hp1, sizeof hp1, "%s/.fonts/*.ttf", home ? home : "/nonexistent");
    snprintf(hp2, sizeof hp2, "%s/.local/share/fonts/*.ttf", home ? home : "/nonexistent");
#ifdef _WIN32
    char wp1[300], wp2[300];
    const char *wd = getenv("WINDIR"), *la = getenv("LOCALAPPDATA");
    snprintf(wp1, sizeof wp1, "%s/Fonts/*.ttf", wd && *wd ? wd : "C:/Windows");
    snprintf(wp2, sizeof wp2, "%s/Microsoft/Windows/Fonts/*.ttf", la && *la ? la : "C:/nonexistent");
    char wp3[MAX_PATH + 16];
    { char me[MAX_PATH]; DWORD l = GetModuleFileNameA(NULL, me, sizeof me); char *sl = l ? strrchr(me, '\\') : NULL; if (sl) *sl = 0;
      snprintf(wp3, sizeof wp3, "%s/fonts/*.ttf", l ? me : "."); }
    const char *pats[] = { wp1, wp2, wp3, hp1 };
    (void)hp2;
#else
    const char *pats[] = { "/usr/share/fonts/truetype/*/*.ttf", "/usr/share/fonts/truetype/*.ttf", "/usr/share/fonts/TTF/*.ttf",
                           "/usr/share/fonts/*/*.ttf", "/usr/local/share/fonts/*.ttf", hp1, hp2 };
#endif
    for (size_t k = 0; k < sizeof pats / sizeof pats[0]; k++) {
        glob_t g;
        if (glob(pats[k], 0, NULL, &g) != 0) continue;
        for (size_t i = 0; i < g.gl_pathc && nf < 400; i++) {
            int dup = 0;
            const char *b = strrchr(g.gl_pathv[i], '/') + 1;
            for (int j = 0; j < nf && !dup; j++) dup = !strcmp(strrchr(found[j], '/') + 1, b);
            if (!dup) snprintf(found[nf++], sizeof found[0], "%.290s", g.gl_pathv[i]);
        }
        globfree(&g);
    }
    qsort(found, (size_t)nf, sizeof found[0], cmp_base);
    /* good SSTV fonts first (bold, well readable), then the rest */
    static const char *pref[] = { "DejaVuSans-Bold.ttf", "Ubuntu-B.ttf", "LiberationSans-Bold.ttf", "FreeSansBold.ttf",
                                  "DejaVuSans.ttf", "Ubuntu-R.ttf", "DejaVuSerif-Bold.ttf", "LiberationSerif-Bold.ttf",
                                  "DejaVuSansMono-Bold.ttf", "LiberationMono-Bold.ttf", "DejaVuSans-BoldOblique.ttf",
                                  "Ubuntu-BI.ttf", "Poppins-Bold.ttf", "Ubuntu-C.ttf", "DejaVuSansCondensed-Bold.ttf",
                                  /* Windows */
                                  "arialbd.ttf", "segoeuib.ttf", "verdanab.ttf", "tahomabd.ttf", "impact.ttf", "arial.ttf",
                                  "segoeui.ttf", "georgiab.ttf", "timesbd.ttf", "courbd.ttf", "consolab.ttf", "comicbd.ttf" };
    char used[400] = { 0 };
    for (size_t p = 0; p < sizeof pref / sizeof pref[0] && nfont < MAXFONT; p++)
        for (int j = 0; j < nf; j++)
            if (!used[j] && !strcasecmp(strrchr(found[j], '/') + 1, pref[p])) {
                snprintf(font_path[nfont], sizeof font_path[0], "%.290s", found[j]); used[j] = 1; nfont++; break;
            }
    for (int j = 0; j < nf && nfont < MAXFONT; j++)
        if (!used[j]) snprintf(font_path[nfont++], sizeof font_path[0], "%.290s", found[j]);
    for (int i = 0; i < nfont; i++) font_base[i] = strrchr(font_path[i], '/') + 1;
}

int tpl_font_count(void) { font_scan(); return nfont; }
const char *tpl_font_name(int i) { font_scan(); return i >= 0 && i < nfont ? font_base[i] : ""; }

int tpl_font_index(const char *name)
{
    font_scan();
    for (int i = 0; i < nfont; i++) if (!strcmp(font_base[i], name)) return i;
    return nfont ? 0 : -1;                      /* missing font: the first one instead */
}

static stbtt_fontinfo *font_get(const char *name)
{
    int i = tpl_font_index(name);
    for (int tries = 0; i >= 0 && tries < nfont; tries++, i = (i + 1) % nfont) {
        if (font_ok[i] == 1) return &font_info[i];
        if (font_ok[i] == 0) {
            font_ok[i] = -1;
            FILE *f = fopen(font_path[i], "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 40L * 1024 * 1024) {
                font_data[i] = malloc((size_t)sz);
                if (font_data[i] && fread(font_data[i], 1, (size_t)sz, f) == (size_t)sz &&
                    stbtt_InitFont(&font_info[i], font_data[i], stbtt_GetFontOffsetForIndex(font_data[i], 0)))
                    font_ok[i] = 1;
            }
            fclose(f);
            if (font_ok[i] == 1) return &font_info[i];
        }
    }
    return NULL;
}

/* -------------------------------------------------------------- model --- */

void tpl_init(tpl *t, const char *name)
{
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
}

void tpl_clear(tpl *t)
{
    for (int i = 0; i < t->n; i++) { image_free(t->o[i].cache); t->o[i].cache = NULL; }
    t->n = 0;
}

static void rgb(unsigned char c[3], int r, int g, int b) { c[0] = (unsigned char)r; c[1] = (unsigned char)g; c[2] = (unsigned char)b; }

tobj *tpl_add(tpl *t, obj_type type)
{
    if (t->n >= TPL_MAXOBJ) return NULL;
    tobj *o = &t->o[t->n++];
    memset(o, 0, sizeof *o);
    o->type = type; o->alpha = 100;
    switch (type) {
    case OBJ_TEXT:
        o->x = 0.1; o->y = 0.4; o->h = 0.12; rgb(o->fill, 255, 230, 40); rgb(o->line, 0, 0, 0);
        o->outline = 1; o->lw = 2.5; o->shadow = 1; snprintf(o->text, sizeof o->text, "%%M");
        snprintf(o->font, sizeof o->font, "%s", tpl_font_name(0));
        break;
    case OBJ_RECT:
        o->x = 0.3; o->y = 0.3; o->w = 0.4; o->h = 0.3; rgb(o->fill, 20, 40, 120); rgb(o->line, 255, 255, 255);
        o->filled = 1; o->lw = 2; o->alpha = 70;
        break;
    case OBJ_ELLIPSE:
        o->x = 0.3; o->y = 0.3; o->w = 0.4; o->h = 0.3; rgb(o->fill, 200, 30, 30); rgb(o->line, 255, 255, 255);
        o->filled = 1; o->lw = 2;
        break;
    case OBJ_LINE:
        o->x = 0.2; o->y = 0.5; o->w = 0.6; o->h = 0; rgb(o->line, 255, 255, 255); rgb(o->fill, 255, 255, 255); o->lw = 3;
        break;
    case OBJ_IMAGE:
        o->x = 0.55; o->y = 0.05; o->w = 0.4; o->h = 0.4; rgb(o->line, 255, 255, 255); o->lw = 2;
        break;
    }
    return o;
}

void tpl_delete(tpl *t, int i)
{
    if (i < 0 || i >= t->n) return;
    image_free(t->o[i].cache);
    memmove(&t->o[i], &t->o[i + 1], (size_t)(t->n - i - 1) * sizeof t->o[0]);
    t->n--;
}

void tpl_move(tpl *t, int i, int d)
{
    int j = i + d;
    if (i < 0 || i >= t->n || j < 0 || j >= t->n) return;
    tobj x = t->o[i]; t->o[i] = t->o[j]; t->o[j] = x;
}

/* ------------------------------------------------------------- macros --- */

void tpl_expand(const char *src, const tpl_macros *m, char *out, size_t n)
{
    size_t o = 0;
    char tmp[64];
    struct tm g;
    time_t now = m && m->now ? m->now : time(NULL);
    gmtime_r(&now, &g);
    for (; *src && o + 1 < n; src++) {
        const char *ins = NULL;
        if (*src == '%' && src[1]) {
            switch (src[1]) {
            case 'M': case 'm': ins = m && m->my_call ? m->my_call : ""; break;
            case 'C': case 'c': ins = m && m->his_call ? m->his_call : ""; break;
            case 'R': case 'r': ins = m && m->rst ? m->rst : ""; break;
            case 'T': case 't': strftime(tmp, sizeof tmp, "%H:%M", &g); ins = tmp; break;
            case 'D': case 'd': strftime(tmp, sizeof tmp, "%Y-%m-%d", &g); ins = tmp; break;
            case 'S': case 's': ins = m && m->snr ? m->snr : ""; break;
            case 'O': case 'o': ins = m && m->offset ? m->offset : ""; break;
            case 'K': case 'k': ins = m && m->clock ? m->clock : ""; break;
            case 'Q': case 'q': ins = m && m->report ? m->report : ""; break;
            case 'F': case 'f': if (m && m->mhz > 0) snprintf(tmp, sizeof tmp, "%.3f", m->mhz); else tmp[0] = 0; ins = tmp; break;
            case '%': ins = "%"; break;
            }
        }
        if (ins) { for (; *ins && o + 1 < n; ins++) out[o++] = *ins; src++; }
        else out[o++] = *src;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ drawing --- */

static void blend(image *img, int x, int y, const unsigned char c[3], double a)
{
    if (x < 0 || y < 0 || x >= img->w || y >= img->h || a <= 0) return;
    if (a > 1) a = 1;
    unsigned char *p = img->px + ((size_t)y * img->w + x) * 3;
    for (int k = 0; k < 3; k++) p[k] = (unsigned char)(p[k] + (c[k] - p[k]) * a + 0.5);
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* next code point of a UTF-8 string (Latin-1 bytes are taken as they are) */
static int next_cp(const unsigned char **s)
{
    const unsigned char *p = *s;
    int c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (p[0] & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p++; }
    else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    *s = p;
    return c;
}

/* text into an 8-bit coverage mask with 'pad' pixels border; returns the text width */
static unsigned char *text_mask(const char *s, const char *font, int px, int pad, int *mw, int *mh, int *tw)
{
    stbtt_fontinfo *f = font_get(font);
    if (!f || px < 2) return NULL;
    float sc = stbtt_ScaleForPixelHeight(f, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(f, &asc, &desc, &gap);
    int base = (int)lroundf(asc * sc);
    /* width */
    double w = 0;
    int prev = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int cp = next_cp(&p), adv, lsb;
        if (prev) w += sc * stbtt_GetCodepointKernAdvance(f, prev, cp);
        stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
        w += sc * adv;
        prev = cp;
    }
    int W = (int)ceil(w) + 2 * pad + 2, H = px + 2 * pad + 2;
    unsigned char *m = calloc((size_t)W * H, 1);
    double x = pad;
    prev = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int cp = next_cp(&p), adv, lsb, x0, y0, x1, y1;
        if (prev) x += sc * stbtt_GetCodepointKernAdvance(f, prev, cp);
        stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBoxSubpixel(f, cp, sc, sc, (float)(x - floor(x)), 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            unsigned char *g = malloc((size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(f, g, gw, gh, gw, sc, sc, (float)(x - floor(x)), 0, cp);
            int ox = (int)floor(x) + x0, oy = pad + base + y0;
            for (int yy = 0; yy < gh; yy++)
                for (int xx = 0; xx < gw; xx++) {
                    int X = ox + xx, Y = oy + yy;
                    if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                    unsigned char v = g[yy * gw + xx], *d = &m[Y * W + X];
                    if (v > *d) *d = v;
                }
            free(g);
        }
        x += sc * adv;
        prev = cp;
    }
    *mw = W; *mh = H; *tw = (int)ceil(w);
    return m;
}

/* round dilation: every pixel takes the maximum within radius r */
static unsigned char *dilate(const unsigned char *m, int W, int H, int r)
{
    unsigned char *o = calloc((size_t)W * H, 1);
    int off[400][2], no = 0;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r && no < 400) { off[no][0] = dx; off[no][1] = dy; no++; }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int best = 0;
            for (int k = 0; k < no && best < 255; k++) {
                int X = x + off[k][0], Y = y + off[k][1];
                if (X >= 0 && Y >= 0 && X < W && Y < H && m[Y * W + X] > best) best = m[Y * W + X];
            }
            o[y * W + x] = (unsigned char)best;
        }
    return o;
}

static void draw_mask(image *img, const unsigned char *m, int W, int H, int ox, int oy, const unsigned char c[3], double a)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (m[y * W + x]) blend(img, ox + x, oy + y, c, a * m[y * W + x] / 255.0);
}

static const unsigned char shadow_c[3] = { 0, 0, 0 };

static void draw_text(tobj *o, image *img, const tpl_macros *mac)
{
    char s[256];
    tpl_expand(o->text, mac, s, sizeof s);
    int px = (int)lround(o->h * img->h);
    int r = o->outline ? (int)lround(o->lw * img->h / 256.0) : 0;
    if (o->outline && r < 1) r = 1;
    int sh = o->shadow ? (px / 14 > 1 ? px / 14 : 1) : 0;
    int pad = r + 2, W, H, tw;
    unsigned char *m = text_mask(s, o->font, px, pad, &W, &H, &tw);
    if (!m) { o->w = 0.05; return; }
    o->w = (double)tw / img->w;                     /* for the editor's selection box */
    int ox = (int)lround(o->x * img->w) - pad, oy = (int)lround(o->y * img->h) - pad;
    double a = o->alpha / 100.0;
    unsigned char *outl = r ? dilate(m, W, H, r) : NULL;
    if (sh) draw_mask(img, outl ? outl : m, W, H, ox + sh, oy + sh, shadow_c, 0.6 * a);
    if (outl) draw_mask(img, outl, W, H, ox, oy, o->line, a);
    draw_mask(img, m, W, H, ox, oy, o->fill, a);
    free(outl);
    free(m);
}

static void draw_rect(const tobj *o, image *img)
{
    double a = o->alpha / 100.0;
    int x0 = (int)lround(o->x * img->w), y0 = (int)lround(o->y * img->h);
    int x1 = (int)lround((o->x + o->w) * img->w), y1 = (int)lround((o->y + o->h) * img->h);
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    int b = (int)lround(o->lw * img->h / 256.0);
    if (o->shadow) {
        int s = img->h / 80 > 1 ? img->h / 80 : 1;
        for (int y = y0 + s; y < y1 + s; y++) for (int x = x0 + s; x < x1 + s; x++)
            if (x >= x1 || y >= y1) blend(img, x, y, shadow_c, 0.5 * a);
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int edge = b > 0 && (x < x0 + b || x >= x1 - b || y < y0 + b || y >= y1 - b);
            if (edge) blend(img, x, y, o->line, a);
            else if (o->filled) blend(img, x, y, o->fill, a);
        }
}

static void draw_ellipse(const tobj *o, image *img)
{
    double a = o->alpha / 100.0;
    double cx = (o->x + o->w / 2) * img->w, cy = (o->y + o->h / 2) * img->h;
    double rx = fabs(o->w) / 2 * img->w, ry = fabs(o->h) / 2 * img->h;
    if (rx < 1 || ry < 1) return;
    double b = o->lw * img->h / 256.0;
    int s = o->shadow ? (img->h / 80 > 1 ? img->h / 80 : 1) : 0;
    for (int pass = s ? 0 : 1; pass < 2; pass++) {
        double ox = pass ? 0 : s;
        for (int y = (int)floor(cy - ry - 1 + ox); y <= (int)ceil(cy + ry + 1 + ox); y++)
            for (int x = (int)floor(cx - rx - 1 + ox); x <= (int)ceil(cx + rx + 1 + ox); x++) {
                double u = (x + 0.5 - cx - ox) / rx, v = (y + 0.5 - cy - ox) / ry, d = sqrt(u * u + v * v);
                if (d < 1e-9) d = 1e-9;
                double gr = sqrt((u / (rx * d)) * (u / (rx * d)) + (v / (ry * d)) * (v / (ry * d)));
                double sd = (d - 1) / gr;                      /* signed distance in pixels, <0 inside */
                double inside = clamp01(0.5 - sd);
                if (!pass) { blend(img, x, y, shadow_c, 0.5 * a * inside); continue; }
                double ring = b > 0 ? inside - clamp01(0.5 - (sd + b)) : 0;
                if (o->filled) blend(img, x, y, o->fill, a * (inside - ring));
                if (ring > 0) blend(img, x, y, o->line, a * ring);
            }
    }
}

static void draw_line(const tobj *o, image *img)
{
    double a = o->alpha / 100.0;
    double x0 = o->x * img->w, y0 = o->y * img->h, x1 = (o->x + o->w) * img->w, y1 = (o->y + o->h) * img->h;
    double hw = fmax(0.5, o->lw * img->h / 256.0 / 2);
    double dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy;
    int s = o->shadow ? (img->h / 80 > 1 ? img->h / 80 : 1) : 0;
    for (int pass = s ? 0 : 1; pass < 2; pass++) {
        double of = pass ? 0 : s;
        for (int y = (int)floor(fmin(y0, y1) - hw - 1 + of); y <= (int)ceil(fmax(y0, y1) + hw + 1 + of); y++)
            for (int x = (int)floor(fmin(x0, x1) - hw - 1 + of); x <= (int)ceil(fmax(x0, x1) + hw + 1 + of); x++) {
                double px = x + 0.5 - of, py = y + 0.5 - of;
                double t = L2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / L2 : 0;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                double ex = px - (x0 + t * dx), ey = py - (y0 + t * dy), dist = sqrt(ex * ex + ey * ey);
                double cov = clamp01(hw + 0.5 - dist);
                if (cov > 0) blend(img, x, y, pass ? o->line : shadow_c, (pass ? 1 : 0.5) * a * cov);
            }
    }
}

static void draw_image(tobj *o, image *img)
{
    if (!o->cache || strcmp(o->cache_path, o->text)) {
        image_free(o->cache);
        o->cache = img_load(o->text);
        snprintf(o->cache_path, sizeof o->cache_path, "%s", o->text);
    }
    int x0 = (int)lround(o->x * img->w), y0 = (int)lround(o->y * img->h);
    int w = (int)lround(o->w * img->w), h = (int)lround(o->h * img->h);
    if (w < 2 || h < 2) return;
    double a = o->alpha / 100.0;
    if (o->shadow) {
        int s = img->h / 80 > 1 ? img->h / 80 : 1;
        for (int y = y0 + s; y < y0 + h + s; y++) for (int x = x0 + s; x < x0 + w + s; x++)
            if (x >= x0 + w || y >= y0 + h) blend(img, x, y, shadow_c, 0.5 * a);
    }
    if (o->cache) {
        image *r = img_fit(o->cache, w, h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) blend(img, x0 + x, y0 + y, r->px + ((size_t)y * w + x) * 3, a);
        image_free(r);
    } else {
        static const unsigned char grey[3] = { 90, 90, 90 };
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) blend(img, x0 + x, y0 + y, grey, 0.6 * a);
    }
    int b = (int)lround(o->lw * img->h / 256.0);
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++)
        if (b > 0 && (x < x0 + b || x >= x0 + w - b || y < y0 + b || y >= y0 + h - b)) blend(img, x, y, o->line, a);
}

void tpl_render(tpl *t, image *img, const tpl_macros *m)
{
    for (int i = 0; i < t->n; i++) {
        tobj *o = &t->o[i];
        switch (o->type) {
        case OBJ_TEXT: draw_text(o, img, m); break;
        case OBJ_RECT: draw_rect(o, img); break;
        case OBJ_ELLIPSE: draw_ellipse(o, img); break;
        case OBJ_LINE: draw_line(o, img); break;
        case OBJ_IMAGE: draw_image(o, img); break;
        }
    }
}

int tpl_hit(const tpl *t, double fx, double fy, double aspect)
{
    const double tol = 0.015;
    for (int i = t->n - 1; i >= 0; i--) {
        const tobj *o = &t->o[i];
        if (o->type == OBJ_LINE) {
            double x0 = o->x, y0 = o->y * aspect, dx = o->w, dy = o->h * aspect, px = fx, py = fy * aspect;
            double L2 = dx * dx + dy * dy, s = L2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / L2 : 0;
            s = s < 0 ? 0 : s > 1 ? 1 : s;
            double ex = px - (x0 + s * dx), ey = py - (y0 + s * dy);
            if (sqrt(ex * ex + ey * ey) < tol + o->lw / 512.0) return i;
            continue;
        }
        double x0 = fmin(o->x, o->x + o->w), x1 = fmax(o->x, o->x + o->w);
        double y0 = fmin(o->y, o->y + o->h), y1 = fmax(o->y, o->y + o->h);
        if (fx >= x0 - tol && fx <= x1 + tol && fy >= y0 - tol && fy <= y1 + tol) return i;
    }
    return -1;
}

/* -------------------------------------------------------------- files --- */

static void path_of(char *out, size_t n, const char *dir, const char *name) { snprintf(out, n, "%.250s/%.48s.tpl", dir, name); }

int tpl_save(const tpl *t, const char *dir)
{
    if (!t->name[0]) return -1;
    mkdir_p(dir);
    char p[320], tmp[330];
    path_of(p, sizeof p, dir, t->name);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    static const char *tn[] = { "text", "rect", "ellipse", "line", "image" };
    fprintf(f, "# SSTV Next template\nname=%s\n", t->name);
    for (int i = 0; i < t->n; i++) {
        const tobj *o = &t->o[i];
        fprintf(f, "type=%s x=%.5f y=%.5f w=%.5f h=%.5f fill=%02x%02x%02x line=%02x%02x%02x filled=%d lw=%.2f alpha=%d shadow=%d outline=%d font=%s text=%s\n",
                tn[o->type], o->x, o->y, o->w, o->h, o->fill[0], o->fill[1], o->fill[2], o->line[0], o->line[1], o->line[2],
                o->filled, o->lw, o->alpha, o->shadow, o->outline, o->font[0] ? o->font : "-", o->text);
    }
    int ok = fclose(f) == 0 && file_replace(tmp, p) == 0;
    return ok ? 0 : -1;
}

static const char *field(const char *l, const char *key)
{
    size_t k = strlen(key);
    for (const char *p = l; (p = strstr(p, key)); p++)
        if ((p == l || p[-1] == ' ') && p[k] == '=') return p + k + 1;
    return NULL;
}

static void hex3(const char *s, unsigned char c[3])
{
    unsigned v = 0;
    if (s && sscanf(s, "%6x", &v) == 1) { c[0] = (unsigned char)(v >> 16); c[1] = (unsigned char)(v >> 8); c[2] = (unsigned char)v; }
}

int tpl_load(tpl *t, const char *dir, const char *name)
{
    char p[320], l[600];
    path_of(p, sizeof p, dir, name);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    tpl_clear(t);
    tpl_init(t, name);
    while (fgets(l, sizeof l, f)) {
        char *nl = strchr(l, '\n'); if (nl) *nl = 0;
        if (l[0] == '#' || !l[0]) continue;
        if (!strncmp(l, "name=", 5)) { snprintf(t->name, sizeof t->name, "%.47s", l + 5); continue; }
        const char *ty = field(l, "type");
        if (!ty || t->n >= TPL_MAXOBJ) continue;
        /* text= is the last field and may contain spaces: cut it off first */
        char *tx = strstr(l, " text=");
        char text[160] = "";
        if (tx) { snprintf(text, sizeof text, "%s", tx + 6); *tx = 0; }
        obj_type type = !strncmp(ty, "rect", 4) ? OBJ_RECT : !strncmp(ty, "ellipse", 7) ? OBJ_ELLIPSE :
                        !strncmp(ty, "line", 4) ? OBJ_LINE : !strncmp(ty, "image", 5) ? OBJ_IMAGE : OBJ_TEXT;
        tobj *o = tpl_add(t, type);
        const char *v;
        if ((v = field(l, "x"))) o->x = atof(v);
        if ((v = field(l, "y"))) o->y = atof(v);
        if ((v = field(l, "w"))) o->w = atof(v);
        if ((v = field(l, "h"))) o->h = atof(v);
        hex3(field(l, "fill"), o->fill);
        hex3(field(l, "line"), o->line);
        if ((v = field(l, "filled"))) o->filled = atoi(v);
        if ((v = field(l, "lw"))) o->lw = atof(v);
        if ((v = field(l, "alpha"))) o->alpha = atoi(v);
        if ((v = field(l, "shadow"))) o->shadow = atoi(v);
        if ((v = field(l, "outline"))) o->outline = atoi(v);
        if ((v = field(l, "font"))) { sscanf(v, "%63s", o->font); if (!strcmp(o->font, "-")) o->font[0] = 0; }
        snprintf(o->text, sizeof o->text, "%s", text);
    }
    fclose(f);
    snprintf(t->name, sizeof t->name, "%s", name);     /* the file name is the name */
    return 0;
}

static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

int tpl_list(const char *dir, char names[][48], int max)
{
    char pat[300];
    snprintf(pat, sizeof pat, "%.280s/*.tpl", dir);
    glob_t g;
    int n = 0;
    if (glob(pat, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && n < max; i++) {
            const char *b = strrchr(g.gl_pathv[i], '/') + 1;
            size_t L = strlen(b) - 4;
            if (L >= 48) L = 47;
            memcpy(names[n], b, L); names[n][L] = 0; n++;
        }
        globfree(&g);
    }
    qsort(names, (size_t)n, sizeof names[0], cmp_name);
    return n;
}

int tpl_remove(const char *dir, const char *name)
{
    char p[320];
    path_of(p, sizeof p, dir, name);
    return unlink(p);
}

/* reply with the signal report of the received picture (%Q) */
void tpl_make_report(const char *dir)
{
    tpl t; tobj *o;
    tpl_init(&t, "Report");
    o = tpl_add(&t, OBJ_RECT); o->x = 0; o->y = 0.66; o->w = 1; o->h = 0.34; rgb(o->fill, 0, 0, 0); o->alpha = 60; o->lw = 0;
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.035; o->y = 0.68; o->h = 0.11; snprintf(o->text, sizeof o->text, "%%C de %%M");
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.035; o->y = 0.805; o->h = 0.068; rgb(o->fill, 120, 230, 255); o->lw = 2;
    snprintf(o->text, sizeof o->text, "%%Q");
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.035; o->y = 0.895; o->h = 0.075; rgb(o->fill, 255, 255, 255); o->lw = 2;
    snprintf(o->text, sizeof o->text, "UR %%R   TNX 73!");
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.70; o->y = 0.025; o->h = 0.065; rgb(o->fill, 255, 255, 255); o->lw = 2;
    snprintf(o->text, sizeof o->text, "%%T UTC");
    tpl_save(&t, dir); tpl_clear(&t);
}

void tpl_make_defaults(const char *dir, const char *old_overlay)
{
    char names[4][48];
    if (tpl_list(dir, names, 4) > 0) return;
    tpl t; tobj *o;

    tpl_init(&t, "Call");                       /* what the old single text overlay did */
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.025; o->y = 0.02; o->h = 0.13; o->lw = 3;
    snprintf(o->text, sizeof o->text, "%s", old_overlay && old_overlay[0] ? old_overlay : "%M");
    tpl_save(&t, dir); tpl_clear(&t);

    tpl_init(&t, "CQ");
    o = tpl_add(&t, OBJ_RECT); o->x = 0; o->y = 0; o->w = 1; o->h = 0.21; rgb(o->fill, 0, 0, 0); o->alpha = 55; o->lw = 0;
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.035; o->y = 0.035; o->h = 0.14; snprintf(o->text, sizeof o->text, "CQ CQ de %%M");
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.035; o->y = 0.89; o->h = 0.075; rgb(o->fill, 255, 255, 255); o->lw = 2;
    snprintf(o->text, sizeof o->text, "%%D  %%T UTC");
    tpl_save(&t, dir); tpl_clear(&t);

    tpl_init(&t, "QSO reply");
    o = tpl_add(&t, OBJ_RECT); o->x = 0; o->y = 0.73; o->w = 1; o->h = 0.27; rgb(o->fill, 0, 0, 0); o->alpha = 60; o->lw = 0;
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.035; o->y = 0.75; o->h = 0.12; snprintf(o->text, sizeof o->text, "%%C de %%M");
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.035; o->y = 0.885; o->h = 0.085; rgb(o->fill, 255, 255, 255); o->lw = 2;
    snprintf(o->text, sizeof o->text, "UR %%R   TNX 73!");
    o = tpl_add(&t, OBJ_TEXT); o->x = 0.70; o->y = 0.025; o->h = 0.065; rgb(o->fill, 255, 255, 255); o->lw = 2;
    snprintf(o->text, sizeof o->text, "%%T UTC");
    tpl_save(&t, dir); tpl_clear(&t);
}

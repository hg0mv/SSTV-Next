#define _POSIX_C_SOURCE 200809L
/*
 * SSTV Next - native X11 SSTV transceiver
 * Build: make   (needs libX11, libasound, pthreads)
 *
 * Live receive with automatic mode detection (VIS), progressive picture,
 * slant correction and automatic saving; transmit of an image from a folder
 * with a callsign overlay. Radio control (rigctld), sound card handling and
 * the ADIF log are the ones proven in DigiPan Next.
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <glob.h>
#include <ctype.h>
#include "sstv.h"
#include "image.h"
#include "imgio.h"
#include "tx.h"
#include "live.h"
#include "audio.h"
#include "rig.h"
#include "adif.h"
#include "config.h"
#include "tpl.h"
#include "ui.h"
#include "drm_tx.h"
#include "mot.h"
#include "rsfile.h"
#include "fskid.h"
#include "version.h"
#ifdef _WIN32
#include <shellapi.h>
#else
#include <sys/wait.h>
#endif

#define APP        "SSTV Next " SSTVNEXT_VERSION
#define SUPPORT_URL "https://ko-fi.com/hg0mv"
#define AUDIO_RATE 11025
#define W0 1280
#define H0 820
#define MIN_W 1100
#define MIN_H 720
#define FFT_N 2048
#define SPEC_RING 4096
#define TX_LIMIT_EXTRA 20       /* s: automatic stop beyond the mode's length */

typedef struct { col_t bg, panel, panel2, grid, text, muted, green, cyan, blue, yellow, red, black, accent, accent2, tx, inset, hi; } Palette;
static Display *dpy;
static Window win;
static GC gc;
static Visual *vis;
static int scr, depth, width = W0, height = H0;
static Palette c;
static volatile sig_atomic_t quit_sig = 0;

/* ---- audio / receive ---- */
static live *lv;
static char audio_devices[32][128];
static int audio_device_count = 0, audio_device_selected = -1, audio_menu = 0;
static char audio_status[160] = "Audio input not connected";
static char test_wav[300] = "", tx_wav[300] = "";
static double test_speed = 1.0;
static pthread_mutex_t spec_mutex = PTHREAD_MUTEX_INITIALIZER;
static float spec_ring[SPEC_RING];
static unsigned long spec_wr = 0;
static double spec_db[FFT_N / 2 + 1];

static image *rx_img = NULL;          /* last live picture */
static unsigned rx_gen = 0;
static live_status rx_st;
static image *view_img = NULL;        /* a saved picture opened from the gallery */
static char view_name[200] = "";
static char view_meta[2048] = "";      /* its reception data (PNG text), "" if none */
/* signal report for the reply (%S %O %K %Q): the last analog picture received */
typedef struct { int valid; double snr, offset, ppm; int syncs, lines; char call[16]; } Report;
static Report last_rep;

/* Own sound card clock (ppm, the decoder's sign): the median slant of the last good
 * pictures. Most stations are close to right, so what they all seem to have is ours.
 * NAN until 5 pictures were measured. */
#define CLK_MIN 5
static double own_clock(void)
{
    if (cfg.nclk < CLK_MIN) return NAN;
    double v[16];
    int n = cfg.nclk;
    memcpy(v, cfg.clk, n * sizeof(double));
    for (int i = 1; i < n; i++) { double t = v[i]; int k = i - 1; while (k >= 0 && v[k] > t) { v[k + 1] = v[k]; k--; } v[k + 1] = t; }
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* a good analog reception: its slant goes into the history */
static void clock_learn(const live_status *st)
{
    if (!st->mode || st->lines < sstv_nlines(st->mode) / 2 || st->syncs < st->lines * 6 / 10) return;
    if (!isnan(st->snr) && st->snr < 6) return;
    if (fabs(st->clock_ppm) > 8000) return;
    if (cfg.nclk == 16) { memmove(cfg.clk, cfg.clk + 1, 15 * sizeof(double)); cfg.nclk--; }
    cfg.clk[cfg.nclk++] = st->clock_ppm;
    config_save();
    live_set_clock(lv, own_clock());
}

/* our transmission at the right speed although the sound card's clock is off:
   resampled by the own clock error (windowed sinc) */
static float *clock_fix(float *x, long *n)
{
    double e = own_clock();
    if (!cfg.txclk || isnan(e) || fabs(e) < 20) return x;
    double k = 1 + e * 1e-6;                     /* we play k times slower than we think */
    long m = (long)(*n / k);
    float *y = malloc(sizeof(float) * (m > 0 ? m : 1));
    if (!y) return x;
    const int H = 8;
    for (long j = 0; j < m; j++) {
        double t = j * k;
        long i0 = (long)floor(t);
        double s = 0;
        for (long i = i0 - H + 1; i <= i0 + H; i++) {
            if (i < 0 || i >= *n) continue;
            double d = t - i, w = d == 0 ? 1 : sin(M_PI * d) / (M_PI * d);
            w *= 0.42 + 0.5 * cos(M_PI * d / H) + 0.08 * cos(2 * M_PI * d / H);   /* Blackman */
            s += x[i] * w;
        }
        y[j] = (float)s;
    }
    free(x);
    *n = m;
    return y;
}
static int rx_force = -1;             /* -1: auto, else index into the mode table */
#define NMODEIDS sstv_mode_count()
#define MODE_ID(i) (sstv_mode_at(i)->id)

/* gallery of received pictures */
#define NGAL 24
#define TW 128
#define TH 102
typedef struct { char path[300]; image *thumb; char snr[12]; } GalItem;
static GalItem gal[NGAL];
static int ngal = 0, gal_scroll = 0;

/* ---- transmit ---- */
static char tx_files[512][300];
static int tx_nfiles = 0, tx_idx = 0, tx_mode_i = 1;
static image *tx_src = NULL;          /* file as loaded */
static image *tx_bg = NULL;           /* background at mode size */
static image *tx_img = NULL;          /* background + template: what goes out */
static pthread_mutex_t tx_img_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int tx_req = 0, tx_abort = 0, tx_state = 0, tx_running = 0;
static atomic_long tx_done = 0, tx_total = 1;
static pthread_t tx_thread;
static pthread_mutex_t tx_msg_mutex = PTHREAD_MUTEX_INITIALIZER;
static char tx_msg[128] = "";
static char last_mode_name[32] = "";  /* for the log comment */

/* ---- templates / editor ---- */
static char tpl_dir[300];
static tpl cur_tpl;
static char tpl_names[64][48];
static int tpl_count = 0;
static int edit_mode = 0, sel = -1, drag = 0, ed_focus = 0, tpl_dirty = 0, del_confirm = 0;
static int ed_fresh = 0;               /* new text object: the first key replaces the default text */
static double drag_fx, drag_fy;
static tobj drag_start;
static int ex0, ey0, ew = 1, eh = 1;  /* editor canvas on screen */
static char ed_msg[120] = "";

/* ---- dialogs / log ---- */
static int rig_dlg = 0, rig_focus = 1, rig_scroll = 0, cli_rig = 0;
static int *rig_vis = NULL, rig_nvis = 0;
static char rig_filter[48] = "";
static char his_call[24] = "", lg_rs[8] = "595", lg_rr[8] = "595", lg_freq[16] = "";
static int log_focus = -1, log_dlg = 0, log_sel = 0, log_scroll = 0, log_confirm = 0, log_dupe_hard = 0, freq_auto = 0;
static time_t qso_start = 0;
static char log_msg[120] = "", log_dupe[96] = "";
static time_t log_msg_until = 0;
static int log_msg_ok = 1;

static void msleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static double mono_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static void tx_set_msg(const char *m) { pthread_mutex_lock(&tx_msg_mutex); snprintf(tx_msg, sizeof tx_msg, "%.120s", m); pthread_mutex_unlock(&tx_msg_mutex); }
static int tx_active(void) { return atomic_load(&tx_req) || atomic_load(&tx_state) != 0; }
static const sstv_mode *tx_mode(void) { return sstv_mode_at(tx_mode_i < NMODEIDS ? tx_mode_i : 0); }
/* the entry after the analog modes is the digital mode (HamDRM) */
static const int drm_sizes[][2] = {{320, 240}, {480, 360}, {640, 480}, {800, 600}};
#define NDRMSIZE 4
static const int drm_kbs[] = {6, 8, 12, 16, 24, 32, 48, 64};
#define NDRMKB 8
static int tx_jpeg_len, tx_jpeg_q;                 /* size of the picture as it would be sent */
/* digital transmissions we made (to answer repeat requests) */
typedef struct { unsigned tid; char name[64]; unsigned char *data; int len; drm_params p; } DrmSent;
#define NSENT 6
static DrmSent drm_sent[NSENT];
static int drm_nsent;
static int tx_kind;                                /* 0 picture, 1 repeat request (bsr.bin), 2 fix */
static char tx_bsr_text[4096]; static int tx_bsr_len;
static int fix_idx = -1, fix_n; static int fix_list[4096]; static char fix_from[10];
static unsigned seen_bsr, seen_heard;
static int tx_is_drm(void) { return tx_mode_i == NMODEIDS; }
static int tx_w(void) { return tx_is_drm() ? drm_sizes[cfg.drm_size][0] : tx_mode()->width; }
static int tx_h(void) { return tx_is_drm() ? drm_sizes[cfg.drm_size][1] : tx_mode()->height; }
static const char *tx_name(void) { return tx_is_drm() ? "Digital DRM" : tx_mode()->name; }
static drm_params tx_drm_params(void)
{
    drm_params p = { cfg.drm_rob, cfg.drm_so, cfg.drm_qam, cfg.drm_prot, 1, "" };
    snprintf(p.call, sizeof p.call, "%.9s", cfg.call);
    return p;
}
static void tx_mode_set(int i)
{
    tx_mode_i = (i + NMODEIDS + 1) % (NMODEIDS + 1);
    snprintf(cfg.tx_mode, sizeof cfg.tx_mode, "%s", tx_is_drm() ? "drm" : MODE_ID(tx_mode_i));
}

/* ================================================================ drawing */

static col_t rgb(unsigned r, unsigned g, unsigned b) { return RGB(r, g, b); }
static void init_palette(void)
{
    /* dark theme: deep blue-grey surfaces, blue accent for controls, teal for receive, orange/red for transmit */
    c.bg = RGB(13, 17, 23);     c.panel = RGB(22, 27, 34);   c.panel2 = RGB(33, 39, 48);
    c.grid = RGB(48, 56, 68);   c.text = RGB(230, 237, 243); c.muted = RGB(178, 187, 197);
    c.green = RGB(86, 211, 100); c.cyan = RGB(88, 166, 255); c.blue = RGB(31, 111, 235);
    c.yellow = RGB(242, 204, 96); c.red = RGB(248, 81, 73);  c.black = RGB(1, 4, 9);
    c.accent = RGB(47, 129, 247); c.accent2 = RGB(45, 212, 191); c.tx = RGB(240, 136, 62);
    c.inset = RGB(10, 13, 18);  c.hi = RGB(52, 61, 74);
}
/* Drawing goes into ui.c's framebuffer. begin_buf()/end_buf() mark a region that is
 * sent to the window in one piece (no flicker); drawing is clipped to it. */
static int bx0, by0, bw0, bh0, in_buf = 0;
static void fill(int x, int y, int w, int h, col_t v) { if (w > 0 && h > 0) ui_fill(x, y, w, h, v); }
static void begin_buf(int x, int y, int w, int h)
{
    if (w < 1 || h < 1) return;
    bx0 = x; by0 = y; bw0 = w; bh0 = h; in_buf = 1;
    ui_clip(x, y, w, h);
}
static void end_buf(void)
{
    if (!in_buf) return;
    ui_noclip();
    ui_present(bx0, by0, bw0, bh0);
    in_buf = 0;
}
static void flush_ui(void) { ui_flush(); }

/* rounded button; active = accent coloured. The label is centred (or cut with ...) */
static void button(int x, int y, int w, int h, const char *s, int active)
{
    int r = h >= 30 ? 7 : 6;
    if (active) {
        ui_rrect_grad(x, y, w, h + 1, r, RGB(56, 139, 253), RGB(31, 106, 222));
        ui_rrect_line(x, y, w, h + 1, r, RGB(121, 184, 255), 0.55);
    } else {
        ui_rrect_grad(x, y, w, h + 1, r, RGB(41, 48, 59), RGB(31, 37, 46));
        ui_rrect_line(x, y, w, h + 1, r, c.grid, 1);
        ui_fill_a(x + r, y + 1, w - 2 * r, 1, RGB(255, 255, 255), 0.06);     /* top light edge */
    }
    int tw = ui_text_w(F_REG, s), ty = y + h / 2 + 5;
    if (tw <= w - 12) ui_text(F_REG, x + (w - tw) / 2, ty, s, active ? RGB(255, 255, 255) : c.text);
    else ui_text_fit(F_REG, x + 6, ty, s, active ? RGB(255, 255, 255) : c.text, w - 12);
}
/* ---- one-line text editing: the caret can be put anywhere in the focused field ---- */
static const char *caret_buf = NULL;     /* the field the caret is in */
static size_t caret = 0;                 /* byte offset in it */
static size_t caret_get(const char *s)
{
    size_t n = strlen(s);
    if (s != caret_buf || caret > n) { caret_buf = s; caret = n; }   /* a new field: at the end */
    return caret;
}
static size_t u8_prev(const char *s, size_t i)
{
    if (!i) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}
static size_t u8_next(const char *s, size_t i)
{
    if (!s[i]) return i;
    i++;
    while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}
/* width of s[a..b) */
static int sub_w(int font, const char *s, size_t a, size_t b)
{
    char t[512];
    size_t l = b > a ? b - a : 0;
    if (l >= sizeof t) l = sizeof t - 1;
    memcpy(t, s + a, l); t[l] = 0;
    return ui_text_w(font, t);
}
/* first byte shown in a field of inner width w, so that position cur is visible */
static size_t field_start(int font, const char *s, size_t cur, int w)
{
    size_t st = 0;
    while (st < cur && sub_w(font, s, st, cur) > w) st = u8_next(s, st);
    return st;
}
/* the text of a field from x (left edge of the box): the caret where it is when focused,
   otherwise the end of a long text */
static void field_text(int font, int x, int y, int w, int h, const char *s, int focus, col_t col)
{
    size_t cur = focus ? caret_get(s) : strlen(s), st = field_start(font, s, cur, w - 16);
    ui_text_fit(font, x + 7, y + h / 2 + 5, s + st, col, w - 14);
    if (focus) fill(x + 8 + sub_w(font, s, st, cur), y + 6, 1, h - 11, c.accent);
}
/* a click in a field: the caret goes to the nearest character boundary */
static void field_click(int font, int x, int w, const char *s, int mx)
{
    size_t cur = caret_get(s), st = field_start(font, s, cur, w - 16), best = st;
    int bd = 1 << 30;
    for (size_t i = st;; i = u8_next(s, i)) {
        int d = abs(x + 7 + sub_w(font, s, st, i) - mx);
        if (d < bd) { bd = d; best = i; }
        if (!s[i]) break;
    }
    caret_buf = s; caret = best;
}
/* editing keys (arrows, Home, End, BackSpace, Delete) and typed characters, inserted at the
   caret. filt: accepts (and may change) a character, NULL: any printable one.
   Returns 1 if the text changed. */
static int line_edit(char *s, size_t size, KeySym ks, const char *kb, int kl, int (*filt)(char *))
{
    size_t n = strlen(s), cur = caret_get(s);
    switch (ks) {
    case XK_Left: case XK_KP_Left:   caret = u8_prev(s, cur); return 0;
    case XK_Right: case XK_KP_Right: caret = u8_next(s, cur); return 0;
    case XK_Home: case XK_KP_Home:   caret = 0; return 0;
    case XK_End: case XK_KP_End:     caret = n; return 0;
    case XK_BackSpace:
        if (!cur) return 0;
        { size_t p = u8_prev(s, cur); memmove(s + p, s + cur, n - cur + 1); caret = p; }
        return 1;
    case XK_Delete: case XK_KP_Delete:
        if (cur >= n) return 0;
        { size_t q = u8_next(s, cur); memmove(s + cur, s + q, n - q + 1); }
        return 1;
    }
    int chg = 0;
    for (int i = 0; i < kl; i++) {
        char ch = kb[i];
        unsigned char u = (unsigned char)ch;
        if (filt ? !filt(&ch) : !((u >= 32 && u <= 126) || u >= 160)) continue;
        if (n + 1 >= size) break;
        memmove(s + caret + 1, s + caret, n - caret + 1);
        s[caret++] = ch; n++; chg = 1;
    }
    return chg;
}
static int filt_call(char *ch)
{
    if (*ch >= 'a' && *ch <= 'z') *ch = (char)(*ch - 32);
    return (*ch >= 'A' && *ch <= 'Z') || (*ch >= '0' && *ch <= '9') || *ch == '/';
}
static int filt_digit(char *ch) { return *ch >= '0' && *ch <= '9'; }
static int filt_freq(char *ch) { return (*ch >= '0' && *ch <= '9') || *ch == '.'; }
static int filt_ascii(char *ch) { return *ch >= 32 && *ch <= 126; }
static int filt_name(char *ch) { return isalnum((unsigned char)*ch) || *ch == ' ' || *ch == '-' || *ch == '_'; }

/* text input box; a caret when focused */
static void field_box(int x, int y, int w, int h, const char *s, int focus)
{
    if (focus) ui_rrect_line(x - 2, y - 2, w + 5, h + 5, 7, c.accent, 0.35);
    ui_rrect(x, y, w + 1, h + 1, 5, c.inset, 1);
    ui_rrect_line(x, y, w + 1, h + 1, 5, focus ? c.accent : c.grid, 1);
    field_text(F_REG, x, y, w, h, s, focus, c.text);
}
/* panel: rounded surface with a title (accent mark + bold label) */
static void panel(int x, int y, int w, int h, const char *title, col_t accent)
{
    fill(x, y, w, h, c.bg);
    ui_rrect(x, y, w, h, 10, c.panel, 1);
    ui_rrect_line(x, y, w, h, 10, RGB(38, 45, 55), 1);
    if (title) {
        ui_rrect(x + 12, y + 9, 3, 13, 1, accent, 1);
        ui_text(F_SMALLB, x + 21, y + 20, title, RGB(201, 209, 217));
    }
}
/* small rounded label */
static int chip(int x, int y, const char *s, col_t fg, col_t bg)
{
    int w = ui_text_w(F_SMALLB, s) + 14;
    ui_rrect(x, y, w, 18, 9, bg, 1);
    ui_text(F_SMALLB, x + 7, y + 13, s, fg);
    return w;
}
static int hit(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }
static const char *short_path(const char *p)        /* $HOME/... -> ~/... */
{
    static char b[2][300]; static int k = 0; k ^= 1;
    const char *h = getenv("HOME"); size_t n = h ? strlen(h) : 0;
    if (n && !strncmp(p, h, n) && (p[n] == '/' || !p[n])) snprintf(b[k], sizeof b[k], "~%.290s", p + n);
    else snprintf(b[k], sizeof b[k], "%.290s", p);
    return b[k];
}

static void put_rgb(const image *img, int x, int y, int dw, int dh) { ui_image(img, x, y, dw, dh); }
/* largest size of img that fits in w x h (integer zoom when it can be) */
static void fit_size(const image *img, int w, int h, int *dw, int *dh)
{
    double s = fmin((double)w / img->w, (double)h / img->h);
    if (s >= 1) s = floor(s);
    *dw = (int)(img->w * s); *dh = (int)(img->h * s);
}

/* ================================================================ layout */

#define LX 12
#define LY 104
#define LPW 664
static int log_bar_y(void) { return height - 66; }
static int lp_h(void) { return log_bar_y() - 12 - LY - 40; }       /* picture panel; progress row below */
static int RX0(void) { return LX + LPW + 14; }
static int RW(void) { return width - RX0() - 12; }
#define TUN_Y LY
#define TUN_H 120
#define TXP_Y (LY + TUN_H + 10)
#define TXP_H 270
#define GAL_Y (TXP_Y + TXP_H + 10)

/* ============================================================== transmit */

static int cmpstr(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void tx_scan(void)
{
    static const char *ext[] = { "png", "PNG", "jpg", "JPG", "jpeg", "JPEG", "bmp", "BMP", "ppm", "gif", "GIF" };
    tx_nfiles = 0;
    for (size_t k = 0; k < sizeof ext / sizeof ext[0]; k++) {
        char pat[320]; snprintf(pat, sizeof pat, "%.290s/*.%s", cfg.tx_dir, ext[k]);
        glob_t g; if (glob(pat, 0, NULL, &g) != 0) continue;
        for (size_t i = 0; i < g.gl_pathc && tx_nfiles < 512; i++) snprintf(tx_files[tx_nfiles++], sizeof tx_files[0], "%s", g.gl_pathv[i]);
        globfree(&g);
    }
    qsort(tx_files, (size_t)tx_nfiles, sizeof tx_files[0], cmpstr);
    if (tx_idx >= tx_nfiles) tx_idx = 0;
}

/* built-in test card when the TX folder is empty */
static image *test_card(void)
{
    image *im = image_new(320, 256);
    static const unsigned char bars[8][3] = { {255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{0,0,0} };
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 320; x++) {
            unsigned char *p = im->px + ((size_t)y * 320 + x) * 3;
            if (y < 150) memcpy(p, bars[x / 40], 3);
            else if (y < 190) p[0] = p[1] = p[2] = (unsigned char)(x * 255 / 319);
            else { p[0] = 20; p[1] = 40; p[2] = (unsigned char)(80 + y / 2); }
        }
    return im;
}

/* the report for %S %O %K %Q: the picture being viewed (if it has reception data), else
   the last analog picture received */
static int report_get(Report *r)
{
    if (view_img && view_meta[0]) {
        char v[32];
        memset(r, 0, sizeof *r);
        r->snr = img_meta_get(view_meta, "snr", v, sizeof v) ? atof(v) : NAN;
        r->offset = img_meta_get(view_meta, "offset", v, sizeof v) ? atof(v) : NAN;
        r->ppm = img_meta_get(view_meta, "ppm", v, sizeof v) ? atof(v) : NAN;
        if (img_meta_get(view_meta, "sync", v, sizeof v)) r->syncs = atoi(v);
        if (img_meta_get(view_meta, "lines", v, sizeof v)) { char *sl = strchr(v, '/'); r->lines = sl ? atoi(sl + 1) : 0; }
        img_meta_get(view_meta, "call", r->call, sizeof r->call);
        r->valid = 1;
        return 1;
    }
    *r = last_rep;
    return r->valid;
}

static void macros_now(tpl_macros *m)
{
    static char snr[16], off[16], clk[20], rep[80];
    m->my_call = cfg.call; m->his_call = his_call; m->rst = lg_rs;
    m->mhz = atof(lg_freq); m->now = time(NULL);
    Report r;
    snr[0] = off[0] = clk[0] = rep[0] = 0;
    if (report_get(&r)) {
        if (!isnan(r.snr)) snprintf(snr, sizeof snr, "%.0f dB", r.snr > 30 ? 30 : r.snr);
        if (!isnan(r.offset)) snprintf(off, sizeof off, "%+.0f Hz", r.offset);
        if (!isnan(r.ppm)) snprintf(clk, sizeof clk, "%+.0f ppm", r.ppm);
        int o = 0;                                /* compact: it has to fit one line */
        if (!isnan(r.snr)) o += snprintf(rep + o, sizeof rep - o, "SNR %.0fdB  ", r.snr > 30 ? 30 : r.snr);
        if (!isnan(r.offset)) o += snprintf(rep + o, sizeof rep - o, "QRG %+.0fHz  ", r.offset);
        if (!isnan(r.ppm)) o += snprintf(rep + o, sizeof rep - o, "CLK %+.0fppm", r.ppm);
        while (o > 0 && rep[o - 1] == ' ') rep[--o] = 0;
    }
    m->snr = snr; m->offset = off; m->clock = clk; m->report = rep;
}

/* background + template -> the picture that goes out (and the preview) */
static void tx_render(void)
{
    if (!tx_bg) return;
    image *im = image_new(tx_bg->w, tx_bg->h);
    memcpy(im->px, tx_bg->px, (size_t)im->w * im->h * 3);
    tpl_macros m; macros_now(&m);
    tpl_render(&cur_tpl, im, &m);
    if (tx_is_drm()) {
        int len = 0, q = 0;
        free(img_jpeg_fit(im, drm_kbs[cfg.drm_kb] * 1024, &len, &q));
        tx_jpeg_len = len; tx_jpeg_q = q;
    }
    pthread_mutex_lock(&tx_img_mutex); image_free(tx_img); tx_img = im; pthread_mutex_unlock(&tx_img_mutex);
}

/* rebuild the picture that will be sent (file or mode changed) */
static void tx_prepare(int reload)
{
    if (reload) {
        image_free(tx_src); tx_src = NULL;
        if (tx_nfiles > 0) tx_src = img_load(tx_files[tx_idx]);
        if (!tx_src) tx_src = test_card();
    }
    image_free(tx_bg);
    tx_bg = img_fit(tx_src, tx_w(), tx_h());
    tx_render();
}

static const char *tx_device(void)
{
    if (test_wav[0] || audio_device_selected < 0) return "default";
    return audio_devices[audio_device_selected];
}

/* Encode the whole picture, then play it with the PTT keyed. Safety as in DigiPan:
 * the PTT is released on normal end, Esc, error, time limit, window close,
 * SIGINT/SIGTERM/SIGHUP and X connection loss; RX is muted while transmitting. */
static void tx_session(void)
{
    atomic_store(&tx_abort, 0);
    atomic_store(&tx_state, 1);
    tx_set_msg("TX: preparing...");
    int kind = tx_kind;
    int drm = tx_is_drm() || kind != 0;
    const sstv_mode *m = tx_mode();
    char mname[32];
    if (kind == 1) snprintf(mname, sizeof mname, "repeat request");
    else if (kind == 2) snprintf(mname, sizeof mname, "fix (%d segments)", fix_n);
    else if (drm) snprintf(mname, sizeof mname, "DRM %c %d-QAM", "ABE"[cfg.drm_rob], 4 << (2 * cfg.drm_qam));
    else snprintf(mname, sizeof mname, "%s", m->name);
    image *img = NULL;
    pthread_mutex_lock(&tx_img_mutex);
    if (tx_img) { img = image_new(tx_img->w, tx_img->h); memcpy(img->px, tx_img->px, (size_t)img->w * img->h * 3); }
    pthread_mutex_unlock(&tx_img_mutex);
    if (kind == 0 && (!img || img->w != tx_w() || img->h != tx_h())) {
        image_free(img); tx_set_msg("TX: picture not ready"); atomic_store(&tx_req, 0); atomic_store(&tx_state, 0); return;
    }
    long n = 0;
    float *x = NULL;
    if (kind == 1) {                              /* ask for the missing segments */
        drm_params p = tx_drm_params();
        tx_set_msg("TX: coding the repeat request...");
        x = drm_tx_render(&p, (const unsigned char *)tx_bsr_text, tx_bsr_len, "bsr.bin", NULL, 0, AUDIO_RATE, &n, NULL);
    } else if (kind == 2) {                       /* resend what another station asked for */
        DrmSent *d = &drm_sent[fix_idx];
        tx_set_msg("TX: coding the missing segments...");
        x = drm_tx_render(&d->p, d->data, d->len, d->name, fix_list, fix_n, AUDIO_RATE, &n, NULL);
    } else if (drm) {
        int len = 0, q = 0;
        unsigned char *jpg = img_jpeg_fit(img, drm_kbs[cfg.drm_kb] * 1024, &len, &q);
        char fname[64], call[16]; time_t now = time(NULL); struct tm g; gmtime_r(&now, &g);
        snprintf(call, sizeof call, "%.12s", cfg.call[0] ? cfg.call : "SSTV");
        for (char *p = call; *p; p++) if (*p == '/' || *p == ' ') *p = '-';
        char ts[24]; strftime(ts, sizeof ts, "%Y%m%d%H%M%S", &g);
        snprintf(fname, sizeof fname, "%s-%s.jpg", ts, call);
        if (jpg && cfg.drm_rs) {                  /* Reed-Solomon protected: name.rsN */
            long ol;
            unsigned char *enc = rs_file_encode(jpg, len, "jpg", cfg.drm_rs, &ol);
            free(jpg); jpg = enc; len = (int)ol;
            snprintf(fname, sizeof fname, "%s-%s.rs%d", ts, call, cfg.drm_rs);
        }
        drm_params p = tx_drm_params();
        tx_set_msg("TX: coding the digital signal...");
        if (jpg) x = drm_tx_render(&p, jpg, len, fname, NULL, 0, AUDIO_RATE, &n, NULL);
        if (jpg && x) {                           /* remember it for repeat requests */
            DrmSent *d = &drm_sent[drm_nsent % NSENT];
            free(d->data);
            d->data = jpg; jpg = NULL; d->len = len; d->p = p;
            snprintf(d->name, sizeof d->name, "%s", fname);
            d->tid = mot_transport_id(fname);
            drm_nsent++;
        }
        free(jpg);
    } else {
        sstv_tx_opts o; sstv_tx_defaults(&o);
        o.fs = AUDIO_RATE; o.lead_ms = 50; o.tail_ms = 150;
        x = sstv_encode(m, img, &o, &n);
        if (x && cfg.fskid && cfg.call[0]) {      /* our callsign as FSK ID after the picture */
            long nf;
            float *fid = fskid_tx(cfg.call, AUDIO_RATE, &nf);
            float *x2 = fid ? realloc(x, sizeof(float) * (n + nf)) : NULL;
            if (x2) { x = x2; memcpy(x + n, fid, sizeof(float) * nf); n += nf; }
            free(fid);
        }
    }
    image_free(img);
    if (kind == 2) fix_idx = -1;                  /* answered */
    if (!x) { tx_set_msg("TX: cannot code the picture"); atomic_store(&tx_req, 0); atomic_store(&tx_state, 0); return; }
    x = clock_fix(x, &n);
    atomic_store(&tx_total, n); atomic_store(&tx_done, 0);
    char err[128] = "";
    audio_out *ao = audio_out_open(tx_device(), tx_wav, AUDIO_RATE, err, sizeof err);
    if (!ao) { tx_set_msg(err); free(x); atomic_store(&tx_req, 0); atomic_store(&tx_state, 0); return; }
    live_mute(lv, 1);
    tx_set_msg("TX: PTT on");
    if (rig_ptt(1) != 0) {
        tx_set_msg("TX: PTT FAILED - is rigctld running?");
        rig_ptt(0);
        audio_out_close(ao, 0); free(x);
        live_mute(lv, 0);
        atomic_store(&tx_req, 0); atomic_store(&tx_state, 0);
        return;
    }
    msleep_ms(150);                               /* let the relay / PA settle before the audio */
    atomic_store(&tx_state, 2);
    char m1[96]; snprintf(m1, sizeof m1, "TX: sending %s", mname); tx_set_msg(m1);
    double t0 = mono_now(), limit = (double)n / AUDIO_RATE + TX_LIMIT_EXTRA;
    int error = 0, stopped = 0;
    for (long off = 0; off < n; off += 1024) {
        if (atomic_load(&tx_abort)) { tx_set_msg("TX: aborted"); stopped = 1; break; }
        if (mono_now() - t0 > limit) { tx_set_msg("TX: time limit reached"); stopped = 1; break; }
        int k = n - off < 1024 ? (int)(n - off) : 1024;
        if (audio_out_write(ao, x + off, k) != 0) { tx_set_msg("TX: audio error"); error = 1; break; }
        atomic_store(&tx_done, off + k);
    }
    atomic_store(&tx_state, 3);
    audio_out_close(ao, !error && !stopped);
    free(x);
    int ok = 0;
    for (int k = 0; k < 6 && !ok; k++) { if (rig_ptt(0) == 0) ok = 1; else msleep_ms(200); }
    if (!ok) tx_set_msg("TX: PTT OFF FAILED - CHECK THE RADIO!");
    else if (!error && !stopped) {
        snprintf(m1, sizeof m1, "TX: %s sent%s", mname, tx_wav[0] ? " (to WAV file)" : "");
        tx_set_msg(m1);
        snprintf(last_mode_name, sizeof last_mode_name, "%.31s", mname);
    }
    msleep_ms(300);                               /* receiver recovery before decoding again */
    live_mute(lv, 0);
    atomic_store(&tx_req, 0);
    atomic_store(&tx_state, 0);
}
static void *tx_worker(void *unused)
{
    (void)unused;
    while (atomic_load(&tx_running)) { if (atomic_load(&tx_req)) tx_session(); else msleep_ms(20); }
    return NULL;
}
static void tx_toggle(void)
{
    if (tx_active()) atomic_store(&tx_abort, 1);          /* a picture cannot be finished early: abort */
    else { tx_render(); tx_set_msg(""); tx_kind = 0; atomic_store(&tx_req, 1); }   /* fresh macros (%T ...) */
}
static void tx_shutdown(void)
{
    atomic_store(&tx_abort, 1); atomic_store(&tx_req, 0);
    atomic_store(&tx_running, 0);
    pthread_join(tx_thread, NULL);
    rig_shutdown();                                       /* PTT off, rigctld stopped */
}
static void on_signal(int sig) { (void)sig; quit_sig = 1; }
#ifdef _WIN32
static void win_setup_home(void)
{
    const char *u = getenv("USERPROFILE");     /* always: a HOME from MSYS/Cygwin/Wine may be a Unix path */
    if (!(u && *u) && getenv("HOME") && *getenv("HOME")) return;
    char h[400];
    snprintf(h, sizeof h, "%s", u && *u ? u : "C:/SSTVNext");
    for (char *p = h; *p; p++) if (*p == '\\') *p = '/';
    _putenv_s("HOME", h);
}
#endif
/* open a web page in the default browser, without waiting for it */
static void open_url(const char *url)
{
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid == 0) {                      /* double fork: the browser is not our child, no zombie */
        if (fork() == 0) {
            setsid();
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
#endif
}

static int on_x_io_error(Display *d) { (void)d; rig_ptt(0); _exit(1); }   /* never leave the radio keyed */

/* ================================================================ receive */

static void on_audio(const float *x, int n, void *u)
{
    (void)u;
    live_push(lv, x, n);
    pthread_mutex_lock(&spec_mutex);
    for (int i = 0; i < n; i++) spec_ring[(spec_wr + (unsigned long)i) & (SPEC_RING - 1)] = x[i];
    spec_wr += (unsigned long)n;
    pthread_mutex_unlock(&spec_mutex);
}

static void audio_select(int idx)
{
    audio_device_selected = idx;
    if (audio_start(audio_devices[idx], AUDIO_RATE, on_audio, NULL, audio_status, sizeof audio_status) == 0) {
        snprintf(cfg.audio, sizeof cfg.audio, "%s", audio_devices[idx]);
        config_save();
    }
    live_reset(lv);
}

static void apply_rx_force(void)
{
    live_set_force(lv, rx_force < 0 ? NULL : sstv_mode_at(rx_force));
    snprintf(cfg.rx_mode, sizeof cfg.rx_mode, "%s", rx_force < 0 ? "auto" : MODE_ID(rx_force));
}

static void gal_add(const char *path, int front)
{
    image *full = img_load(path);
    if (!full) return;
    image *th = img_fit(full, TW, TH);
    image_free(full);
    GalItem *g = NULL;
    for (int i = 0; i < ngal && !g; i++)          /* the same file again (updated): in place, */
        if (!strcmp(gal[i].path, path)) {           /* a combined picture that got better: to the front */
            image_free(gal[i].thumb);
            if (front && strstr(path, "_combined.png")) {
                memmove(&gal[i], &gal[i + 1], (size_t)(ngal - i - 1) * sizeof gal[0]);
                ngal--;
                break;
            }
            g = &gal[i];
        }
    if (!g) {
        if (ngal == NGAL) { image_free(gal[NGAL - 1].thumb); ngal--; }
        if (front) { memmove(&gal[1], &gal[0], (size_t)ngal * sizeof gal[0]); }
        g = &gal[front ? 0 : ngal];
        ngal++;
    }
    snprintf(g->path, sizeof g->path, "%s", path);
    g->thumb = th;
    g->snr[0] = 0;
    char meta[2048], v[16];
    if (img_png_meta(path, meta, sizeof meta)) {
        if (img_meta_get(meta, "combined", v, sizeof v)) snprintf(g->snr, sizeof g->snr, "%dx", atoi(v));   /* copies combined */
        else if (img_meta_get(meta, "snr", v, sizeof v)) snprintf(g->snr, sizeof g->snr, "%.0f dB", atof(v) > 30 ? 30 : atof(v));
    }
}
static void gal_load_dir(void)
{
    char pat[320]; snprintf(pat, sizeof pat, "%.300s/*.png", cfg.rx_dir);
    glob_t g; if (glob(pat, 0, NULL, &g) != 0) return;
    for (size_t i = g.gl_pathc; i-- > 0 && ngal < NGAL;) gal_add(g.gl_pathv[i], 0);   /* newest first */
    globfree(&g);
}

/* radix-2 FFT of the newest FFT_N samples -> spec_db */
static void update_spectrum(void)
{
    static double re[FFT_N], im[FFT_N];
    pthread_mutex_lock(&spec_mutex);
    unsigned long wr = spec_wr; int have = wr >= FFT_N;
    if (have) for (unsigned i = 0; i < FFT_N; i++) re[i] = spec_ring[(wr - FFT_N + i) & (SPEC_RING - 1)];
    pthread_mutex_unlock(&spec_mutex);
    if (!have) { for (int k = 0; k <= FFT_N / 2; k++) spec_db[k] = -100; return; }
    for (int i = 0; i < FFT_N; i++) { re[i] *= 0.5 - 0.5 * cos(2.0 * M_PI * i / (FFT_N - 1)); im[i] = 0; }
    unsigned j = 0;
    for (unsigned i = 1; i < FFT_N; i++) {
        unsigned bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (unsigned len = 2; len <= FFT_N; len <<= 1) {
        double a = -2.0 * M_PI / len, wr0 = cos(a), wi0 = sin(a);
        for (unsigned base = 0; base < FFT_N; base += len) {
            double wre = 1, wim = 0; unsigned half = len >> 1;
            for (unsigned k = 0; k < half; k++) {
                unsigned p = base + k, q = p + half;
                double tr = wre * re[q] - wim * im[q], ti = wre * im[q] + wim * re[q];
                re[q] = re[p] - tr; im[q] = im[p] - ti; re[p] += tr; im[p] += ti;
                double nw = wre * wr0 - wim * wi0; wim = wre * wi0 + wim * wr0; wre = nw;
            }
        }
    }
    for (int k = 0; k <= FFT_N / 2; k++) {
        double v = 20 * log10(hypot(re[k], im[k]) / (FFT_N * 0.25) + 1e-7);
        spec_db[k] = spec_db[k] * 0.5 + v * 0.5;               /* light smoothing */
    }
}

/* ============================================================ UI panels */

static void draw_editor_canvas(void);
static void draw_editor_tools(void);

/* filled triangle pointing right (play icon) */
static void tri_right(int x, int y, int h, col_t c)
{
    for (int i = 0; i < h; i++) { int w = (i < h / 2 ? i : h - 1 - i) * 2 / 2 + 1; fill(x, y + i, w, 1, c); }
}

static void draw_picture(void)
{
    if (edit_mode) { draw_editor_canvas(); return; }
    int ph = lp_h();
    begin_buf(LX, LY, LPW, ph);
    fill(LX, LY, LPW, ph, c.bg);
    ui_rrect(LX, LY, LPW, ph, 10, c.inset, 1);
    ui_rrect_line(LX, LY, LPW, ph, 10, RGB(38, 45, 55), 1);
    const image *im = view_img ? view_img : rx_img;
    if (im) {
        int dw, dh; fit_size(im, LPW - 8, ph - 8, &dw, &dh);
        int px = LX + (LPW - dw) / 2, py = LY + (ph - dh) / 2;
        ui_shadow(px, py, dw, dh, 4, 14, 0.45);
        put_rgb(im, px, py, dw, dh);
        ui_rrect_line(px - 1, py - 1, dw + 2, dh + 2, 3, RGB(255, 255, 255), 0.08);
    } else {
        /* empty state: a stylised picture icon and what the receiver is doing */
        int cx = LX + LPW / 2, cy = LY + ph / 2 - 30;
        ui_rrect(cx - 46, cy - 34, 92, 68, 10, c.panel2, 1);
        ui_rrect_line(cx - 46, cy - 34, 92, 68, 10, c.grid, 1);
        static const col_t bars[3] = { 0xE5534B, 0x57AB5A, 0x539BF5 };
        for (int i = 0; i < 3; i++) ui_rrect(cx - 30, cy - 18 + i * 13, 60, 7, 3, bars[i], 0.85);
        const char *m1 = "Waiting for an SSTV picture";
        ui_text(F_TITLE, cx - ui_text_w(F_TITLE, m1) / 2, cy + 66, m1, c.text);
        const char *m2 = "Automatic detection - all 25 analog modes and digital SSTV (HamDRM)";
        ui_text(F_SMALL, cx - ui_text_w(F_SMALL, m2) / 2, cy + 88, m2, c.muted);
    }
    /* badges in the top margin */
    if (view_img) {
        int w = chip(LX + 12, LY + 10, "VIEW", RGB(255, 255, 255), c.accent);
        ui_text_fit(F_SMALL, LX + 20 + w, LY + 23, view_name, c.muted, LPW - w - 40);
    } else if (rx_st.state == LIVE_RX && rx_st.mode) {
        ui_rrect(LX + 12, LY + 10, 58, 18, 9, RGB(110, 22, 22), 1);
        ui_circle(LX + 23, LY + 19, 4, c.red, 1);
        ui_text(F_SMALLB, LX + 32, LY + 23, "LIVE", RGB(255, 205, 205));
        chip(LX + 76, LY + 10, rx_st.mode->name, c.text, c.panel2);
    } else if (rx_st.drm_sync) {
        ui_rrect(LX + 12, LY + 10, 74, 18, 9, RGB(20, 70, 110), 1);
        ui_circle(LX + 23, LY + 19, 4, c.accent, 1);
        ui_text(F_SMALLB, LX + 32, LY + 23, "DIGITAL", RGB(200, 230, 255));
        int w = chip(LX + 92, LY + 10, rx_st.drm_info, c.text, c.panel2);
        if (rx_st.drm_call[0]) chip(LX + 100 + w, LY + 10, rx_st.drm_call, c.accent2, c.panel2);
    } else if (rx_st.mode && im) {
        int w = chip(LX + 12, LY + 10, rx_st.mode->name, c.muted, c.panel2);
        if (rx_st.combined) {
            char t[48]; snprintf(t, sizeof t, "COMBINED FROM %d RECEPTIONS", rx_st.combined);
            chip(LX + 20 + w, LY + 10, t, RGB(255, 255, 255), c.accent);
        }
    }
    end_buf();
}

/* "label value" pairs in one line */
static int kv(int x, int y, const char *k, const char *v, col_t vc)
{
    int w = ui_text(F_SMALL, x, y, k, c.muted);
    w += 5 + ui_text(F_SMALLB, x + w + 5, y, v, vc);
    return w + 16;
}

static void draw_rx_status(void)
{
    int y = LY + lp_h() + 4;
    begin_buf(LX, y, LPW, 36);
    fill(LX, y, LPW, 36, c.bg);
    if (edit_mode) {
        char t[200];
        snprintf(t, sizeof t, "Editing template \"%s\" on %s", cur_tpl.name, tx_name());
        ui_text(F_BOLD, LX + 4, y + 14, t, c.cyan);
        if (rx_st.state == LIVE_RX) snprintf(t, sizeof t, "Receiving in the background: %s  %d/%d lines", rx_st.mode->name, rx_st.lines, rx_st.lines_total);
        else snprintf(t, sizeof t, "Click: select    drag: move    corner: resize    Del: delete    arrows: nudge    Esc: done");
        ui_text(F_SMALL, LX + 4, y + 31, t, rx_st.state == LIVE_RX ? c.accent2 : c.muted);
    } else if (view_img) {
        if (view_meta[0]) {             /* reception data stored with the picture */
            char v[64], t[40];
            int x = LX + 2, ty = y + 22;
            if (img_meta_get(view_meta, "call", v, sizeof v)) x += kv(x, ty, "from", v, c.accent2);
            if (img_meta_get(view_meta, "snr", v, sizeof v)) { double d = atof(v); snprintf(t, sizeof t, "%.0f dB", d > 30 ? 30 : d); x += kv(x, ty, "SNR", t, d < 5 ? c.yellow : c.text); }
            if (img_meta_get(view_meta, "offset", v, sizeof v)) { snprintf(t, sizeof t, "%+.0f Hz", atof(v)); x += kv(x, ty, "offset", t, fabs(atof(v)) > 50 ? c.yellow : c.text); }
            if (img_meta_get(view_meta, "ppm", v, sizeof v)) { snprintf(t, sizeof t, "%+.0f ppm", atof(v)); x += kv(x, ty, "clock", t, c.text); }
            if (img_meta_get(view_meta, "lines", v, sizeof v)) x += kv(x, ty, "lines", v, c.text);
            if (img_meta_get(view_meta, "combined", v, sizeof v)) x += kv(x, ty, "combined", v, c.accent);
        } else ui_text(F_SMALL, LX + 4, y + 22, "Saved picture - receiving goes on in the background", c.muted);
        button(LX + LPW - 80, y + 4, 80, 28, "Live", 1);
    } else {
        const live_status *s = &rx_st;
        int H = s->lines_total;
        double fr = H ? (double)s->lines / H : 0;
        ui_rrect(LX, y + 1, LPW, 7, 3, c.panel2, 1);
        if (s->mode && fr > 0) {
            int fw = (int)(LPW * fr); if (fw < 7) fw = 7;
            if (s->state == LIVE_RX) ui_rrect_grad(LX, y + 1, fw, 7, 3, RGB(64, 224, 180), RGB(35, 170, 140));
            else ui_rrect(LX, y + 1, fw, 7, 3, RGB(56, 110, 170), 1);
        }
        char v[64];
        int x = LX + 2, ty = y + 28;
        if (s->state != LIVE_RX && (s->drm_sync || (s->drm_total > 0 && s->drm_have > 0))) {
            /* digital picture: progress in segments */
            double f2 = s->drm_total ? (double)s->drm_have / s->drm_total : 0;
            if (f2 > 0) {
                int fw = (int)(LPW * f2); if (fw < 7) fw = 7;
                if (s->drm_sync) ui_rrect_grad(LX, y + 1, fw, 7, 3, RGB(83, 155, 245), RGB(50, 110, 200));
                else ui_rrect(LX, y + 1, fw, 7, 3, RGB(56, 110, 170), 1);
            }
            x += ui_text(F_BOLD, x, ty, "DRM", s->drm_sync ? c.accent : c.text) + 14;
            if (s->drm_sync) {
                if (s->drm_call[0]) x += kv(x, ty, "from", s->drm_call, c.accent2);
                snprintf(v, sizeof v, "%.0f dB", s->drm_snr);      x += kv(x, ty, "SNR", v, s->drm_snr < 8 ? c.yellow : c.text);
                snprintf(v, sizeof v, "%+.0f Hz", s->drm_foff);    x += kv(x, ty, "offset", v, c.text);
            }
            if (s->drm_total) snprintf(v, sizeof v, "%d / %d", s->drm_have, s->drm_total);
            else snprintf(v, sizeof v, "%d", s->drm_have);
            x += kv(x, ty, "segments", v, c.text);
            int bw2 = s->drm_can_bsr && !tx_active() ? 112 : 0;
            if (s->drm_name[0]) ui_text_fit(F_SMALL, x, ty, s->drm_name, c.muted, LX + LPW - x - 8 - bw2);
            if (bw2) button(LX + LPW - bw2, y + 10, bw2, 26, "Ask repeat", 1);
        } else if (s->state == LIVE_RX || s->mode) {
            x += ui_text(F_BOLD, x, ty, s->mode->name, s->state == LIVE_RX ? c.accent2 : c.text) + 14;
            snprintf(v, sizeof v, "%d / %d", s->lines, H);           x += kv(x, ty, "lines", v, c.text);
            if (s->state == LIVE_RX) { snprintf(v, sizeof v, "%d", s->syncs); x += kv(x, ty, "sync", v, c.text); }
            if (!isnan(s->snr)) { snprintf(v, sizeof v, "%.0f dB", s->snr > 30 ? 30 : s->snr); x += kv(x, ty, "SNR", v, s->snr < 5 ? c.yellow : c.text); }
            {   /* the sender's clock: ours taken off when known */
                double own = own_clock();
                snprintf(v, sizeof v, "%+.0f ppm", isnan(own) ? s->clock_ppm : s->clock_ppm - own);
                x += kv(x, ty, isnan(own) ? "slant" : "clock", v, c.text);
            }
            snprintf(v, sizeof v, "%+.0f Hz", s->offset);              x += kv(x, ty, "offset", v, fabs(s->offset) > 50 ? c.yellow : c.text);
            if (s->state != LIVE_RX && x < LX + LPW - ui_text_w(F_SMALL, "last picture") - 4) ui_text(F_SMALL, LX + LPW - ui_text_w(F_SMALL, "last picture") - 4, ty, "last picture", c.muted);
        } else {
            ui_circle(x + 5, ty - 4, 4, c.accent2, 0.9);
            ui_text(F_SMALL, x + 16, ty, "Idle - listening on all modes", c.muted);
        }
    }
    end_buf();
}

/* spectrum 1000..2600 Hz with the SSTV tone markers: tune the 1900 Hz leader onto its line */
static void draw_tuning(void)
{
    if (edit_mode) return;                        /* the editor's tools are there */
    int x = RX0(), y = TUN_Y, w = RW(), h = TUN_H;
    begin_buf(x, y, w, h);
    panel(x, y, w, h, "TUNING", c.accent2);
    int px = x + 10, pw = w - 20, py = y + 28, phh = h - 50;
    ui_rrect(px, py, pw, phh, 6, c.inset, 1);
    const double f0 = 1000, f1 = 2600;
    /* faint 100 Hz grid */
    for (int f = 1100; f < 2600; f += 100) {
        int gx = px + (int)((f - f0) / (f1 - f0) * pw);
        ui_fill_a(gx, py + 3, 1, phh - 6, RGB(255, 255, 255), f % 500 ? 0.03 : 0.06);
    }
    static const struct { double f; const char *n; } mk[] = { {1200,"SYNC 1200"}, {1500,"BLACK 1500"}, {1900,"VIS 1900"}, {2300,"WHITE 2300"} };
    /* the curve */
    static int top[2048];
    double pk = -200, pkf = 0;
    int npw = pw - 4 < 2048 ? pw - 4 : 2048;
    for (int i = 0; i < npw; i++) {
        double f = f0 + (f1 - f0) * i / npw, bin = f * FFT_N / AUDIO_RATE;
        int b = (int)bin; double fr = bin - b;
        double db = spec_db[b] * (1 - fr) + spec_db[b + 1] * fr;
        if (db > pk) { pk = db; pkf = f; }
        int v = (int)((db + 90) / 80 * (phh - 8)); if (v < 0) v = 0; if (v > phh - 8) v = phh - 8;
        top[i] = py + phh - 3 - v;
    }
    ui_vgrad_area(px + 2, py, npw, top, py + phh - 3, c.cyan, 0.38, 0.03);
    for (int i = 1; i < npw; i++) ui_line(px + 2 + i - 0.5, top[i - 1] + 0.5, px + 2 + i + 0.5, top[i] + 0.5, RGB(121, 192, 255), 1);
    for (size_t i = 0; i < 4; i++) {
        int mx = px + (int)((mk[i].f - f0) / (f1 - f0) * pw);
        if (i == 2) ui_fill_a(mx, py + 2, 1, phh - 4, c.yellow, 0.9);
        else ui_vline_dash(mx, py + 3, py + phh - 4, c.muted, 0.45);
        int tw = ui_text_w(F_SMALL, mk[i].n);
        ui_text(F_SMALL, mx - tw / 2, y + h - 7, mk[i].n, i == 2 ? c.yellow : c.muted);
    }
    /* level meter + peak in the title row */
    double lv = audio_level();
    int mw = 70, mx = x + w - mw - 12;
    ui_rrect(mx, y + 11, mw, 8, 4, c.inset, 1);
    int lw = (int)(mw * fmin(1.0, lv * 2.5));
    if (lw > 3) ui_rrect_grad(mx, y + 11, lw, 8, 4, lv > 0.32 ? c.red : RGB(64, 224, 180), lv > 0.32 ? RGB(200, 60, 50) : RGB(35, 170, 140));
    char t[80];
    if (pk > -70) snprintf(t, sizeof t, "peak %.0f Hz   level %.0f%%", pkf, lv * 100);
    else snprintf(t, sizeof t, "level %.0f%%", lv * 100);
    ui_text(F_SMALL, mx - 10 - ui_text_w(F_SMALL, t), y + 20, t, c.muted);
    end_buf();
}

static void draw_tx_panel(void)
{
    if (edit_mode) { draw_editor_tools(); return; }
    int x = RX0(), y = TXP_Y, w = RW(), h = TXP_H;
    begin_buf(x, y, w, h);
    int on = tx_active();
    panel(x, y, w, h, "TRANSMIT", c.tx);
    if (on) ui_rrect_line(x, y, w, h, 10, c.red, 0.8);
    /* preview */
    int bw = 264, bh = 212;
    ui_rrect(x + 10, y + 28, bw, bh, 6, c.inset, 1);
    pthread_mutex_lock(&tx_img_mutex);
    if (tx_img) {
        int dw, dh; double sc = fmin((double)bw / tx_img->w, (double)bh / tx_img->h);
        dw = (int)(tx_img->w * sc); dh = (int)(tx_img->h * sc);
        int ix = x + 10 + (bw - dw) / 2, iy = y + 28 + (bh - dh) / 2;
        put_rgb(tx_img, ix, iy, dw, dh);
        ui_round_corners(ix, iy, dw, dh, 6, c.inset);
    }
    pthread_mutex_unlock(&tx_img_mutex);
    ui_rrect_line(x + 10, y + 28, bw, bh, 6, on ? c.red : c.grid, 1);
    /* controls */
    int cx = x + bw + 22, cw = w - bw - 32;
    button(cx, y + 28, 34, 28, "<", 0); button(cx + 40, y + 28, 34, 28, ">", 0);
    char fn[160];
    if (tx_nfiles) {
        const char *b = strrchr(tx_files[tx_idx], '/');
        snprintf(fn, sizeof fn, "%d/%d", tx_idx + 1, tx_nfiles);
        int nw = ui_text(F_SMALLB, cx + 84, y + 46, fn, c.muted);
        ui_text_fit(F_REG, cx + 92 + nw, y + 47, b ? b + 1 : tx_files[tx_idx], c.text, cw - 100 - nw);
    } else ui_text_fit(F_SMALL, cx + 84, y + 46, "test card - put pictures in the folder", c.muted, cw - 88);
    ui_text(F_SMALL, cx, y + 85, "Mode", c.muted);
    button(cx + 44, y + 66, 120, 28, tx_name(), tx_is_drm());
    char dur[32];
    if (tx_is_drm()) {
        drm_params p = tx_drm_params();
        long sent = cfg.drm_rs ? (long)((tx_jpeg_len + 7 + rs_kk(cfg.drm_rs) - 1) / rs_kk(cfg.drm_rs)) * 255 : tx_jpeg_len;
        snprintf(dur, sizeof dur, "~%.0f s  %.1f kB", drm_tx_duration(&p, (int)sent) + 0.5, tx_jpeg_len / 1024.0);
    }
    else { const sstv_mode *m = tx_mode(); snprintf(dur, sizeof dur, "%.0f s", (m->pre_sync_ms + sstv_nlines(m) * sstv_line_ms(m)) / 1000 + 2.1); }
    chip(cx + 174, y + 71, dur, c.muted, c.panel2);
    ui_text(F_SMALL, cx, y + 123, "Tmpl", c.muted);
    button(cx + 44, y + 104, 26, 28, "<", 0);
    { int nw = cw - 44 - 26 - 26 - 70 - 12;
      ui_rrect(cx + 74, y + 104, nw + 1, 29, 5, c.inset, 1); ui_rrect_line(cx + 74, y + 104, nw + 1, 29, 5, c.grid, 1);
      ui_text_fit(F_REG, cx + 81, y + 123, cur_tpl.name, c.text, nw - 12);
      button(cx + 78 + nw, y + 104, 26, 28, ">", 0); }
    button(cx + cw - 64, y + 104, 64, 28, "Edit", 0);
    /* send / abort */
    if (on) {
        ui_shadow(cx, y + 144, 120, 36, 8, 10, 0.4);
        ui_rrect_grad(cx, y + 144, 120, 37, 8, RGB(248, 81, 73), RGB(200, 45, 40));
        ui_rrect_line(cx, y + 144, 120, 37, 8, RGB(255, 160, 150), 0.5);
        ui_circle(cx + 17, y + 162, 5, RGB(255, 255, 255), 1);
        ui_text(F_BOLD, cx + 30, y + 167, "ON AIR - Esc", RGB(255, 255, 255));
    } else {
        ui_shadow(cx, y + 144, 120, 36, 8, 10, 0.35);
        ui_rrect_grad(cx, y + 144, 120, 37, 8, RGB(247, 150, 75), RGB(214, 104, 34));
        ui_rrect_line(cx, y + 144, 120, 37, 8, RGB(255, 200, 150), 0.45);
        tri_right(cx + 16, y + 155, 15, RGB(255, 255, 255));
        ui_text(F_BOLD, cx + 36, y + 167, "Send", RGB(255, 255, 255));
        ui_text(F_SMALL, cx + 76, y + 167, "F1", RGB(255, 230, 210));
    }
    if (fix_idx >= 0 && !on) {
        char b[32]; snprintf(b, sizeof b, "Fix %d", fix_n);
        button(cx + 130, y + 148, 80, 28, b, 1);
    } else button(cx + 130, y + 148, 80, 28, "Rescan", 0);
    if (!tx_is_drm() && !on) {
        ui_text(F_SMALL, cx, y + 205, "ID", c.muted);
        button(cx + 44, y + 187, 120, 26, cfg.fskid ? "FSK ID on" : "FSK ID off", cfg.fskid);
    }
    if (!on) {     /* own sound card clock: measured on received pictures, corrects the transmission */
        char b[40];
        double e = own_clock();
        int bx = cx + 172, bw = tx_is_drm() ? 0 : cw - 172;   /* analog: next to FSK ID (the DRM row is full) */
        if (bw >= 90) {
            if (isnan(e)) { snprintf(b, sizeof b, "Clock: %d/%d pics", cfg.nclk, CLK_MIN); button(bx, y + 187, bw, 26, b, 0); }
            else { snprintf(b, sizeof b, "Clock %s %+.0f", cfg.txclk ? "fix" : "off", e); button(bx, y + 187, bw, 26, b, cfg.txclk); }
        }
    }
    if (tx_is_drm() && !on) {
        /* digital settings: robustness, QAM, protection, bandwidth / picture size, JPEG size */
        char b[24];
        ui_text(F_SMALL, cx, y + 205, "DRM", c.muted);
        snprintf(b, sizeof b, "%c", "ABE"[cfg.drm_rob]);           button(cx + 44, y + 187, 34, 26, b, 0);
        snprintf(b, sizeof b, "%d-QAM", 4 << (2 * cfg.drm_qam));  button(cx + 82, y + 187, 70, 26, b, 0);
        button(cx + 156, y + 187, 52, 26, cfg.drm_prot ? "Norm" : "High", 0);
        button(cx + 212, y + 187, 50, 26, cfg.drm_so ? "2.5k" : "2.3k", 0);
        ui_text(F_SMALL, cx, y + 235, "Pic", c.muted);
        snprintf(b, sizeof b, "%dx%d", tx_w(), tx_h());           button(cx + 44, y + 217, 84, 26, b, 0);
        snprintf(b, sizeof b, "max %d kB", drm_kbs[cfg.drm_kb]);  button(cx + 132, y + 217, 84, 26, b, 0);
        if (cfg.drm_rs) snprintf(b, sizeof b, "RS%d", cfg.drm_rs); else snprintf(b, sizeof b, "no RS");
        button(cx + 220, y + 217, 58, 26, b, cfg.drm_rs != 0);
    }
    if (on) {
        double fr = (double)atomic_load(&tx_done) / (double)atomic_load(&tx_total);
        ui_rrect(cx, y + 190, cw, 8, 4, c.panel2, 1);
        int pw2 = (int)(cw * fr); if (pw2 > 8) ui_rrect_grad(cx, y + 190, pw2, 8, 4, RGB(255, 140, 90), RGB(248, 81, 73));
        char left[32]; snprintf(left, sizeof left, "%.0f s left", (atomic_load(&tx_total) - atomic_load(&tx_done)) / (double)AUDIO_RATE);
        ui_text(F_SMALL, cx + cw - ui_text_w(F_SMALL, left), y + 213, left, c.muted);
    }
    char msg[sizeof tx_msg];
    pthread_mutex_lock(&tx_msg_mutex); snprintf(msg, sizeof msg, "%s", tx_msg); pthread_mutex_unlock(&tx_msg_mutex);
    ui_text_fit(F_SMALL, cx, y + (on ? 230 : 256), msg, strstr(msg, "FAIL") ? c.red : c.yellow, cw);
    { char d[300]; snprintf(d, sizeof d, "Folder  %s", short_path(cfg.tx_dir)); ui_text_fit(F_SMALL, x + 12, y + h - 10, d, c.muted, w - 24); }
    end_buf();
}

static int gal_cols(void) { int n = (RW() - 12) / (TW + 8); return n < 1 ? 1 : n; }
static int gal_rows(void) { int n = (log_bar_y() - 12 - GAL_Y - 28) / (TH + 8); return n < 1 ? 1 : n; }
static void draw_gallery(void)
{
    int x = RX0(), y = GAL_Y, w = RW(), h = log_bar_y() - 12 - GAL_Y;
    if (h < 40) return;
    begin_buf(x, y, w, h);
    panel(x, y, w, h, edit_mode ? "RECEIVED - click to paste" : "RECEIVED", c.accent2);
    { char n[16]; snprintf(n, sizeof n, "%d", ngal);
      int tx0 = x + 21 + ui_text_w(F_SMALLB, edit_mode ? "RECEIVED - click to paste" : "RECEIVED") + 8;
      int cw2 = chip(tx0, y + 7, n, c.text, c.panel2);
      ui_text_fit(F_SMALL, tx0 + cw2 + 10, y + 20, short_path(cfg.rx_dir), c.muted, x + w - (tx0 + cw2 + 22)); }
    int cols = gal_cols(), rows = gal_rows();
    for (int r = 0; r < rows; r++)
        for (int k = 0; k < cols; k++) {
            int i = gal_scroll * cols + r * cols + k;
            if (i >= ngal) break;
            int tx = x + 8 + k * (TW + 8), ty = y + 26 + r * (TH + 8);
            put_rgb(gal[i].thumb, tx, ty, TW, TH);
            ui_round_corners(tx, ty, TW, TH, 6, c.panel);
            if (gal[i].snr[0]) {
                int bw = ui_text_w(F_SMALLB, gal[i].snr) + 10;
                int comb = gal[i].snr[strlen(gal[i].snr) - 1] == 'x';
                ui_rrect(tx + TW - bw - 4, ty + TH - 20, bw, 16, 8, comb ? c.accent : RGB(0, 0, 0), comb ? 0.95 : 0.62);
                ui_text(F_SMALLB, tx + TW - bw + 1, ty + TH - 8, gal[i].snr, RGB(235, 240, 245));
            }
            int sel_ = view_img && !strcmp(view_name, strrchr(gal[i].path, '/') + 1);
            if (sel_) { ui_rrect_line(tx - 2, ty - 2, TW + 4, TH + 4, 8, c.accent, 1); ui_rrect_line(tx - 1, ty - 1, TW + 2, TH + 2, 7, c.accent, 1); }
            else ui_rrect_line(tx, ty, TW, TH, 6, RGB(255, 255, 255), 0.08);
        }
    if (!ngal) ui_text(F_SMALL, x + 14, y + 50, "Nothing received yet - pictures appear here and are saved automatically.", c.muted);
    end_buf();
}

/* ---------------------------------------------------------------- log bar */
static char *log_field_ptr(int idx, size_t *mx)
{
    switch (idx) {
    case 0: *mx = sizeof his_call; return his_call;
    case 1: *mx = 4; return lg_rs;              /* RST: 3 digits (the box shows no more) */
    case 2: *mx = 4; return lg_rr;
    default: *mx = sizeof lg_freq; return lg_freq;
    }
}
static void log_say(const char *m, int ok) { snprintf(log_msg, sizeof log_msg, "%s", m); log_msg_ok = ok; log_msg_until = time(NULL) + 6; }
static void log_poll_freq(void)
{
    double hz;
    if (tx_active() || rig_get_freq(&hz) != 0) return;
    snprintf(lg_freq, sizeof lg_freq, "%.5f", hz / 1e6); freq_auto = 1;
}
static void log_check_dupe(void) { log_dupe_hard = adif_dupe(his_call, atof(lg_freq), log_dupe, sizeof log_dupe) == 2; }
static void log_reset_fields(void)
{
    his_call[0] = 0; snprintf(lg_rs, sizeof lg_rs, "595"); snprintf(lg_rr, sizeof lg_rr, "595");
    qso_start = 0; log_dupe[0] = 0; log_dupe_hard = 0;
}
static void log_do(void)
{
    time_t now = time(NULL); if (!qso_start) qso_start = now;
    if (freq_auto || !lg_freq[0]) log_poll_freq();
    char comment[48] = "";
    if (last_mode_name[0]) snprintf(comment, sizeof comment, "%s", last_mode_name);
    else if (rx_st.mode) snprintf(comment, sizeof comment, "%s", rx_st.mode->name);
    char m[120];
    int ok = adif_log(his_call, atof(lg_freq), lg_rs, lg_rr, comment, cfg.call, (long)qso_start, (long)now, m, sizeof m) == 0;
    log_say(m, ok);
    if (ok) { log_reset_fields(); tx_render(); }
}
#define LB_H 28
static void log_box(int idx, int x, int y, int w, int h)
{
    size_t mx; char *s = log_field_ptr(idx, &mx); int foc = (log_focus == idx);
    if (foc) ui_rrect_line(x - 2, y - 2, w + 5, h + 5, 7, c.accent, 0.35);
    ui_rrect(x, y, w + 1, h + 1, 5, c.inset, 1);
    ui_rrect_line(x, y, w + 1, h + 1, 5, foc ? c.accent : (idx == 0 && log_dupe_hard) ? c.red : c.grid, 1);
    col_t tc = (idx == 0 && log_dupe_hard) ? c.red : (idx == 3 && freq_auto && s[0]) ? c.yellow : c.text;
    field_text(idx == 0 ? F_BOLD : F_REG, x, y, w, h, s, foc, tc);
}static void draw_log_bar(void)
{
    int ly = log_bar_y(), ty = ly + LB_H / 2 + 5;
    begin_buf(8, ly - 4, width - 16, LB_H + 8);
    fill(8, ly - 4, width - 16, LB_H + 8, c.bg);
    ui_rrect(8, ly - 4, width - 16, LB_H + 8, 9, c.panel, 1);
    ui_rrect_line(8, ly - 4, width - 16, LB_H + 8, 9, RGB(38, 45, 55), 1);
    ui_text(F_SMALL, 16, ty, "Call", c.muted); log_box(0, 44, ly, 104, LB_H);
    ui_text(F_SMALL, 158, ty, "S", c.muted);   log_box(1, 170, ly, 40, LB_H);
    ui_text(F_SMALL, 218, ty, "R", c.muted);   log_box(2, 230, ly, 40, LB_H);
    ui_text(F_SMALL, 280, ty, "MHz", c.muted); log_box(3, 310, ly, 80, LB_H);
    int room = width - 244 - 410;
    if (room > 40) {
        char info[200]; col_t col = c.muted; int pill = 0;
        if (log_msg[0] && time(NULL) < log_msg_until) { snprintf(info, sizeof info, "%s", log_msg); col = log_msg_ok ? c.green : c.red; pill = 1; }
        else if (log_dupe[0]) { snprintf(info, sizeof info, "%s", log_dupe); col = log_dupe_hard ? c.red : c.yellow; pill = 1; }
        if (pill) {
            ui_circle(410, ty - 4, 4, col, 1);
            ui_text_fit(F_REG, 420, ty, info, col, room - 20);
        } else {
            time_t now = time(NULL); struct tm g; gmtime_r(&now, &g);
            char t[32]; snprintf(t, sizeof t, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
            int x = 404 + ui_text(F_SMALL, 404, ty, "UTC", c.muted) + 6;
            x += ui_text(F_BOLD, x, ty, t, c.text) + 16;
            const char *band = band_of(atof(lg_freq));
            if (band[0]) x += chip(x, ly + 5, band, c.text, c.panel2) + 12;
            ui_text_fit(F_SMALL, x, ty, cfg.call[0] ? cfg.call : "set your callsign: Radio", cfg.call[0] ? c.green : c.yellow, 404 + room - x);
        }
    }
    button(width - 236, ly, 84, LB_H, "Log QSO", 1); button(width - 142, ly, 56, LB_H, "Clear", 0); button(width - 78, ly, 64, LB_H, "List", 0);
    end_buf();
}
/* ------------------------------------------------------------ radio dialog */
#define DLG_W 640
#define DLG_H 452
#define DLG_Y 100
#define DLG_ROWS 8
static void rig_refilter(void)
{
    free(rig_vis); rig_vis = malloc((size_t)(rig_nmodels + 1) * sizeof *rig_vis); rig_nvis = 0;
    if (!rig_vis) return;
    char toks[6][48]; int nt = 0;
    { const char *s = rig_filter;
      while (*s && nt < 6) { while (*s == ' ') s++; if (!*s) break; int n = 0; while (*s && *s != ' ' && n < 47) { toks[nt][n++] = (char)tolower((unsigned char)*s); s++; } toks[nt][n] = 0; nt++; } }
    for (int i = 0; i < rig_nmodels; i++) {
        char hay[120]; snprintf(hay, sizeof hay, "%d %s", rig_models[i].id, rig_models[i].name);
        for (char *q = hay; *q; q++) *q = (char)tolower((unsigned char)*q);
        int ok = 1; for (int t = 0; t < nt && ok; t++) if (!strstr(hay, toks[t])) ok = 0;
        if (ok) rig_vis[rig_nvis++] = i;
    }
    if (rig_scroll > rig_nvis - DLG_ROWS) rig_scroll = rig_nvis - DLG_ROWS;
    if (rig_scroll < 0) rig_scroll = 0;
}
static int rig_vis_index(void) { for (int i = 0; i < rig_nvis; i++) if (rig_models[rig_vis[i]].id == cfg_model) return i; return -1; }
static void rig_select_vis(int i)
{
    if (i < 0) i = 0;
    if (i >= rig_nvis) i = rig_nvis - 1;
    if (i < 0) return;
    cfg_model = rig_models[rig_vis[i]].id;
    snprintf(cfg_model_name, sizeof cfg_model_name, "%s", rig_models[rig_vis[i]].name);
    if (i < rig_scroll) rig_scroll = i;
    if (i >= rig_scroll + DLG_ROWS) rig_scroll = i - DLG_ROWS + 1;
}
static int rig_is_numeric(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static char *rig_field(size_t *max)
{
    if (rig_focus == 0) { *max = 20; return cfg.call; }
    if (rig_focus == 2) { *max = 90; return cfg_port; }
    *max = 40; return rig_filter;
}
static void rig_dlg_open(void)
{
    rig_load_models(); rig_scan_ports();
    rig_filter[0] = 0; rig_refilter();
    int i = rig_vis_index();
    rig_scroll = i >= 0 ? (i > 3 ? i - 3 : 0) : 0;
    if (rig_scroll > rig_nvis - DLG_ROWS) rig_scroll = rig_nvis - DLG_ROWS;
    if (rig_scroll < 0) rig_scroll = 0;
    rig_focus = 1; rig_dlg = 1;
}
static void rig_dlg_close(void) { rig_dlg = 0; config_save(); tx_prepare(0); }
static void rig_do_apply(void)
{
    if (tx_active()) { snprintf(rig_status, sizeof rig_status, "Stop the transmission first"); return; }
    if (cfg_model <= 0 && rig_nmodels == 0 && rig_is_numeric(rig_filter)) { cfg_model = atoi(rig_filter); snprintf(cfg_model_name, sizeof cfg_model_name, "model %d", cfg_model); }
    rig_apply();
    config_save();
}
/* dialog frame: shadow, surface, title + hint */
static void dialog_frame(int x, int y, int w, int h, const char *title, const char *hint)
{
    ui_shadow(x, y, w, h, 12, 26, 0.55);
    ui_rrect(x, y, w, h, 12, RGB(28, 33, 41), 1);
    ui_rrect_line(x, y, w, h, 12, RGB(60, 70, 84), 1);
    ui_text(F_TITLE, x + 20, y + 27, title, c.text);
    if (hint) ui_text(F_SMALL, x + w - 20 - ui_text_w(F_SMALL, hint), y + 26, hint, c.muted);
    ui_fill_a(x + 1, y + 38, w - 2, 1, RGB(255, 255, 255), 0.06);
}

static void draw_rig_dialog(void)
{
    if (!rig_dlg) return;
    int dx = (width - DLG_W) / 2, dy = DLG_Y;
    dialog_frame(dx, dy, DLG_W, DLG_H, "Radio / CAT setup", "Tab: next field    Esc: close");
    ui_text(F_SMALL, dx + 20, dy + 63, "Callsign", c.muted); field_box(dx + 90, dy + 44, 150, 28, cfg.call, rig_focus == 0);
    ui_text(F_SMALL, dx + 270, dy + 63, "PTT", c.muted); button(dx + 310, dy + 44, 130, 28, "VOX / manual", !cfg_cat); button(dx + 446, dy + 44, 150, 28, "CAT (rigctld)", cfg_cat);
    char hd[80]; snprintf(hd, sizeof hd, "Radio model - type to search   (%d of %d)", rig_nvis, rig_nmodels);
    ui_text(F_SMALL, dx + 20, dy + 100, hd, c.muted);
    field_box(dx + 20, dy + 108, 600, 28, rig_filter, rig_focus == 1);
    int ly = dy + 142;
    ui_rrect(dx + 20, ly, 601, DLG_ROWS * 22 + 5, 6, c.inset, 1);
    ui_rrect_line(dx + 20, ly, 601, DLG_ROWS * 22 + 5, 6, c.grid, 1);
    if (rig_nmodels == 0) ui_text_fit(F_SMALL, dx + 30, ly + 20, "No model list: rigctl not found. Install Hamlib, or type a model number and press Apply.", c.yellow, 580);
    for (int r = 0; r < DLG_ROWS; r++) {
        int i = rig_scroll + r; if (i >= rig_nvis) break;
        const RigModel *m = &rig_models[rig_vis[i]];
        if (m->id == cfg_model) ui_rrect(dx + 23, ly + 2 + r * 22, 586, 22, 5, c.blue, 1);
        char id[16]; snprintf(id, sizeof id, "%5d", m->id);
        ui_text(F_MONO, dx + 30, ly + 18 + r * 22, id, m->id == cfg_model ? RGB(220, 235, 255) : c.muted);
        ui_text_fit(F_REG, dx + 84, ly + 18 + r * 22, m->name, c.text, 520);
    }
    if (rig_nvis > DLG_ROWS) {
        int th = (DLG_ROWS * 22) * DLG_ROWS / rig_nvis; if (th < 14) th = 14;
        int ty = ly + 2 + (DLG_ROWS * 22 - th) * rig_scroll / (rig_nvis - DLG_ROWS);
        ui_rrect(dx + 612, ty, 5, th, 2, c.muted, 0.8);
    }
    ui_text(F_SMALL, dx + 20, dy + 340, "Selected", c.muted);
    ui_text_fit(F_BOLD, dx + 80, dy + 340, cfg_model > 0 ? cfg_model_name : "(none)", c.cyan, 520);
    ui_text(F_SMALL, dx + 20, dy + 371, "Port", c.muted); field_box(dx + 60, dy + 352, 300, 28, cfg_port, rig_focus == 2);
    button(dx + 370, dy + 352, 60, 28, "Next", 0);
    ui_text(F_SMALL, dx + 450, dy + 371, "Baud", c.muted);
    char bs[16]; snprintf(bs, sizeof bs, "%d", cfg_baud); button(dx + 500, dy + 352, 100, 28, bs, 0);
    col_t sc = strstr(rig_status, "OK") ? c.green : (strstr(rig_status, "fail") || strstr(rig_status, "not") || strstr(rig_status, "Select") || strstr(rig_status, "Stop")) ? c.yellow : c.muted;
    ui_circle(dx + 344, dy + 425, 4, sc, 1);
    ui_text_fit(F_SMALL, dx + 354, dy + 430, rig_status, sc, DLG_W - 374);
    button(dx + 20, dy + 412, 100, 28, "Apply", 1); button(dx + 130, dy + 412, 110, 28, "Disconnect", 0); button(dx + 250, dy + 412, 80, 28, "Close", 0);
}static void rig_dlg_click(int bx, int by, int btn)
{
    int rx = bx - (width - DLG_W) / 2, ry = by - DLG_Y;
    if (btn == 4 || btn == 5) { rig_scroll += btn == 4 ? -3 : 3; if (rig_scroll > rig_nvis - DLG_ROWS) rig_scroll = rig_nvis - DLG_ROWS; if (rig_scroll < 0) rig_scroll = 0; return; }
    if (btn != 1) return;
    int dx0 = (width - DLG_W) / 2;
    if (hit(rx, ry, 90, 44, 150, 28)) { if (rig_focus == 0) field_click(F_REG, dx0 + 90, 150, cfg.call, bx); rig_focus = 0; }
    else if (hit(rx, ry, 310, 44, 130, 28)) {          /* VOX: takes effect at once (rigctld stopped, saved) */
        if (tx_active()) snprintf(rig_status, sizeof rig_status, "Stop the transmission first");
        else { cfg_cat = 0; rig_filter[0] = 0; rig_refilter(); rig_do_apply(); }
    }
    else if (hit(rx, ry, 446, 44, 150, 28)) { cfg_cat = 1; snprintf(rig_status, sizeof rig_status, "Select the radio model and port, then Apply"); }
    else if (hit(rx, ry, 20, 108, 600, 28)) { if (rig_focus == 1) field_click(F_REG, dx0 + 20, 600, rig_filter, bx); rig_focus = 1; }
    else if (hit(rx, ry, 20, 142, 600, DLG_ROWS * 22 + 4)) { int i = rig_scroll + (ry - 142 - 2) / 22; if (i >= 0 && i < rig_nvis) { rig_select_vis(i); cfg_cat = 1; } }
    else if (hit(rx, ry, 60, 352, 300, 28)) { if (rig_focus == 2) field_click(F_REG, dx0 + 60, 300, cfg_port, bx); rig_focus = 2; }
    else if (hit(rx, ry, 370, 352, 60, 28)) {
        if (rig_nports == 0) rig_scan_ports();
        if (rig_nports == 0) snprintf(rig_status, sizeof rig_status, "No serial ports found (/dev/ttyUSB*, /dev/ttyACM*)");
        else { int k = -1; for (int i = 0; i < rig_nports; i++) if (!strcmp(rig_ports[i], cfg_port)) k = i; snprintf(cfg_port, sizeof cfg_port, "%.90s", rig_ports[(k + 1) % rig_nports]); }
    }
    else if (hit(rx, ry, 500, 352, 100, 28)) { int k = 0; for (int i = 0; i < 6; i++) if (rig_bauds[i] == cfg_baud) k = i; cfg_baud = rig_bauds[(k + 1) % 6]; }
    else if (hit(rx, ry, 20, 412, 100, 28)) { cfg_cat = cfg_cat || rig_is_numeric(rig_filter); draw_rig_dialog(); flush_ui(); rig_do_apply(); }
    else if (hit(rx, ry, 130, 412, 110, 28)) { if (tx_active()) snprintf(rig_status, sizeof rig_status, "Stop the transmission first"); else { cfg_cat = 0; rig_do_apply(); } }
    else if (hit(rx, ry, 250, 412, 80, 28)) rig_dlg_close();
}
static void rig_dlg_key(KeySym ks, const char *kb, int kl)
{
    if (ks == XK_Escape) { rig_dlg_close(); return; }
    if (ks == XK_Tab || ks == XK_ISO_Left_Tab) { rig_focus = (rig_focus + 1) % 3; return; }
    if (ks == XK_Return || ks == XK_KP_Enter) { cfg_cat = cfg_cat || rig_is_numeric(rig_filter); draw_rig_dialog(); flush_ui(); rig_do_apply(); return; }
    if (ks == XK_Up || ks == XK_Down || ks == XK_Prior || ks == XK_Next) {
        int i = rig_vis_index(), d = ks == XK_Up ? -1 : ks == XK_Down ? 1 : ks == XK_Prior ? -DLG_ROWS : DLG_ROWS;
        rig_select_vis(i < 0 ? (d > 0 ? 0 : rig_nvis - 1) : i + d); if (cfg_model > 0) cfg_cat = 1;
        return;
    }
    size_t mx; char *f = rig_field(&mx);
    if (line_edit(f, mx, ks, kb, kl, rig_focus == 0 ? filt_call : filt_ascii) && rig_focus == 1) { rig_scroll = 0; rig_refilter(); }
}

/* ------------------------------------------------------------- log dialog */
#define DLG_LH 420
#define LROWS 12
static void draw_log_dialog(void)
{
    if (!log_dlg) return;
    int dx = (width - DLG_W) / 2, dy = DLG_Y;
    char h[80]; snprintf(h, sizeof h, "QSO log  -  %d QSO%s", nqso, nqso == 1 ? "" : "s");
    dialog_frame(dx, dy, DLG_W, DLG_LH, h, NULL);
    ui_text_fit(F_SMALL, dx + 20 + ui_text_w(F_TITLE, h) + 14, dy + 26, short_path(log_path), c.muted, DLG_W - 60 - ui_text_w(F_TITLE, h));
    ui_text(F_MONO, dx + 28, dy + 53, "Date       UTC    Call        Band  Mode  RST      Comment", c.muted);
    for (int r = 0; r < LROWS; r++) {
        int k = nqso - 1 - (log_scroll + r); if (k < 0) break;
        const Qso *q = &qsos[k]; int y = dy + 58 + r * 24;
        if (log_scroll + r == log_sel) ui_rrect(dx + 20, y, 600, 23, 5, c.blue, 1);
        else if (r & 1) ui_rrect(dx + 20, y, 600, 23, 5, RGB(255, 255, 255), 0.025);
        char row[200], d[12] = "", tm[8] = "";
        if (strlen(q->date) == 8) snprintf(d, sizeof d, "%.4s-%.2s-%.2s", q->date, q->date + 4, q->date + 6);
        if (strlen(q->time_on) >= 4) snprintf(tm, sizeof tm, "%.2s:%.2s", q->time_on, q->time_on + 2);
        snprintf(row, sizeof row, "%-10s %-5s  %-11.11s %-5.5s %-5.5s %3.3s/%-3.3s  %.18s", d, tm, q->call, q->band, q->mode, q->rst_s, q->rst_r, q->comment);
        ui_text_fit(F_MONO, dx + 28, y + 16, row, c.text, 586);
    }
    if (nqso == 0) ui_text(F_REG, dx + 28, dy + 84, "No QSOs logged yet.", c.muted);
    button(dx + 20, dy + 382, 170, 28, log_confirm ? "Really delete?" : "Delete selected", log_confirm);
    button(dx + 200, dy + 382, 80, 28, "Close", 1);
    ui_text(F_SMALL, dx + 300, dy + 401, "Up / Down / wheel: scroll    Esc: close", c.muted);
}static void log_dlg_select(int d)
{
    log_sel += d; if (log_sel >= nqso) log_sel = nqso - 1; if (log_sel < 0) log_sel = 0;
    if (log_sel < log_scroll) log_scroll = log_sel;
    if (log_sel >= log_scroll + LROWS) log_scroll = log_sel - LROWS + 1;
    log_confirm = 0;
}
static void log_dlg_click(int bx, int by, int btn)
{
    int rx = bx - (width - DLG_W) / 2, ry = by - DLG_Y;
    if (btn == 4) { log_dlg_select(-1); return; }
    if (btn == 5) { log_dlg_select(1); return; }
    if (btn != 1) return;
    if (hit(rx, ry, 20, 58, 600, LROWS * 24)) { int i = log_scroll + (ry - 58) / 24; if (i < nqso) log_sel = i; log_confirm = 0; }
    else if (hit(rx, ry, 20, 382, 170, 28)) {
        if (nqso == 0) return;
        if (!log_confirm) { log_confirm = 1; return; }
        if (adif_delete(nqso - 1 - log_sel) != 0) log_say("Cannot rewrite the log file", 0);
        log_confirm = 0; if (log_sel >= nqso) log_sel = nqso - 1; if (log_sel < 0) log_sel = 0;
    }
    else if (hit(rx, ry, 200, 382, 80, 28)) log_dlg = 0;
}
static void log_dlg_key(KeySym ks)
{
    if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter) log_dlg = 0;
    else if (ks == XK_Up) log_dlg_select(-1);
    else if (ks == XK_Down) log_dlg_select(1);
    else if (ks == XK_Prior) log_dlg_select(-LROWS);
    else if (ks == XK_Next) log_dlg_select(LROWS);
}

/* ------------------------------------------------------------- whole window */
/* input channel row under the devices: Both / Left / Right */
#define CH_ROW_Y(n) (79 + (n) * 25 + 10)
static void draw_channel_row(void)
{
    int y = CH_ROW_Y(audio_device_count);
    static const char *lab[3] = {"Both", "Left", "Right"};
    ui_line(112, y - 4, 406, y - 4, RGB(60, 70, 84), 1);
    ui_text(F_SMALL, 118, y + 19, "Channel", c.muted);
    for (int i = 0; i < 3; i++) button(186 + i * 74, y + 2, 68, 24, lab[i], cfg.in_ch == i);
}

static void draw_audio_menu_overlay(void)
{
    if (!audio_menu) return;
    int menu_h = audio_device_count * 25 + 8 + 40;
    ui_shadow(104, 78, 310, menu_h, 8, 16, 0.5);
    ui_rrect(104, 78, 310, menu_h, 8, RGB(28, 33, 41), 1);
    ui_rrect_line(104, 78, 310, menu_h, 8, RGB(60, 70, 84), 1);
    for (int i = 0; i < audio_device_count; i++) {
        int sel_ = i == audio_device_selected;
        if (sel_) ui_rrect(108, 79 + i * 25 + 3, 302, 23, 5, c.blue, 1);
        ui_text(F_REG, 118, 95 + i * 25 + 3, audio_devices[i], c.text);
        if (sel_) ui_text(F_SMALLB, 380, 95 + i * 25 + 3, "in use", RGB(200, 220, 255));
    }
    draw_channel_row();
}
/* ------------------------------------------------------------ scanner --- */
/* Goes round the SSTV frequencies (rig in DATA mode: D-L below 10 MHz, D-U above), stays
 * where it finds a picture (analog or digital), and when the frequency has gone quiet
 * after it offers to send (F1). It never transmits by itself. */
typedef struct { double mhz; int usb; } ScanFreq;
static const ScanFreq scan_list[] = {
    {3.730, 0}, {3.735, 0}, {7.165, 0}, {7.171, 0}, {14.230, 1},
    {14.233, 1}, {18.160, 1}, {21.340, 1}, {24.975, 1}, {28.680, 1},
};
#define NSCAN ((int)(sizeof scan_list / sizeof scan_list[0]))
#define SCAN_DWELL 12.0      /* s listening on a frequency */
#define SCAN_QUIET 6.0       /* s of silence after a picture: the frequency is free */
#define SCAN_OFFER 45.0      /* s the offer to send stays before scanning on */
enum { SC_OFF, SC_LISTEN, SC_STAY, SC_QUIET, SC_HOLD };
static int scan_state = SC_OFF, scan_i = -1, scan_menu = 0;
static double scan_t0, scan_last_act;
static char scan_err[80] = "";

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int scan_tune(int i)
{
    char cmd[64];
    snprintf(cmd, sizeof cmd, "F %.0f\n", scan_list[i].mhz * 1e6);
    if (rig_cmd(cmd) != 0) return -1;
    rig_cmd(scan_list[i].usb ? "M PKTUSB 0\n" : "M PKTLSB 0\n");
    snprintf(lg_freq, sizeof lg_freq, "%.5f", scan_list[i].mhz);
    freq_auto = 1;
    return 0;
}

static int scan_next(void)
{
    if (!cfg.scan_on) return -1;
    for (int k = 1; k <= NSCAN; k++) {
        int i = (scan_i + k + NSCAN) % NSCAN;
        if (cfg.scan_on & (1u << i)) return i;
    }
    return -1;
}

static void scan_stop(void) { scan_state = SC_OFF; scan_err[0] = 0; }

static void scan_go_next(void)
{
    int i = scan_next();
    if (i < 0) { scan_stop(); snprintf(scan_err, sizeof scan_err, "Scan: no frequency selected (right click on Scan)"); return; }
    if (scan_tune(i) != 0) { scan_stop(); snprintf(scan_err, sizeof scan_err, "Scan: the radio does not answer (CAT)"); return; }
    scan_i = i; scan_state = SC_LISTEN; scan_t0 = now_s();
}

static void scan_start(void)
{
    if (!rig_enabled) { snprintf(scan_err, sizeof scan_err, "Scan needs CAT: set up the radio (Radio)"); return; }
    scan_err[0] = 0;
    scan_go_next();
}

/* once in a while from the main loop; returns 1 if the display has to change */
static int scan_tick(const live_status *st)
{
    if (scan_state == SC_OFF) return 0;
    double t = now_s();
    int act = st->state == LIVE_RX || st->drm_sync;
    if (act) scan_last_act = t;
    int old = scan_state;
    if (tx_active()) { scan_state = SC_HOLD; return old != SC_HOLD; }     /* a QSO: stay here */
    switch (scan_state) {
    case SC_LISTEN:
        if (act) scan_state = SC_STAY;
        else if (t - scan_t0 > SCAN_DWELL) { scan_go_next(); return 1; }
        break;
    case SC_STAY:
        if (!act && t - scan_last_act > SCAN_QUIET) { scan_state = SC_QUIET; scan_t0 = t; }
        break;
    case SC_QUIET:
        if (act) scan_state = SC_STAY;
        else if (t - scan_t0 > SCAN_OFFER) { scan_go_next(); return 1; }
        break;
    }
    if (scan_state != old) return 1;
    static int last_sec = -1;                       /* the countdown on the button */
    int sec = (int)(t - scan_t0);
    if (sec != last_sec && (scan_state == SC_LISTEN || scan_state == SC_QUIET)) { last_sec = sec; return 1; }
    return 0;
}

static void scan_label(char *b, size_t n)
{
    if (scan_state == SC_OFF) { snprintf(b, n, "Scan"); return; }
    double f = scan_list[scan_i].mhz;
    int left = (int)((scan_state == SC_LISTEN ? SCAN_DWELL : SCAN_OFFER) - (now_s() - scan_t0) + 0.99);
    if (left < 0) left = 0;
    switch (scan_state) {
    case SC_LISTEN: snprintf(b, n, "%.3f %ds", f, left); break;
    case SC_STAY:   snprintf(b, n, "%.3f RX", f); break;
    case SC_QUIET:  snprintf(b, n, "%.3f F1?", f); break;
    default:        snprintf(b, n, "%.3f hold", f); break;
    }
}

/* the scanner's word in the message line, or NULL */
static const char *scan_msg(void)
{
    static char m[120];
    if (scan_err[0]) return scan_err;
    if (scan_state == SC_QUIET) {
        snprintf(m, sizeof m, "%.3f quiet now - F1: send", scan_list[scan_i].mhz);
        return m;
    }
    if (scan_state == SC_HOLD) {
        snprintf(m, sizeof m, "Scan paused (QSO) - Scan: go on");
        return m;
    }
    return NULL;
}

static void draw_scan_button(void)
{
    char b[32];
    scan_label(b, sizeof b);
    begin_buf(900, 43, 94, 33);
    fill(900, 43, 94, 33, c.panel);
    button(900, 43, 94, 32, b, scan_state != SC_OFF);
    end_buf();
}

static void draw_scan_menu(void)
{
    if (!scan_menu) return;
    int x = 900, y = 78, w = 190, h = NSCAN * 25 + 34;
    ui_shadow(x, y, w, h, 8, 16, 0.5);
    ui_rrect(x, y, w, h, 8, RGB(28, 33, 41), 1);
    ui_rrect_line(x, y, w, h, 8, RGB(60, 70, 84), 1);
    ui_text(F_SMALL, x + 12, y + 20, "Frequencies to scan", c.muted);
    for (int i = 0; i < NSCAN; i++) {
        int on = (cfg.scan_on >> i) & 1, yy = y + 28 + i * 25;
        if (on) ui_rrect(x + 4, yy, w - 8, 23, 5, c.blue, 1);
        char b[40]; snprintf(b, sizeof b, "%.3f MHz", scan_list[i].mhz);
        ui_text(F_REG, x + 14, yy + 16, b, c.text);
        ui_text(F_SMALLB, x + 130, yy + 16, scan_list[i].usb ? "D-U" : "D-L", on ? RGB(200, 220, 255) : c.muted);
    }
}

static void draw_status_bar(void)
{
    begin_buf(0, height - 25, width, 25);
    fill(0, height - 25, width, 25, c.inset);
    ui_fill(0, height - 25, width, 1, RGB(33, 39, 48));
    int ok = !strncmp(audio_status, "Audio OK", 8) || !strncmp(audio_status, "Test input", 10);
    ui_circle(16, height - 12.5, 4, ok ? c.green : c.yellow, 1);
    ui_text_fit(F_SMALL, 26, height - 8, audio_status, ok ? RGB(201, 209, 217) : c.yellow, width / 2 - 40);
    ui_circle(width / 2 - 10, height - 12.5, 4, rig_enabled ? c.green : c.muted, 1);
    ui_text_fit(F_SMALL, width / 2, height - 8, rig_status, rig_enabled ? RGB(201, 209, 217) : c.muted, width / 2 - 130);
    ui_text(F_SMALL, width - 12 - ui_text_w(F_SMALL, APP), height - 8, APP, RGB(88, 96, 105));
    end_buf();
}static void draw_rx_msg(void)
{
    int x = 1002, w = width - 1014;
    begin_buf(x, 43, w, 33);
    fill(x, 43, w, 33, c.panel);
    int qrm = strstr(rx_st.msg, "QRM") != NULL, live = rx_st.state == LIVE_RX, txon = tx_active();
    col_t dot = txon ? c.tx : qrm ? c.yellow : live ? c.red : c.accent2;
    ui_rrect(x, 43, w, 33, 16, c.inset, 1);
    ui_rrect_line(x, 43, w, 33, 16, qrm ? RGB(120, 100, 40) : RGB(38, 45, 55), 1);
    ui_circle(x + 17, 59.5, 4.5, dot, 1);
    if (live) ui_circle(x + 17, 59.5, 8, dot, 0.18);
    const char *sm = txon ? NULL : scan_msg();
    ui_text_fit(F_REG, x + 31, 64, txon ? "Transmitting - receiver muted" : sm ? sm : rx_st.msg,
                txon ? c.tx : sm ? (scan_err[0] ? c.yellow : c.accent) : qrm ? c.yellow : c.text, w - 44);
    end_buf();
}/* app icon: a small picture with red/green/blue scan lines */
static void draw_logo(int x, int y)
{
    ui_rrect_grad(x, y, 24, 24, 6, RGB(56, 139, 253), RGB(35, 90, 200));
    static const col_t bars[3] = { 0xFF7B72, 0x7EE787, 0xA5D6FF };
    for (int i = 0; i < 3; i++) ui_rrect(x + 5, y + 6 + i * 5, 14, 3, 1, bars[i], 0.95);
}

/* place of the Support button in the header, left of the callsign */
static void support_rect(int *x, int *w)
{
    int right = width - 12;
    if (cfg.call[0]) right -= ui_text_w(F_BOLD, cfg.call) + 26 + 8;
    *w = ui_text_w(F_BOLD, "Support") + 28;
    *x = right - *w;
}

static void draw(void)
{
    fill(0, 0, width, height, c.bg);
    /* header */
    for (int j = 0; j < 36; j++) ui_fill(0, j, width, 1, ui_mix(RGB(26, 32, 41), RGB(17, 21, 28), j / 35.0));
    draw_logo(12, 6);
    int tx = 44 + ui_text(F_TITLE, 44, 23, "SSTV Next", c.text);
    tx += 8 + ui_text(F_SMALL, tx + 8, 23, SSTVNEXT_VERSION, c.muted);
    ui_text(F_SMALL, tx + 14, 23, "slow-scan television transceiver", RGB(110, 118, 129));
    if (cfg.call[0]) {
        int w = ui_text_w(F_BOLD, cfg.call) + 26;
        ui_rrect(width - w - 12, 7, w, 22, 11, RGB(18, 52, 32), 1);
        ui_rrect_line(width - w - 12, 7, w, 22, 11, RGB(46, 160, 67), 0.8);
        ui_circle(width - w - 1, 18, 3.5, c.green, 1);
        ui_text(F_BOLD, width - w + 6, 23, cfg.call, RGB(126, 231, 135));
    }
    int sx, sw; support_rect(&sx, &sw);              /* Support button: opens the Ko-fi page */
    ui_rrect(sx, 7, sw, 22, 11, RGB(58, 24, 36), 1);
    ui_rrect_line(sx, 7, sw, 22, 11, RGB(219, 97, 130), 0.8);
    ui_circle(sx + 11, 18, 3.5, RGB(255, 123, 160), 1);
    ui_text(F_BOLD, sx + 18, 23, "Support", RGB(255, 182, 202));
    /* toolbar */
    ui_fill(0, 36, width, 58, c.panel);
    ui_fill(0, 36, width, 1, RGB(38, 45, 55));
    ui_fill(0, 93, width, 1, RGB(38, 45, 55));
    ui_text(F_SMALL, 16, 63, "Audio input", c.muted);
    button(104, 43, 260, 32, test_wav[0] ? "Test WAV input" : audio_menu ? "Select audio input" : (audio_device_selected >= 0 ? audio_devices[audio_device_selected] : "Choose audio input"), audio_menu);
    ui_text(F_SMALL, 378, 63, "RX mode", c.muted);
    button(440, 43, 140, 32, rx_force < 0 ? "Auto (all modes)" : sstv_mode_at(rx_force)->name, rx_force >= 0);
    button(590, 43, 100, 32, "Save now", 0);
    button(700, 43, 90, 32, cfg_cat ? "Radio" : "Radio: VOX", rig_enabled);
    button(800, 43, 90, 32, cfg.nr ? "QRM filter" : "QRM filt off", cfg.nr);
    draw_scan_button();
    draw_rx_msg();
    draw_picture();
    draw_rx_status();
    draw_tuning();
    draw_tx_panel();
    draw_gallery();
    draw_log_bar();
    draw_status_bar();
    if (rig_dlg || log_dlg) ui_dim(0, 0, width, height, 0.55);
    draw_audio_menu_overlay();
    draw_scan_menu();
    draw_rig_dialog();
    draw_log_dialog();
    ui_present(0, 0, width, height);
    flush_ui();
}
/* ------------------------------------------------------------------ input */
static void open_view(const char *path)
{
    image *im = img_load(path);
    if (!im) return;
    image_free(view_img); view_img = im;
    const char *b = strrchr(path, '/'); snprintf(view_name, sizeof view_name, "%s", b ? b + 1 : path);
    img_png_meta(path, view_meta, sizeof view_meta);
    tx_render();                                  /* the report macros follow the viewed picture */
}
static void close_view(void) { image_free(view_img); view_img = NULL; view_name[0] = 0; view_meta[0] = 0; tx_render(); }

static int log_bar_click(int x, int y, int btn)
{
    int ly = log_bar_y(), was = log_focus;
    if (!(y >= ly && y < ly + LB_H && btn == 1)) { if (was >= 0) log_focus = -1; return 0; }
    static const int fx[4] = { 44, 170, 230, 310 }, fw[4] = { 104, 40, 40, 80 };
    for (int i = 0; i < 4; i++) if (x >= fx[i] && x < fx[i] + fw[i]) {
        size_t mx; char *f = log_field_ptr(i, &mx);
        if (log_focus == i) field_click(i == 0 ? F_BOLD : F_REG, fx[i], fw[i], f, x);   /* caret to the click */
        else caret_buf = NULL;                                                          /* new field: at the end */
        log_focus = i; ed_focus = 0; return 1;
    }
    log_focus = -1;
    if (x >= width - 236 && x < width - 152) log_do();
    else if (x >= width - 142 && x < width - 86) log_reset_fields();
    else if (x >= width - 78 && x < width - 14) { log_dlg = 1; log_sel = 0; log_scroll = 0; log_confirm = 0; }
    return 1;
}
static void log_key(KeySym ks, const char *kb, int kl)
{
    if (ks == XK_Escape) { log_focus = -1; return; }
    if (ks == XK_Tab) { log_focus = (log_focus + 1) % 4; return; }
    if (ks == XK_Return || ks == XK_KP_Enter) { log_do(); return; }
    size_t mx; char *f = log_field_ptr(log_focus, &mx);
    int chg = line_edit(f, mx, ks, kb, kl, log_focus == 0 ? filt_call : log_focus == 3 ? filt_freq : filt_digit);
    if (chg) {
        tx_render();
        if (log_focus == 3) freq_auto = 0;
        if (log_focus == 0 && his_call[0] && !qso_start) { qso_start = time(NULL); if (!lg_freq[0] || freq_auto) { draw_log_bar(); flush_ui(); log_poll_freq(); } }
        if (log_focus == 0 || log_focus == 3) log_check_dupe();
    }
}
/* ============================================================ template editor
 * The big picture panel becomes the canvas (background + template at the
 * transmit mode's size), the tools take the place of the tuning and transmit
 * panels; the gallery stays, a click on a received picture pastes it in. */

static void tpl_refresh_list(void) { tpl_count = tpl_list(tpl_dir, tpl_names, 64); }

static int tpl_index(void)
{
    for (int i = 0; i < tpl_count; i++) if (!strcmp(tpl_names[i], cur_tpl.name)) return i;
    return 0;
}

static void tpl_select(const char *name)
{
    tpl_clear(&cur_tpl);
    if (!name || tpl_load(&cur_tpl, tpl_dir, name) != 0) {
        if (tpl_count > 0 && tpl_load(&cur_tpl, tpl_dir, tpl_names[0]) == 0) {}
        else tpl_init(&cur_tpl, "Empty");
    }
    snprintf(cfg.tpl, sizeof cfg.tpl, "%s", cur_tpl.name);
    tpl_dirty = 0; sel = -1;
}

static void tpl_save_cur(void)
{
    if (!cur_tpl.name[0]) snprintf(cur_tpl.name, sizeof cur_tpl.name, "Untitled");
    if (tpl_save(&cur_tpl, tpl_dir) == 0) { tpl_dirty = 0; snprintf(ed_msg, sizeof ed_msg, "Saved \"%s\"", cur_tpl.name); }
    else snprintf(ed_msg, sizeof ed_msg, "Cannot save to %.80s", tpl_dir);
    tpl_refresh_list();
    snprintf(cfg.tpl, sizeof cfg.tpl, "%s", cur_tpl.name);
    config_save();
}

static void tpl_step(int d)
{
    if (tpl_dirty) tpl_save_cur();
    tpl_refresh_list();
    if (tpl_count == 0) return;
    int i = (tpl_index() + d + tpl_count) % tpl_count;
    tpl_select(tpl_names[i]);
    config_save();
    tx_render();
}

static void edit_open(void)
{
    if (tx_active()) { tx_set_msg("Wait for the end of the transmission"); return; }
    close_view();
    edit_mode = 1; sel = -1; drag = 0; ed_focus = 0; del_confirm = 0; ed_msg[0] = 0;
    log_focus = -1;
}

static void edit_close(void)
{
    if (tpl_dirty) tpl_save_cur();
    edit_mode = 0; sel = -1; drag = 0; ed_focus = 0;
    tx_render();
}

static void ed_add_picture(const char *path)
{
    tobj *o = tpl_add(&cur_tpl, OBJ_IMAGE);
    if (!o) { snprintf(ed_msg, sizeof ed_msg, "Too many objects"); return; }
    snprintf(o->text, sizeof o->text, "%s", path);
    image *im = img_load(path);
    if (im && tx_bg) {
        double ar = (double)im->h / im->w * tx_bg->w / tx_bg->h;   /* keep the picture's shape */
        o->w = 0.42; o->h = o->w * ar;
        if (o->h > 0.6) { o->h = 0.6; o->w = o->h / ar; }
        o->x = 0.97 - o->w; o->y = 0.03;
    }
    image_free(im);
    o->shadow = 1;
    sel = cur_tpl.n - 1; tpl_dirty = 1;
    tx_render();
}

/* selection box of an object on the screen */
static void obj_screen(const tobj *o, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = ex0 + (int)lround(o->x * ew); *y0 = ey0 + (int)lround(o->y * eh);
    *x1 = ex0 + (int)lround((o->x + o->w) * ew); *y1 = ey0 + (int)lround((o->y + o->h) * eh);
}

static void handle(int x, int y)
{
    ui_rrect(x - 5, y - 5, 11, 11, 3, RGB(255, 255, 255), 1);
    ui_rrect_line(x - 5, y - 5, 11, 11, 3, c.accent, 1);
}

static void draw_editor_canvas(void)
{
    int ph = lp_h();
    begin_buf(LX, LY, LPW, ph);
    fill(LX, LY, LPW, ph, c.bg);
    ui_rrect(LX, LY, LPW, ph, 10, RGB(30, 35, 43), 1);
    /* checkerboard hint that this is a canvas */
    for (int yy = LY + 2; yy < LY + ph - 2; yy += 16)
        for (int xx = LX + 2 + (((yy - LY) / 16) & 1) * 16; xx < LX + LPW - 2; xx += 32)
            ui_fill_a(xx, yy, 16, 16, RGB(255, 255, 255), 0.018);
    ui_rrect_line(LX, LY, LPW, ph, 10, c.accent, 0.9);
    pthread_mutex_lock(&tx_img_mutex);
    if (tx_img) {
        int dw, dh; fit_size(tx_img, LPW - 8, ph - 8, &dw, &dh);
        ex0 = LX + (LPW - dw) / 2; ey0 = LY + (ph - dh) / 2; ew = dw; eh = dh;
        ui_shadow(ex0, ey0, dw, dh, 2, 16, 0.5);
        put_rgb(tx_img, ex0, ey0, dw, dh);
    }
    pthread_mutex_unlock(&tx_img_mutex);
    chip(LX + 12, LY + 10, "EDIT", RGB(255, 255, 255), c.accent);
    { char t[96]; snprintf(t, sizeof t, "%s  -  %s, %dx%d", cur_tpl.name, tx_name(), tx_w(), tx_h());
      ui_text_fit(F_SMALL, LX + 64, LY + 23, t, c.muted, LPW - 80); }
    if (sel >= 0 && sel < cur_tpl.n) {
        const tobj *o = &cur_tpl.o[sel];
        int x0, y0, x1, y1; obj_screen(o, &x0, &y0, &x1, &y1);
        if (o->type == OBJ_LINE) {
            handle(x0, y0); handle(x1, y1);
        } else {
            int a = x0 < x1 ? x0 : x1, b = y0 < y1 ? y0 : y1, w = abs(x1 - x0), h = abs(y1 - y0);
            ui_rect(a - 3, b - 3, w + 6, h + 6, RGB(0, 0, 0));
            ui_hline_dash(a - 2, a + w + 2, b - 2, c.cyan, 1); ui_hline_dash(a - 2, a + w + 2, b + h + 2, c.cyan, 1);
            ui_vline_dash(a - 2, b - 2, b + h + 2, c.cyan, 1); ui_vline_dash(a + w + 2, b - 2, b + h + 2, c.cyan, 1);
            handle(x1 + 3, y1 + 3);
        }
    }
    end_buf();
}
/* ---- tools panel: buttons are recorded while drawing, clicks look them up ---- */
enum { B_NONE, B_DONE, B_SAVE, B_NAME, B_ADD_TEXT, B_ADD_RECT, B_ADD_ELL, B_ADD_LINE, B_ADD_PIC, B_TEXTF,
       B_FONT_P, B_FONT_N, B_OUTL, B_SHAD, B_FILLED, B_SIZE_M, B_SIZE_P, B_W_M, B_W_P, B_A_M, B_A_P,
       B_UP, B_DOWN, B_COPY, B_DEL, B_T_PREV, B_T_NEXT, B_T_NEW, B_T_DEL, B_FILL0 = 100, B_LINE0 = 200 };
typedef struct { int x, y, w, h, id; } edbtn;
static edbtn edb[96];
static int nedb = 0;
static void eb(int x, int y, int w, int h, const char *label, int active, int id)
{
    button(x, y, w, h, label, active);
    if (nedb < 96) { edb[nedb].x = x; edb[nedb].y = y; edb[nedb].w = w; edb[nedb].h = h; edb[nedb].id = id; nedb++; }
}
static void eb_area(int x, int y, int w, int h, int id)
{
    if (nedb < 96) { edb[nedb].x = x; edb[nedb].y = y; edb[nedb].w = w; edb[nedb].h = h; edb[nedb].id = id; nedb++; }
}

static const unsigned char pal[12][3] = {
    {255,255,255}, {0,0,0}, {255,230,40}, {255,140,0}, {230,30,30}, {230,40,200},
    {40,90,255}, {0,200,255}, {40,210,60}, {20,40,120}, {128,128,128}, {120,70,30} };

/* rows of the tools panel: muted label left */
static void elabel(int x, int y, const char *s) { ui_text(F_SMALL, x, y, s, c.muted); }

static void draw_editor_tools(void)
{
    int x = RX0(), y = TUN_Y, w = RW(), h = TUN_H + 10 + TXP_H;
    nedb = 0;
    begin_buf(x, y, w, h);
    fill(x, y, w, h, c.bg);
    ui_rrect(x, y, w, h, 10, c.panel, 1);
    ui_rrect_line(x, y, w, h, 10, c.accent, 0.9);
    tobj *o = sel >= 0 && sel < cur_tpl.n ? &cur_tpl.o[sel] : NULL;

    elabel(x + 10, y + 23, "Template");
    field_box(x + 72, y + 6, 170, 26, cur_tpl.name, ed_focus == 2);
    eb_area(x + 72, y + 6, 170, 26, B_NAME);
    eb(x + 248, y + 6, 54, 26, tpl_dirty ? "Save*" : "Save", tpl_dirty, B_SAVE);
    eb(x + w - 84, y + 6, 74, 26, "Done", 1, B_DONE);
    ui_fill_a(x + 10, y + 36, w - 20, 1, RGB(255, 255, 255), 0.05);

    elabel(x + 10, y + 58, "Add");
    static const char *add[5] = { "Text", "Rect", "Ellipse", "Line", "Picture" };
    for (int i = 0; i < 5; i++) eb(x + 46 + i * 78, y + 40, 72, 26, add[i], 0, B_ADD_TEXT + i);

    elabel(x + 10, y + 92, "Text");
    { int on = o && o->type == OBJ_TEXT;
      if (on) field_box(x + 46, y + 74, w - 56, 26, o->text, ed_focus == 1);
      else { ui_rrect(x + 46, y + 74, w - 55, 27, 5, c.inset, 1); ui_rrect_line(x + 46, y + 74, w - 55, 27, 5, c.grid, 1);
             ui_text(F_SMALL, x + 53, y + 91, "select a text object to edit it", c.muted); }
      eb_area(x + 46, y + 74, w - 56, 26, B_TEXTF); }
    { static const char *mac[10][2] = { {"%M","my call"}, {"%C","his"}, {"%R","RST"}, {"%T","UTC"}, {"%D","date"}, {"%F","MHz"},
                                        {"%S","SNR"}, {"%O","offset"}, {"%K","clock"}, {"%Q","report"} };
      int mx = x + 46, tw = 0;                       /* spread the ten over the row, whatever the font size */
      for (int i = 0; i < 10; i++) tw += ui_text_w(F_SMALLB, mac[i][0]) + 3 + ui_text_w(F_SMALL, mac[i][1]);
      int gap = (w - 56 - tw) / 9; if (gap > 12) gap = 12; if (gap < 3) gap = 3;
      for (int i = 0; i < 10; i++) { mx += ui_text(F_SMALLB, mx, y + 116, mac[i][0], c.cyan) + 3; mx += ui_text(F_SMALL, mx, y + 116, mac[i][1], c.muted) + gap; } }

    elabel(x + 10, y + 142, "Font");
    eb(x + 46, y + 124, 26, 26, "<", 0, B_FONT_P);
    { const char *fn = o && o->type == OBJ_TEXT ? o->font : tpl_font_name(0);
      char t[64]; snprintf(t, sizeof t, "%.40s", fn[0] ? fn : "-"); size_t L = strlen(t); if (L > 4 && !strcasecmp(t + L - 4, ".ttf")) t[L - 4] = 0;
      ui_rrect(x + 76, y + 124, 171, 27, 5, c.inset, 1);
      ui_text_fit(F_REG, x + 83, y + 142, t, o && o->type == OBJ_TEXT ? c.text : c.muted, 158); }
    eb(x + 250, y + 124, 26, 26, ">", 0, B_FONT_N);
    eb(x + 286, y + 124, 76, 26, "Outline", o && o->outline, B_OUTL);
    eb(x + 368, y + 124, 70, 26, "Shadow", o && o->shadow, B_SHAD);
    eb(x + 444, y + 124, 64, 26, "Filled", o && o->filled, B_FILLED);

    static const char *rown[2] = { "Fill", "Line" };
    for (int r = 0; r < 2; r++) {
        int yy = y + 160 + r * 30;
        elabel(x + 10, yy + 16, rown[r]);
        const unsigned char *cur = o ? (r ? o->line : o->fill) : NULL;
        for (int i = 0; i < 12; i++) {
            int sx = x + 46 + i * 30;
            int same = cur && !memcmp(cur, pal[i], 3);
            if (same) ui_rrect_line(sx - 3, yy - 3, 32, 30, 7, c.accent, 1);
            ui_rrect(sx, yy, 26, 24, 5, rgb(pal[i][0], pal[i][1], pal[i][2]), 1);
            ui_rrect_line(sx, yy, 26, 24, 5, RGB(255, 255, 255), same ? 0.6 : 0.12);
            eb_area(sx, yy, 26, 24, (r ? B_LINE0 : B_FILL0) + i);
        }
    }

    { int yy = y + 226; char t[32];
      elabel(x + 10, yy + 18, "Size");
      eb(x + 46, yy, 26, 26, "-", 0, B_SIZE_M);
      if (o) snprintf(t, sizeof t, "%.0f%%", (o->type == OBJ_TEXT ? o->h : fmax(fabs(o->w), fabs(o->h))) * 100); else snprintf(t, sizeof t, "-");
      ui_text(F_BOLD, x + 97 - ui_text_w(F_BOLD, t) / 2, yy + 18, t, c.text);
      eb(x + 122, yy, 26, 26, "+", 0, B_SIZE_P);
      elabel(x + 166, yy + 18, "Width");
      eb(x + 210, yy, 26, 26, "-", 0, B_W_M);
      if (o) snprintf(t, sizeof t, "%.1f", o->lw); else snprintf(t, sizeof t, "-");
      ui_text(F_BOLD, x + 257 - ui_text_w(F_BOLD, t) / 2, yy + 18, t, c.text);
      eb(x + 278, yy, 26, 26, "+", 0, B_W_P);
      elabel(x + 322, yy + 18, "Alpha");
      eb(x + 366, yy, 26, 26, "-", 0, B_A_M);
      if (o) snprintf(t, sizeof t, "%d%%", o->alpha); else snprintf(t, sizeof t, "-");
      ui_text(F_BOLD, x + 417 - ui_text_w(F_BOLD, t) / 2, yy + 18, t, c.text);
      eb(x + 442, yy, 26, 26, "+", 0, B_A_P); }

    { int yy = y + 262;
      elabel(x + 10, yy + 18, "Layer");
      eb(x + 56, yy, 60, 26, "Up", 0, B_UP);
      eb(x + 120, yy, 60, 26, "Down", 0, B_DOWN);
      eb(x + 184, yy, 60, 26, "Copy", 0, B_COPY);
      eb(x + 248, yy, 70, 26, "Delete", 0, B_DEL);
      char t[48]; snprintf(t, sizeof t, "%d object%s", cur_tpl.n, cur_tpl.n == 1 ? "" : "s");
      chip(x + 332, yy + 4, t, c.muted, c.panel2); }
    ui_fill_a(x + 10, y + 295, w - 20, 1, RGB(255, 255, 255), 0.05);

    { int yy = y + 302;
      elabel(x + 10, yy + 18, "Templates");
      eb(x + 84, yy, 26, 26, "<", 0, B_T_PREV);
      eb(x + 114, yy, 26, 26, ">", 0, B_T_NEXT);
      eb(x + 150, yy, 56, 26, "New", 0, B_T_NEW);
      eb(x + 212, yy, 96, 26, del_confirm ? "Really?" : "Delete tpl", del_confirm, B_T_DEL);
      char t[48]; snprintf(t, sizeof t, "%d saved", tpl_count);
      chip(x + 320, yy + 4, t, c.muted, c.panel2); }

    if (ed_msg[0]) { ui_circle(x + 15, y + 348, 4, c.yellow, 1); ui_text_fit(F_SMALL, x + 25, y + 352, ed_msg, c.yellow, w - 35); }
    ui_text(F_SMALL, x + 10, y + 378, "Tip: click a received picture in the gallery below to paste it into the template.", c.muted);
    end_buf();
}
static void ed_changed(void) { tpl_dirty = 1; tx_render(); }

static void ed_tool(int id)
{
    tobj *o = sel >= 0 && sel < cur_tpl.n ? &cur_tpl.o[sel] : NULL;
    if (id != B_T_DEL) del_confirm = 0;
    ed_focus = 0; ed_fresh = 0;
    ed_msg[0] = 0;
    if (id >= B_FILL0 && id < B_FILL0 + 12) { if (o) { memcpy(o->fill, pal[id - B_FILL0], 3); if (o->type == OBJ_RECT || o->type == OBJ_ELLIPSE) o->filled = 1; ed_changed(); } return; }
    if (id >= B_LINE0 && id < B_LINE0 + 12) { if (o) { memcpy(o->line, pal[id - B_LINE0], 3); if (o->type == OBJ_TEXT) o->outline = 1; if (o->lw < 0.5) o->lw = 2; ed_changed(); } return; }
    switch (id) {
    case B_DONE: edit_close(); break;
    case B_SAVE: tpl_save_cur(); break;
    case B_NAME: ed_focus = 2; break;
    case B_ADD_TEXT: case B_ADD_RECT: case B_ADD_ELL: case B_ADD_LINE: {
        static const obj_type ty[4] = { OBJ_TEXT, OBJ_RECT, OBJ_ELLIPSE, OBJ_LINE };
        if (tpl_add(&cur_tpl, ty[id - B_ADD_TEXT])) { sel = cur_tpl.n - 1; if (id == B_ADD_TEXT) { ed_focus = 1; ed_fresh = 1; } ed_changed(); }
        else snprintf(ed_msg, sizeof ed_msg, "Too many objects");
        break; }
    case B_ADD_PIC:
        if (ngal > 0) ed_add_picture(gal[0].path);
        else snprintf(ed_msg, sizeof ed_msg, "Nothing received yet - pictures come from the gallery");
        break;
    case B_TEXTF: if (o && o->type == OBJ_TEXT) ed_focus = 1; else snprintf(ed_msg, sizeof ed_msg, "Select a text object first"); break;
    case B_FONT_P: case B_FONT_N:
        if (o && o->type == OBJ_TEXT && tpl_font_count() > 0) {
            int n = tpl_font_count(), i = (tpl_font_index(o->font) + (id == B_FONT_N ? 1 : -1) + n) % n;
            snprintf(o->font, sizeof o->font, "%s", tpl_font_name(i)); ed_changed();
        }
        break;
    case B_OUTL: if (o) { o->outline = !o->outline; if (o->outline && o->lw < 0.5) o->lw = 2; ed_changed(); } break;
    case B_SHAD: if (o) { o->shadow = !o->shadow; ed_changed(); } break;
    case B_FILLED: if (o) { o->filled = !o->filled; ed_changed(); } break;
    case B_SIZE_M: case B_SIZE_P:
        if (o) {
            double f = id == B_SIZE_P ? 1.1 : 1 / 1.1;
            if (o->type == OBJ_TEXT) o->h = fmax(0.02, fmin(0.6, o->h * f));
            else { double cx = o->x + o->w / 2, cy = o->y + o->h / 2; o->w *= f; o->h *= f; o->x = cx - o->w / 2; o->y = cy - o->h / 2; }
            ed_changed();
        }
        break;
    case B_W_M: if (o) { o->lw = fmax(0, o->lw - 0.5); ed_changed(); } break;
    case B_W_P: if (o) { o->lw = fmin(20, o->lw + 0.5); ed_changed(); } break;
    case B_A_M: if (o) { o->alpha = o->alpha > 10 ? o->alpha - 10 : 10; ed_changed(); } break;
    case B_A_P: if (o) { o->alpha = o->alpha < 90 ? o->alpha + 10 : 100; ed_changed(); } break;
    case B_UP: if (o && sel < cur_tpl.n - 1) { tpl_move(&cur_tpl, sel, 1); sel++; ed_changed(); } break;
    case B_DOWN: if (o && sel > 0) { tpl_move(&cur_tpl, sel, -1); sel--; ed_changed(); } break;
    case B_COPY:
        if (o && cur_tpl.n < TPL_MAXOBJ) {
            cur_tpl.o[cur_tpl.n] = *o; cur_tpl.o[cur_tpl.n].cache = NULL; cur_tpl.o[cur_tpl.n].cache_path[0] = 0;
            cur_tpl.o[cur_tpl.n].x += 0.03; cur_tpl.o[cur_tpl.n].y += 0.03;
            sel = cur_tpl.n++; ed_changed();
        }
        break;
    case B_DEL: if (o) { tpl_delete(&cur_tpl, sel); sel = -1; ed_changed(); } break;
    case B_T_PREV: tpl_step(-1); break;
    case B_T_NEXT: tpl_step(1); break;
    case B_T_NEW: {
        if (tpl_dirty) tpl_save_cur();
        char nm[48]; int k = 1;
        do { snprintf(nm, sizeof nm, k == 1 ? "New" : "New %d", k); k++; }
        while (k < 100 && ({ int f = 0; for (int i = 0; i < tpl_count; i++) f |= !strcmp(tpl_names[i], nm); f; }));
        tpl_clear(&cur_tpl); tpl_init(&cur_tpl, nm); sel = -1;
        tpl_save_cur(); tx_render(); ed_focus = 2;
        snprintf(ed_msg, sizeof ed_msg, "New template - type its name, then add objects");
        break; }
    case B_T_DEL:
        if (!del_confirm) { del_confirm = 1; break; }
        del_confirm = 0;
        tpl_remove(tpl_dir, cur_tpl.name);
        tpl_refresh_list();
        if (tpl_count == 0) { tpl_make_defaults(tpl_dir, "%M"); tpl_refresh_list(); }
        tpl_select(tpl_names[0]); config_save(); tx_render();
        snprintf(ed_msg, sizeof ed_msg, "Template deleted");
        break;
    }
}

/* mouse in editor mode; returns 1 if used */
static int ed_click(int mx, int my, int btn)
{
    /* tools */
    if (mx >= RX0() && my >= TUN_Y && my < TXP_Y + TXP_H) {
        if (btn != 1) return 1;
        for (int i = nedb - 1; i >= 0; i--)
            if (hit(mx, my, edb[i].x, edb[i].y, edb[i].w, edb[i].h)) {
                int was = ed_focus, id = edb[i].id;
                ed_tool(id);
                /* a click in the focused text / name field moves the caret there */
                char *f = id == B_TEXTF && sel >= 0 && sel < cur_tpl.n ? cur_tpl.o[sel].text : id == B_NAME ? cur_tpl.name : NULL;
                if (f && ((id == B_TEXTF && was == 1 && ed_focus == 1) || (id == B_NAME && was == 2 && ed_focus == 2)))
                    field_click(F_REG, edb[i].x, edb[i].w, f, mx);
                else if (f) caret_buf = NULL;              /* newly focused: caret at the end */
                break;
            }
        draw_editor_tools(); draw_editor_canvas(); draw_rx_status(); flush_ui();
        return 1;
    }
    /* canvas (whole left panel) */
    if (mx >= LX && mx < LX + LPW && my >= LY && my < LY + lp_h()) {
        if (btn != 1) { if (btn == 3) { sel = -1; draw_editor_canvas(); draw_editor_tools(); } return 1; }
        ed_focus = 0; del_confirm = 0; ed_fresh = 0;
        double fx = (mx - ex0) / (double)ew, fy = (my - ey0) / (double)eh;
        drag = 0;
        if (sel >= 0 && sel < cur_tpl.n) {
            const tobj *o = &cur_tpl.o[sel];
            int x0, y0, x1, y1; obj_screen(o, &x0, &y0, &x1, &y1);
            if (abs(mx - x1 - 3) <= 7 && abs(my - y1 - 3) <= 7) drag = 2;            /* corner / line end */
            else if (o->type == OBJ_LINE && abs(mx - x0) <= 7 && abs(my - y0) <= 7) drag = 3;
            else if (o->type == OBJ_LINE && abs(mx - x1) <= 7 && abs(my - y1) <= 7) drag = 2;
        }
        if (!drag) {
            sel = tpl_hit(&cur_tpl, fx, fy, tx_bg ? (double)tx_bg->h / tx_bg->w : 0.8);
            if (sel >= 0) drag = 1;
        }
        if (sel >= 0) drag_start = cur_tpl.o[sel];
        drag_fx = fx; drag_fy = fy;
        draw_editor_canvas(); draw_editor_tools(); flush_ui();
        return 1;
    }
    /* gallery: handled by right_click (pastes the picture) */
    return 0;
}

static void ed_motion(int mx, int my)
{
    if (!drag || sel < 0 || sel >= cur_tpl.n) return;
    double dx = (mx - ex0) / (double)ew - drag_fx, dy = (my - ey0) / (double)eh - drag_fy;
    tobj *o = &cur_tpl.o[sel];
    const tobj *s = &drag_start;
    if (drag == 1) { o->x = s->x + dx; o->y = s->y + dy; }
    else if (drag == 2) {
        if (o->type == OBJ_TEXT) o->h = fmax(0.02, fmin(0.6, s->h + dy));
        else if (o->type == OBJ_LINE) { o->w = s->w + dx; o->h = s->h + dy; }
        else { o->w = fmax(0.02, s->w + dx); o->h = fmax(0.02, s->h + dy); }
    } else if (drag == 3) { o->x = s->x + dx; o->y = s->y + dy; o->w = s->w - dx; o->h = s->h - dy; }
    tpl_dirty = 1;
    tx_render();
    draw_editor_canvas();
    flush_ui();
}

static void ed_release(void)
{
    if (!drag) return;
    drag = 0;
    draw_editor_tools(); flush_ui();
}

static void ed_key(KeySym ks, const char *kb, int kl)
{
    tobj *o = sel >= 0 && sel < cur_tpl.n ? &cur_tpl.o[sel] : NULL;
    if (ed_focus == 1 && o && o->type == OBJ_TEXT) {
        if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter || ks == XK_Tab) { ed_focus = 0; return; }
        if (kl > 0 || ks == XK_BackSpace || ks == XK_Delete) {             /* (not on Shift or an arrow) */
            if (ed_fresh) { o->text[0] = 0; caret_buf = NULL; }            /* typing replaces the default */
            ed_fresh = 0;
        }
        if (line_edit(o->text, sizeof o->text, ks, kb, kl, NULL)) ed_changed();
        else draw_editor_tools();                                          /* the caret moved */
        return;
    }
    if (ed_focus == 2) {
        if (ks == XK_Escape || ks == XK_Tab) { ed_focus = 0; return; }
        if (ks == XK_Return || ks == XK_KP_Enter) { ed_focus = 0; tpl_save_cur(); return; }
        if (line_edit(cur_tpl.name, 40, ks, kb, kl, filt_name)) tpl_dirty = 1;
        else draw_editor_tools();
        return;
    }
    ed_focus = 0;
    if (ks == XK_Escape) { if (sel >= 0) sel = -1; else edit_close(); return; }
    if (ks == XK_F1) { snprintf(ed_msg, sizeof ed_msg, "Press Done first, then send"); return; }
    if (!o) return;
    double px = tx_bg ? 1.0 / tx_bg->w : 0.003, py = tx_bg ? 1.0 / tx_bg->h : 0.004;
    if (ks == XK_Delete || ks == XK_BackSpace) { tpl_delete(&cur_tpl, sel); sel = -1; ed_changed(); }
    else if (ks == XK_Left) { o->x -= px; ed_changed(); }
    else if (ks == XK_Right) { o->x += px; ed_changed(); }
    else if (ks == XK_Up) { o->y -= py; ed_changed(); }
    else if (ks == XK_Down) { o->y += py; ed_changed(); }
}

/* clicks in the right column; returns 1 if handled */
static int right_click(int x, int y, int btn)
{
    int X = RX0(), cx = X + 264 + 22, cw = RW() - 264 - 32;
    if (!edit_mode && y >= TXP_Y && y < TXP_Y + TXP_H && x >= X) {
        if (hit(x, y, cx + 44, TXP_Y + 66, 120, 28) && btn >= 3 && btn <= 5 && !tx_active()) {   /* back: right click / wheel up */
            int d = btn == 5 ? 1 : -1;
            tx_mode_set(tx_mode_i + d); config_save(); tx_prepare(0);
            return 1;
        }
        if (!tx_is_drm() && !tx_active() && btn == 1 && hit(x, y, cx + 44, TXP_Y + 187, 120, 26)) {
            cfg.fskid = !cfg.fskid; config_save(); draw_tx_panel(); return 1;
        }
        {   /* own clock correction on/off (right click: forget the measurements) */
            int bx = cx + 172, bw = tx_is_drm() ? 0 : cw - 172;   /* analog: next to FSK ID (the DRM row is full) */
            if (!tx_active() && bw >= 90 && (btn == 1 || btn == 3) && hit(x, y, bx, TXP_Y + 187, bw, 26)) {
                if (btn == 3) { cfg.nclk = 0; cfg.txclk = 0; live_set_clock(lv, NAN); }
                else if (!isnan(own_clock())) cfg.txclk = !cfg.txclk;
                config_save(); draw_tx_panel(); return 1;
            }
        }
        if (tx_is_drm() && !tx_active() && btn >= 1 && btn <= 5 && btn != 2 && y >= TXP_Y + 185) {
            int d = (btn == 3 || btn == 4) ? -1 : 1, hitp = 1, size = 0;
            if (hit(x, y, cx + 44, TXP_Y + 187, 34, 26)) cfg.drm_rob = (cfg.drm_rob + 3 + d) % 3;
            else if (hit(x, y, cx + 82, TXP_Y + 187, 70, 26)) cfg.drm_qam = (cfg.drm_qam + 3 + d) % 3;
            else if (hit(x, y, cx + 156, TXP_Y + 187, 52, 26)) cfg.drm_prot = !cfg.drm_prot;
            else if (hit(x, y, cx + 212, TXP_Y + 187, 50, 26)) cfg.drm_so = !cfg.drm_so;
            else if (hit(x, y, cx + 44, TXP_Y + 217, 84, 26)) { cfg.drm_size = (cfg.drm_size + NDRMSIZE + d) % NDRMSIZE; size = 1; }
            else if (hit(x, y, cx + 132, TXP_Y + 217, 84, 26)) { cfg.drm_kb = (cfg.drm_kb + NDRMKB + d) % NDRMKB; size = 1; }
            else if (hit(x, y, cx + 220, TXP_Y + 217, 58, 26)) cfg.drm_rs = (cfg.drm_rs + 5 + d) % 5;
            else hitp = 0;
            if (hitp) { config_save(); if (size) tx_prepare(0); return 1; }
        }
        if (btn != 1) return 1;
        if (tx_active() && !hit(x, y, cx, TXP_Y + 144, 120, 36)) { tx_set_msg("Wait for the end of the transmission (or abort)"); return 1; }
        int nw = cw - 44 - 26 - 26 - 70 - 12;
        if (hit(x, y, cx, TXP_Y + 28, 34, 28) && tx_nfiles) { tx_idx = (tx_idx + tx_nfiles - 1) % tx_nfiles; tx_prepare(1); }
        else if (hit(x, y, cx + 40, TXP_Y + 28, 34, 28) && tx_nfiles) { tx_idx = (tx_idx + 1) % tx_nfiles; tx_prepare(1); }
        else if (hit(x, y, cx + 44, TXP_Y + 66, 120, 28)) { tx_mode_set(tx_mode_i + 1); config_save(); tx_prepare(0); }
        else if (hit(x, y, cx + 44, TXP_Y + 104, 26, 28)) tpl_step(-1);
        else if (hit(x, y, cx + 78 + nw, TXP_Y + 104, 26, 28)) tpl_step(1);
        else if (hit(x, y, cx + cw - 64, TXP_Y + 104, 64, 28)) edit_open();
        else if (hit(x, y, cx, TXP_Y + 144, 120, 36)) tx_toggle();
        else if (hit(x, y, cx + 130, TXP_Y + 148, 80, 28) && fix_idx >= 0) { tx_kind = 2; tx_set_msg(""); atomic_store(&tx_req, 1); }
        else if (hit(x, y, cx + 130, TXP_Y + 148, 80, 28)) { tx_scan(); tx_prepare(1); }
        return 1;
    }
    if (y >= GAL_Y && y < log_bar_y() - 12 && x >= X) {
        if (btn == 4) { if (gal_scroll > 0) gal_scroll--; return 1; }
        if (btn == 5) { if ((gal_scroll + gal_rows()) * gal_cols() < ngal) gal_scroll++; return 1; }
        if (btn != 1) return 1;
        int k = (x - X - 8) / (TW + 8), r = (y - GAL_Y - 26) / (TH + 8);
        if (k >= 0 && k < gal_cols() && r >= 0 && y >= GAL_Y + 26) {
            int i = gal_scroll * gal_cols() + r * gal_cols() + k;
            if (i < ngal) { if (edit_mode) ed_add_picture(gal[i].path); else open_view(gal[i].path); }
        }
        return 1;
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-r host:port|none] [-c CALLSIGN] [-l log.adi] [-f test.wav [-s speed]] [-x tx.wav]\n"
                    "  The radio is normally set up in the program (Radio button): the program then\n"
                    "  starts and stops Hamlib's rigctld by itself and remembers the settings.\n"
                    "  -r  use an already running rigctld instead ('none' = no PTT control / VOX)\n"
                    "  -c  your callsign (saved; %%M in the picture templates)\n"
                    "  -l  ADIF log file (default ~/digipan_log.adi, shared with DigiPan)\n"
                    "  -f  test: play a WAV recording into the receiver instead of the sound card\n"
                    "  -s  test: speed of -f (e.g. 10 = ten times real time)\n"
                    "  -x  test: transmit into a WAV file instead of the sound card\n", prog);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    win_setup_home();                    /* HOME = the user profile folder, so ~/... paths work */
#endif
    config_load();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            const char *a = argv[++i];
            cli_rig = 1;
            if (strcmp(a, "none")) {
                char host[64] = "127.0.0.1"; int port = 4532;
                const char *col = strrchr(a, ':');
                if (col) { size_t n = (size_t)(col - a); if (n >= sizeof host) n = sizeof host - 1; memcpy(host, a, n); host[n] = 0; port = atoi(col + 1); }
                else snprintf(host, sizeof host, "%.60s", a);
                if (port <= 0 || port > 65535) { usage(argv[0]); return 1; }
                rig_use_external(host, port);
            }
        } else if (!strcmp(argv[i], "-l") && i + 1 < argc) snprintf(log_path, sizeof log_path, "%.290s", argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            snprintf(cfg.call, sizeof cfg.call, "%.20s", argv[++i]);
            for (char *q = cfg.call; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        }
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) snprintf(test_wav, sizeof test_wav, "%.290s", argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) test_speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) snprintf(tx_wav, sizeof tx_wav, "%.290s", argv[++i]);
        else { usage(argv[0]); return strcmp(argv[i], "-h") ? 1 : 0; }
    }
    adif_open();
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
#ifndef _WIN32
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);
#endif
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open X display. Run inside an X11 desktop session.\n"); return 1; }
    scr = DefaultScreen(dpy); vis = DefaultVisual(dpy, scr); depth = DefaultDepth(dpy, scr);
    if (vis->class != TrueColor) { fprintf(stderr, "A true-colour display is needed.\n"); return 1; }
    init_palette();
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 40, 40, W0, H0, 1, BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XStoreName(dpy, win, APP);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | StructureNotifyMask);
    XSizeHints hints = { 0 }; hints.flags = PMinSize; hints.min_width = MIN_W; hints.min_height = MIN_H; XSetWMNormalHints(dpy, win, &hints);
    XSetIOErrorHandler(on_x_io_error);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    gc = XCreateGC(dpy, win, 0, NULL);
    if (ui_init(dpy, win, vis, depth) != 0) { fprintf(stderr, "Cannot set up the drawing buffer.\n"); return 1; }
    XMapWindow(dpy, win);

    if (cfg.drm_rob < 0 || cfg.drm_rob > 2) cfg.drm_rob = 1;
    if (cfg.drm_qam < 0 || cfg.drm_qam > 2) cfg.drm_qam = 1;
    if (cfg.drm_size < 0 || cfg.drm_size >= NDRMSIZE) cfg.drm_size = 0;
    if (cfg.drm_kb < 0 || cfg.drm_kb >= NDRMKB) cfg.drm_kb = 3;
    cfg.drm_prot = !!cfg.drm_prot; cfg.drm_so = !!cfg.drm_so;
    if (cfg.drm_rs < 0 || cfg.drm_rs > 4) cfg.drm_rs = 0;        /* (rs_kk() of anything else is 0: a division) */
    if (!strcmp(cfg.tx_mode, "drm")) tx_mode_i = NMODEIDS;
    for (int i = 0; i < NMODEIDS; i++) {
        if (!strcmp(MODE_ID(i), cfg.tx_mode)) tx_mode_i = i;
        if (!strcmp(MODE_ID(i), cfg.rx_mode)) rx_force = i;
    }
    mkdir_p(cfg.rx_dir); mkdir_p(cfg.tx_dir);
    lv = live_new(AUDIO_RATE, cfg.rx_dir);
    if (lv) live_set_clock(lv, own_clock());
    live_set_nr(lv, cfg.nr);
    apply_rx_force();
    gal_load_dir();
    { const char *h = getenv("HOME"); snprintf(tpl_dir, sizeof tpl_dir, "%.250s/.config/sstvnext/templates", h && *h ? h : "/tmp"); }
    tpl_make_defaults(tpl_dir, cfg.overlay);        /* first start: Call (the old text), CQ, QSO reply */
    if (!cfg.tpl_report) { tpl_make_report(tpl_dir); cfg.tpl_report = 1; config_save(); }   /* once, also for older installs */
    tpl_refresh_list();
    tpl_select(cfg.tpl[0] ? cfg.tpl : "Call");
    tx_scan(); tx_prepare(1);

    audio_device_count = audio_list(audio_devices, 30);
    audio_set_channel(cfg.in_ch);
    if (test_wav[0]) audio_start_file(test_wav, AUDIO_RATE, test_speed, on_audio, NULL, audio_status, sizeof audio_status);
    else for (int i = 0; i < audio_device_count; i++) if (cfg.audio[0] && !strcmp(audio_devices[i], cfg.audio)) { audio_select(i); break; }

    if (!cli_rig && cfg_cat && cfg_model > 0) rig_apply();   /* start the saved radio link */
    atomic_store(&tx_running, 1);
    pthread_create(&tx_thread, NULL, tx_worker, NULL);

    struct timespec pause = { 0, 40000000 };
    unsigned ticks = 0;
    int quit = 0, was_tx = 0;
    while (!quit) {
        if (quit_sig) quit = 1;
        while (XPending(dpy)) {
            XEvent e; XNextEvent(dpy, &e);
            if (e.type == Expose) { if (e.xexpose.count == 0) draw(); }
            else if (e.type == ClientMessage) { if ((Atom)e.xclient.data.l[0] == wm_delete) quit = 1; }
            else if (e.type == ConfigureNotify) {
                if (e.xconfigure.width != width || e.xconfigure.height != height) { width = e.xconfigure.width; height = e.xconfigure.height; ui_resize(width, height); draw(); }
            }
            else if (e.type == KeyPress) {
                char kb[16]; KeySym ks = NoSymbol;
                int kl = XLookupString(&e.xkey, kb, (int)sizeof kb, &ks, NULL);
                if (rig_dlg) { rig_dlg_key(ks, kb, kl); draw(); continue; }
                if (log_dlg) { log_dlg_key(ks); draw(); continue; }
                if (log_focus >= 0) { log_key(ks, kb, kl); draw_log_bar(); flush_ui(); continue; }
                if (edit_mode) { ed_key(ks, kb, kl); draw_editor_tools(); draw_editor_canvas(); draw_rx_status(); flush_ui(); continue; }
                int txon = tx_active();
                if (ks == XK_Escape) { if (txon) { atomic_store(&tx_abort, 1); draw(); } else if (view_img) { close_view(); draw(); } else quit = 1; }
                else if (ks == XK_F1) { tx_toggle(); draw(); }
                else if (!txon && ks == XK_Left && tx_nfiles) { tx_idx = (tx_idx + tx_nfiles - 1) % tx_nfiles; tx_prepare(1); draw_tx_panel(); }
                else if (!txon && ks == XK_Right && tx_nfiles) { tx_idx = (tx_idx + 1) % tx_nfiles; tx_prepare(1); draw_tx_panel(); }
                else if (!txon && ks == XK_q) quit = 1;
            }
            else if (e.type == MotionNotify) {
                while (XCheckTypedWindowEvent(dpy, win, MotionNotify, &e)) {}      /* only the newest position */
                if (edit_mode) ed_motion(e.xmotion.x, e.xmotion.y);
            }
            else if (e.type == ButtonRelease) { if (edit_mode) ed_release(); }
            else if (e.type == ButtonPress) {
                int bx = e.xbutton.x, by = e.xbutton.y, btn = (int)e.xbutton.button;
                if (rig_dlg) { rig_dlg_click(bx, by, btn); draw(); continue; }
                if (log_dlg) { log_dlg_click(bx, by, btn); draw(); continue; }
                if (scan_menu) {                          /* frequency list of the scanner */
                    int i = (by - 78 - 28) / 25;
                    if (btn == 1 && bx >= 900 && bx < 1090 && by >= 106 && i >= 0 && i < NSCAN) {
                        cfg.scan_on ^= 1u << i; config_save(); draw_scan_menu(); flush_ui(); continue;
                    }
                    scan_menu = 0; draw(); continue;
                }
                if (hit(bx, by, 900, 43, 94, 32) && (btn == 1 || btn == 3)) {
                    if (btn == 3) { if (!tx_active()) { scan_menu = 1; draw_scan_menu(); flush_ui(); } continue; }
                    if (tx_active()) continue;
                    if (scan_state == SC_OFF || scan_state == SC_HOLD) scan_start(); else scan_stop();
                    draw_scan_button(); draw_rx_msg(); flush_ui(); continue;
                }
                if (audio_menu) {
                    int cy = CH_ROW_Y(audio_device_count) + 2;
                    if (btn == 1 && by >= cy && by < cy + 24 && bx >= 186 && bx < 186 + 3 * 74) {   /* channel */
                        int ch = (bx - 186) / 74;
                        if (ch != cfg.in_ch) {
                            cfg.in_ch = ch; config_save(); audio_set_channel(ch);
                            if (test_wav[0]) audio_start_file(test_wav, AUDIO_RATE, test_speed, on_audio, NULL, audio_status, sizeof audio_status);
                            else if (audio_device_selected >= 0) audio_select(audio_device_selected);
                            live_reset(lv);
                        }
                        draw_audio_menu_overlay(); flush_ui(); continue;
                    }
                    if (btn == 1 && bx >= 104 && bx <= 414 && by >= 75 && by < 75 + audio_device_count * 25 + 8) {
                        int idx = (by - 79) / 25;
                        if (idx >= 0 && idx < audio_device_count) { test_wav[0] = 0; audio_select(idx); }
                    }
                    audio_menu = 0; draw(); continue;
                }
                if (btn == 1) {
                    int sx, sw; support_rect(&sx, &sw);
                    if (hit(bx, by, sx, 7, sw, 22)) { open_url(SUPPORT_URL); continue; }
                }
                if (log_bar_click(bx, by, btn)) { if (log_dlg) draw(); else { draw_log_bar(); draw_tx_panel(); flush_ui(); } continue; }
                if (edit_mode && ed_click(bx, by, btn)) { if (!edit_mode) draw(); continue; }
                if (right_click(bx, by, btn)) { draw(); continue; }
                if (hit(bx, by, 440, 43, 140, 32) && btn >= 3 && btn <= 5) {          /* RX mode back / wheel */
                    int d = btn == 5 ? 1 : -1, n = NMODEIDS + 1;                      /* -1 = auto */
                    rx_force = (rx_force + 1 + d + n) % n - 1;
                    apply_rx_force(); config_save(); draw(); continue;
                }
                if (btn != 1) continue;
                if (hit(bx, by, 104, 43, 260, 32)) {
                    if (tx_active()) continue;
                    audio_device_count = audio_list(audio_devices, 30);
                    audio_menu = 1; draw_audio_menu_overlay(); flush_ui();
                }
                else if (hit(bx, by, 440, 43, 140, 32)) { rx_force = rx_force + 1 >= NMODEIDS ? -1 : rx_force + 1; apply_rx_force(); config_save(); draw(); }
                else if (hit(bx, by, 590, 43, 100, 32)) live_stop(lv);
                else if (hit(bx, by, 700, 43, 90, 32)) { rig_dlg_open(); draw(); }
                else if (hit(bx, by, 800, 43, 90, 32)) { cfg.nr = !cfg.nr; live_set_nr(lv, cfg.nr); config_save(); draw(); }
                else if (view_img && hit(bx, by, LX + LPW - 80, LY + lp_h() + 8, 80, 28)) { close_view(); draw(); }
                else if (!view_img && !edit_mode && rx_st.drm_can_bsr && !tx_active() &&
                         hit(bx, by, LX + LPW - 112, LY + lp_h() + 14, 112, 26)) {
                    tx_bsr_len = live_bsr_request(lv, tx_bsr_text, sizeof tx_bsr_text);
                    if (tx_bsr_len > 0) { tx_kind = 1; tx_set_msg(""); atomic_store(&tx_req, 1); }
                }
            }
        }

        /* live picture: redraw when the engine has something new */
        image *ni = NULL; live_status st;
        int newpic = live_snapshot(lv, &rx_gen, &ni, &st);
        int started = st.state == LIVE_RX && rx_st.state != LIVE_RX;
        if (st.state != LIVE_RX && rx_st.state == LIVE_RX && st.mode) {      /* analog picture finished */
            clock_learn(&st);
            last_rep.valid = 1; last_rep.snr = st.snr; last_rep.offset = st.offset;
            last_rep.ppm = isnan(own_clock()) ? st.clock_ppm : st.clock_ppm - own_clock();
            last_rep.syncs = st.syncs; last_rep.lines = st.lines_total; last_rep.call[0] = 0;
            if (!view_img) { tx_render(); if (!(rig_dlg || log_dlg || audio_menu || scan_menu) && !edit_mode) draw_tx_panel(); }   /* %S %O %K %Q in the reply */
        }
        int stchg = memcmp(&st, &rx_st, sizeof st) != 0;
        int drmchg = st.drm_sync != rx_st.drm_sync || strcmp(st.drm_call, rx_st.drm_call) || strcmp(st.drm_info, rx_st.drm_info);
        rx_st = st;
        if (st.state == LIVE_RX && st.mode) snprintf(last_mode_name, sizeof last_mode_name, "%s", st.mode->name);
        else if (st.drm_sync) snprintf(last_mode_name, sizeof last_mode_name, "DRM");
        if (newpic) { image_free(rx_img); rx_img = ni; }
        int covered = rig_dlg || log_dlg || audio_menu || scan_menu;     /* a dialog or menu lies over the panels */
        if (scan_tick(&st) && !rig_dlg && !log_dlg) { draw_scan_button(); draw_rx_msg(); if (scan_menu) draw_scan_menu(); }
        if (started && view_img) { close_view(); if (!covered) draw(); }
        else if (!covered) {
            if ((newpic || drmchg) && !view_img && !edit_mode) draw_picture();
            if (stchg) { draw_rx_status(); draw_rx_msg(); }
        }
        if (st.heard_new != seen_heard) {         /* the other station's callsign (FSK ID / DRM) */
            seen_heard = st.heard_new;
            if (last_rep.valid && !last_rep.call[0]) snprintf(last_rep.call, sizeof last_rep.call, "%s", st.heard_call);
            if (st.heard_call[0] && !his_call[0] && log_focus != 0) {
                snprintf(his_call, sizeof his_call, "%s", st.heard_call);
                if (!qso_start) qso_start = time(NULL);
                char m[80];
                snprintf(m, sizeof m, "Call %s filled in from the %s", st.heard_call, st.heard_src == 1 ? "FSK ID" : "digital signal");
                log_say(m, 1);
                tx_render();                                         /* %C in the reply */
                if (!covered && !edit_mode) { draw_log_bar(); draw_tx_panel(); }
            }
        }
        if (st.bsr_new != seen_bsr) {             /* a repeat request arrived */
            seen_bsr = st.bsr_new;
            unsigned tid; int n = live_last_bsr(lv, &tid, fix_list, 4096, fix_from, sizeof fix_from);
            int idx = -1;
            for (int i = 0; i < NSENT; i++) if (drm_sent[i].data && drm_sent[i].tid == tid) idx = i;
            char m[120];
            if (idx >= 0 && n > 0) {
                fix_idx = idx; fix_n = n;
                snprintf(m, sizeof m, "BSR from %s: %d segments of your picture missing - press Fix", fix_from[0] ? fix_from : "?", n);
            } else snprintf(m, sizeof m, "BSR from %s for another station's picture", fix_from[0] ? fix_from : "?");
            tx_set_msg(m);
            if (!covered && !edit_mode) draw_tx_panel();
        }
        char saved[300]; int got = 0;
        while (live_take_saved(lv, saved, sizeof saved)) { gal_add(saved, 1); gal_scroll = 0; got = 1; }
        if (got && !covered) draw_gallery();

        {   /* %T in the template: new minute, new preview */
            static int last_min = -1;
            time_t now = time(NULL); int mn = (int)(now / 60);
            if (mn != last_min) { if (last_min >= 0 && !drag) { tx_render(); if (!rig_dlg && !log_dlg && !audio_menu && !scan_menu) { if (edit_mode) draw_editor_canvas(); else draw_tx_panel(); } } last_min = mn; }
        }
        if (++ticks % 5 == 0) {
            if (!audio_menu && !scan_menu && !rig_dlg && !log_dlg) { update_spectrum(); draw_tuning(); draw_log_bar(); draw_status_bar(); }
            int txnow = tx_active();
            if ((txnow || was_tx) && !audio_menu && !scan_menu && !rig_dlg && !log_dlg && !edit_mode) { draw_tx_panel(); if (txnow != was_tx) draw_rx_msg(); }
            was_tx = txnow;
            flush_ui();
        }
        nanosleep(&pause, NULL);
    }
    tx_shutdown();                                   /* releases the PTT first */
    audio_stop();
    live_free(lv);
    config_save();
    ui_free();
    if (edit_mode && tpl_dirty) tpl_save_cur();
    tpl_clear(&cur_tpl);
    XFreeGC(dpy, gc); XDestroyWindow(dpy, win); XCloseDisplay(dpy);
    return 0;
}

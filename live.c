/* live.c - real-time SSTV receiver engine
 *
 * The demodulated frequency stream is kept in one growing buffer together
 * with its prefix sums, so the same offline functions (VIS search, sync
 * tracking, robust slant fit, rendering) work on live data. While an image
 * is coming in it is re-decoded twice a second: every pass refits the line
 * period over all sync pulses received so far, so the slant correction gets
 * better as the image grows and the finished image is fully corrected.
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include "live.h"
#include "demod.h"
#include "rx.h"
#include "imgio.h"
#include "drm_rx.h"
#include "jp2.h"
#include "fskid.h"

#define RING        (1 << 19)     /* capture -> engine, ~47 s at 11025 Hz */
#define NSAVED      8

struct live {
    double fs;
    pthread_t th;
    atomic_int run, mute, stop_req, reset_req, nr;

    /* capture thread -> engine */
    pthread_mutex_t rmx;
    float *ring;
    unsigned long wr, rd;

    /* engine only */
    fmdemod *dm;
    float  *f;
    double *P, *Q;
    long    n, cap;
    long    search_from;
    long    sync_from;       /* syncs before this belong to an old image */
    long    last_auto;       /* last automatic (no VIS) search */
    int     pending;         /* damaged VIS found, waiting for sync pulses to tell the mode */
    int     vchk;            /* picture started from a VIS: confirm the mode with the syncs */
    sstv_vis vis;            /* that VIS */
    long    last_work;
    int     muted_now;
    drm_rx  drm;
    int     drm_shown; long drm_shown_at;
    fskid_rx fid;
    char    drm_announced[12];
    char    bsr_req[4096]; int bsr_req_len; int bsr_have;     /* omx */
    unsigned bsr_tid; int bsr_list[4096]; int bsr_n; char bsr_from[10];   /* omx */
    double  t0, off;
    time_t  started;

    /* shared with the UI (omx) */
    pthread_mutex_t omx;
    const sstv_mode *force;
    char    dir[256];
    image  *img;
    unsigned gen;
    live_status st;
    char    saved[NSAVED][300];
    int     sv_head, sv_tail;
};

/* ------------------------------------------------------------- buffer --- */

static void buf_append(live *l, float y)
{
    if (l->n + 1 >= l->cap) {
        long nc = l->cap ? l->cap * 2 : (long)(l->fs * 60);
        l->f = realloc(l->f, (size_t)nc * sizeof(float));
        l->P = realloc(l->P, (size_t)(nc + 1) * sizeof(double));
        l->Q = realloc(l->Q, (size_t)(nc + 1) * sizeof(double));
        if (!l->cap) l->P[0] = l->Q[0] = 0;
        l->cap = nc;
    }
    l->f[l->n] = y;
    l->P[l->n + 1] = l->P[l->n] + y;
    l->Q[l->n + 1] = l->Q[l->n] + (double)y * y;
    l->n++;
}

/* drop the first k samples (prefix sums are renormalised to start at 0) */
static void buf_trim(live *l, long k)
{
    if (k <= 0) return;
    if (k > l->n) k = l->n;
    double p0 = l->P[k], q0 = l->Q[k];
    memmove(l->f, l->f + k, (size_t)(l->n - k) * sizeof(float));
    for (long i = 0; i <= l->n - k; i++) { l->P[i] = l->P[i + k] - p0; l->Q[i] = l->Q[i + k] - q0; }
    l->n -= k;
    l->search_from -= k; if (l->search_from < 0) l->search_from = 0;
    l->sync_from -= k; if (l->sync_from < 0) l->sync_from = 0;
    l->last_auto -= k; if (l->last_auto < 0) l->last_auto = 0;
    l->last_work -= k; if (l->last_work < 0) l->last_work = 0;
    l->t0 -= (double)k;
}

static void fbuf_of(const live *l, sstv_fbuf *b)
{
    b->f = l->f; b->n = l->n; b->fs = l->fs; b->P = l->P; b->Q = l->Q;
}

/* ------------------------------------------------------------ publish --- */

static void set_msg(live *l, const char *m)
{
    pthread_mutex_lock(&l->omx);
    snprintf(l->st.msg, sizeof l->st.msg, "%s", m);
    pthread_mutex_unlock(&l->omx);
}

static void publish(live *l, image *img, const live_status *st)
{
    pthread_mutex_lock(&l->omx);
    if (img) { image_free(l->img); l->img = img; l->gen++; }
    char msg[sizeof l->st.msg];
    memcpy(msg, l->st.msg, sizeof msg);
    l->st = *st;
    memcpy(l->st.msg, msg, sizeof msg);
    pthread_mutex_unlock(&l->omx);
}

static void save_image(live *l, const image *img, const sstv_mode *m, int partial)
{
    char dir[256];
    pthread_mutex_lock(&l->omx); snprintf(dir, sizeof dir, "%s", l->dir); pthread_mutex_unlock(&l->omx);
    mkdir_p(dir);
    struct tm g; gmtime_r(&l->started, &g);
    char name[64], path[300];
    strftime(name, sizeof name, "%Y%m%d_%H%M%SZ", &g);
    snprintf(path, sizeof path, "%.200s/%s_%s%s.png", dir, name, m->id, partial ? "_part" : "");
    char msg[120];
    if (img_save_png(path, img) == 0) {
        pthread_mutex_lock(&l->omx);
        snprintf(l->saved[l->sv_head % NSAVED], sizeof l->saved[0], "%s", path);
        l->sv_head++;
        if (l->sv_head - l->sv_tail > NSAVED) l->sv_tail = l->sv_head - NSAVED;
        pthread_mutex_unlock(&l->omx);
        snprintf(msg, sizeof msg, "Saved %.100s", strrchr(path, '/') + 1);
    } else {
        snprintf(msg, sizeof msg, "Cannot save the image to %.90s", dir);
    }
    set_msg(l, msg);
}

/* ------------------------------------------------------------- engine --- */

static void start_rx(live *l, const sstv_mode *m, double t0, double off, int vis)
{
    l->t0 = t0;
    l->off = off;
    l->search_from = (long)t0;
    l->vchk = 0;
    l->started = time(NULL);
    l->last_work = 0;
    live_status st; memset(&st, 0, sizeof st);
    st.state = LIVE_RX; st.mode = m; st.vis = vis; st.offset = isnan(off) ? 0 : off;
    st.lines_total = sstv_nlines(m);
    image *blank = image_new(m->width, m->height);
    publish(l, blank, &st);
    char msg[120];
    if (vis >= 0) snprintf(msg, sizeof msg, "VIS %d: %s, tuning offset %+.0f Hz", vis, m->name, off);
    else if (vis == -2) snprintf(msg, sizeof msg, "VIS damaged by QRM - %s from its sync pulses", m->name);
    else if (vis == -3) snprintf(msg, sizeof msg, "VIS misread (QRM) - the sync pulses say %s", m->name);
    else snprintf(msg, sizeof msg, "No VIS - %s recognised from its sync pulses", m->name);
    set_msg(l, msg);
}

/* finish the image being received: final decode, save, back to idle */
static void finish_rx(live *l, int reason)    /* 0 complete, 1 signal lost, 2 stopped */
{
    live_status st;
    pthread_mutex_lock(&l->omx); st = l->st; pthread_mutex_unlock(&l->omx);
    const sstv_mode *m = st.mode;
    sstv_fbuf b; fbuf_of(l, &b);
    sstv_rxinfo info;
    image *img = sstv_decode_ex(&b, m, l->t0, l->off, atomic_load(&l->nr) ? SSTV_NR : 0, &info);
    double bb = info.line_ms * l->fs / 1000.0, start = info.end - sstv_nlines(m) * bb;
    int got = reason == 1 ? info.last_sync + 1 : (int)((l->n - start) / bb);
    if (got > sstv_nlines(m)) got = sstv_nlines(m);
    if (got < 0) got = 0;
    st.state = LIVE_IDLE; st.lines = got; st.syncs = info.syncs; st.clock_ppm = info.clock_ppm; st.offset = info.offset;
    publish(l, img, &st);
    int partial = got < sstv_nlines(m);
    if (got >= sstv_nlines(m) / 5) save_image(l, img, m, partial);
    else set_msg(l, reason == 2 ? "Stopped - too little received, not saved" : "Signal lost - too little received, not saved");
    /* look for the next image right after what was used */
    long next = reason == 0 ? (long)info.end : (long)(start + got * bb);
    if (next > l->n) next = l->n;
    if (next < 0) next = 0;
    l->search_from = next;
    l->sync_from = next;
}

static void work_idle(live *l)
{
    sstv_fbuf b; fbuf_of(l, &b);
    const sstv_mode *force;
    pthread_mutex_lock(&l->omx); force = l->force; pthread_mutex_unlock(&l->omx);

    sstv_vis v;
    if (sstv_vis_find(&b, l->search_from, &v)) {
        if (v.complete) {                                /* a VIS always wins over the forced mode */
            l->pending = 0;
            start_rx(l, sstv_mode_by_vis(v.code), v.img_start, v.offset, v.code);
            l->vis = v; l->vchk = 1;                      /* two flipped bits pass the parity */
            return;
        }
        /* header damaged by QRM: the picture start is known, the mode comes from the syncs */
        const sstv_mode *m;
        int r = sstv_vis_resolve(&b, &v, 0, &m);
        if (r == 1) { l->pending = 0; start_rx(l, m, v.img_start, v.offset, -2); return; }
        if (r == 0) {
            if (!l->pending) set_msg(l, "VIS damaged by QRM - checking the sync pulses...");
            l->pending = 1;
            return;                                      /* look again with more audio */
        }
        l->pending = 0;
        set_msg(l, "VIS damaged by QRM - mode not recognised");
        l->search_from = (long)v.img_start;
        return;
    }
    l->pending = 0;
    /* start-bit positions up to n - 300 ms are checked; the next pass begins there
     * (sstv_vis_find starts 312 ms after 'from') - nothing is examined twice */
    long keep = l->n - (long)(0.615 * l->fs);
    if (keep > l->search_from) l->search_from = keep;

    if (force) {                                     /* receive without VIS: periodic syncs */
        double Ln = sstv_line_ms(force) * l->fs / 1000.0;
        long from = l->n - (long)(4 * Ln);
        if (from < l->sync_from) from = l->sync_from;
        if (from < 0) from = 0;
        double t0;
        if (l->n - from > 3 * Ln && sstv_sync_find(&b, force, from, 0, &t0)) {
            start_rx(l, force, t0, NAN, -1);
            return;
        }
    }

    else if (l->n - l->last_auto >= (long)(0.5 * l->fs)) {
        /* no VIS (missed, faded, or tuned in late): recognise the mode from its sync pulses */
        l->last_auto = l->n;
        long from = l->n - (long)(15 * l->fs);
        if (from < l->sync_from) from = l->sync_from;
        if (from < 0) from = 0;
        const sstv_mode *m; double t0;
        if (sstv_auto_find(&b, from, 0, &m, &t0)) { start_rx(l, m, t0, NAN, -1); return; }
    }

    /* keep the buffer short while nothing is coming (the automatic search looks back 15 s) */
    if (l->n > (long)(60 * l->fs)) buf_trim(l, l->n - (long)(20 * l->fs));
}

static void work_rx(live *l)
{
    live_status st;
    pthread_mutex_lock(&l->omx); st = l->st; pthread_mutex_unlock(&l->omx);
    const sstv_mode *m = st.mode;
    sstv_fbuf b; fbuf_of(l, &b);
    sstv_rxinfo info;
    image *img = sstv_decode_ex(&b, m, l->t0, l->off, atomic_load(&l->nr) ? SSTV_NR : 0, &info);
    int H = sstv_nlines(m);
    double bb = info.line_ms * l->fs / 1000.0, start = info.end - H * bb;
    int got = (int)((l->n - start) / bb);
    if (got > H) got = H;
    if (got < 0) got = 0;
    st.lines = got; st.syncs = info.syncs; st.clock_ppm = info.clock_ppm; st.offset = info.offset;
    publish(l, img, &st);

    if (l->vchk) {
        /* a few seconds in: do the sync pulses agree with the VIS? */
        const sstv_mode *m2;
        int r = sstv_vis_resolve(&b, &l->vis, 0, &m2);
        if (r != 0) l->vchk = 0;
        if (r == 1 && m2 != m) {                         /* (img now belongs to the snapshot) */
            start_rx(l, m2, l->vis.img_start, l->vis.offset, -3);
            return;
        }
    }
    if (l->n >= (long)(info.end + 0.05 * l->fs)) { finish_rx(l, 0); return; }

    /* a new VIS after the last good line: another station started - close this picture, take the new one */
    long vfrom = (long)(start + (info.last_sync + 1) * bb);
    if (vfrom < l->search_from) vfrom = l->search_from;
    sstv_vis v;
    if (sstv_vis_find(&b, vfrom, &v) && v.complete) {
        finish_rx(l, 1);
        start_rx(l, sstv_mode_by_vis(v.code), v.img_start, v.offset, v.code);
        return;
    }
    l->search_from = l->n - (long)(0.615 * l->fs);
    /* no sync for ~8 s (and at least 10 lines): the station stopped or faded out */
    int lost = (int)(8.0 / (info.line_ms / 1000.0)); if (lost < 10) lost = 10;
    if (got > 12 && got - info.last_sync > lost) finish_rx(l, 1);
}

/* ---------------------------------------------------- digital (HamDRM) --- */

static const char *qam_name[3] = {"4-QAM", "16-QAM", "64-QAM"};

static void heard(live *l, const char *call, int src)
{
    char m[120];
    pthread_mutex_lock(&l->omx);
    snprintf(l->st.heard_call, sizeof l->st.heard_call, "%s", call);
    l->st.heard_src = src;
    l->st.heard_new++;
    pthread_mutex_unlock(&l->omx);
    snprintf(m, sizeof m, "%s: %s", src == 1 ? "FSK ID" : "Digital station", call);
    set_msg(l, m);
}

static void fskid_cb(void *user, const char *call) { heard(user, call, 1); }

/* a complete picture (or file) arrived */
static void drm_done(void *user, const char *name, const uint8_t *data, long len, int partial)
{
    live *l = user;
    if (!strcmp(name, "bsr.bin")) {               /* another station asks for missing segments */
        if (partial) return;
        unsigned tid; int list[4096];
        int n = mot_bsr_parse((const char *)data, len, &tid, list, 4096);
        if (n <= 0) return;
        char m[120];
        pthread_mutex_lock(&l->omx);
        l->bsr_tid = tid; l->bsr_n = n; memcpy(l->bsr_list, list, sizeof(int) * n);
        snprintf(l->bsr_from, sizeof l->bsr_from, "%s", l->drm.label);
        l->st.bsr_new++;
        pthread_mutex_unlock(&l->omx);
        snprintf(m, sizeof m, "Repeat request (BSR) from %s: %d segments", l->drm.label[0] ? l->drm.label : "?", n);
        set_msg(l, m);
        return;
    }
    char dir[256];
    pthread_mutex_lock(&l->omx); snprintf(dir, sizeof dir, "%s", l->dir); pthread_mutex_unlock(&l->omx);
    char ddir[300]; snprintf(ddir, sizeof ddir, "%.250s/digital", dir);
    mkdir_p(ddir);
    time_t now = time(NULL); struct tm g; gmtime_r(&now, &g);
    char ts[32]; strftime(ts, sizeof ts, "%Y%m%d_%H%M%SZ", &g);
    char raw[600];
    snprintf(raw, sizeof raw, "%s/%s_%s%.200s", ddir, ts, partial ? "part_" : "", name);
    FILE *f = fopen(raw, "wb");
    if (f) { fwrite(data, 1, (size_t)len, f); fclose(f); }
    image *img = img_load(raw);
    if (!img) img = jp2_load(raw);
    char msg[120];
    if (img) {
        char path[300];
        snprintf(path, sizeof path, "%.200s/%s_DRM%s.png", dir, ts, partial ? "_part" : "");
        if (img_save_png(path, img) == 0) {
            pthread_mutex_lock(&l->omx);
            snprintf(l->saved[l->sv_head % NSAVED], sizeof l->saved[0], "%s", path);
            l->sv_head++;
            if (l->sv_head - l->sv_tail > NSAVED) l->sv_tail = l->sv_head - NSAVED;
            pthread_mutex_unlock(&l->omx);
        }
        live_status st; pthread_mutex_lock(&l->omx); st = l->st; pthread_mutex_unlock(&l->omx);
        if (st.state != LIVE_RX) { st.mode = NULL; publish(l, img, &st); } else image_free(img);
        if (partial) snprintf(msg, sizeof msg, "Digital picture incomplete - saved what arrived (%ld bytes)", len);
        else snprintf(msg, sizeof msg, "Digital picture received: %.60s (%ld bytes)", name, len);
    } else {
        const char *ext = strrchr(name, '.');
        if (ext && (!strcmp(ext, ".jp2") || !strcmp(ext, ".j2k")) && !jp2_available())
            snprintf(msg, sizeof msg, "JPEG 2000 received but libopenjp2 is missing - saved as file");
        else
            snprintf(msg, sizeof msg, "Digital file received: %.70s", name);
    }
    pthread_mutex_lock(&l->omx); l->st.drm_new++; pthread_mutex_unlock(&l->omx);
    set_msg(l, msg);
}

/* copy the digital receiver's state into the shared status */
static void drm_status(live *l)
{
    drm_rx *r = &l->drm;
    int have, tot; const char *nm;
    mot_rx_progress(&r->mot, &have, &tot, &nm);
    pthread_mutex_lock(&l->omx);
    live_status *st = &l->st;
    int was = st->drm_sync;
    st->drm_sync = r->state == DRM_RX_SYNC && r->params_ok;
    if (st->drm_sync) {
        snprintf(st->drm_info, sizeof st->drm_info, "Mode %c  %s  %s  %s kHz", "ABE"[r->mode], qam_name[r->qam],
                 r->prot ? "Norm" : "High", r->so ? "2.5" : "2.3");
        snprintf(st->drm_call, sizeof st->drm_call, "%s", r->label);
        st->drm_snr = r->snr;
        st->drm_foff = r->foff;
    }
    st->drm_have = have;
    st->drm_total = tot;
    if (have != l->bsr_have) {                    /* keep a repeat request ready */
        l->bsr_have = have;
        l->bsr_req_len = mot_rx_bsr(&r->mot, l->bsr_req, sizeof l->bsr_req);
    }
    st->drm_can_bsr = !st->drm_sync && l->bsr_req_len > 0 && strcmp(nm, "bsr.bin");
    snprintf(st->drm_name, sizeof st->drm_name, "%s", nm);
    pthread_mutex_unlock(&l->omx);
    if (st->drm_sync && !was) set_msg(l, "Digital signal (HamDRM) - receiving");
    if (!st->drm_sync) l->drm_announced[0] = 0;
    else if (r->fac_since_sync >= 6) {          /* the label is complete after a super frame */
        char lab[12];
        snprintf(lab, sizeof lab, "%s", r->label);
        for (char *q = lab + strlen(lab); q > lab && q[-1] == ' '; q--) q[-1] = 0;
        for (char *q = lab; *q; q++) *q = (char)toupper((unsigned char)*q);
        if (fskid_valid_call(lab) && strcmp(lab, l->drm_announced)) {
            snprintf(l->drm_announced, sizeof l->drm_announced, "%s", lab);
            heard(l, lab, 2);
        }
    }
    if (!st->drm_sync && was && tot > 0 && have < tot && strcmp(nm, "bsr.bin")) {
        char m[120];
        snprintf(m, sizeof m, "Digital signal ended - %d of %d segments missing (Ask repeat sends a BSR)", tot - have, tot);
        set_msg(l, m);
    }
    /* show the picture growing: decode what arrived so far, about once a second */
    if (st->drm_sync && have > l->drm_shown && l->n - l->drm_shown_at > (long)l->fs) {
        live_status cur; pthread_mutex_lock(&l->omx); cur = l->st; pthread_mutex_unlock(&l->omx);
        const char *ext = strrchr(nm, '.');
        if (cur.state != LIVE_RX && ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))) {
            uint8_t *d = NULL;
            long len = mot_rx_prefix(&r->mot, &d);
            if (len > 600) {
                image *img = img_load_mem(d, (int)len);
                if (img) { cur.mode = NULL; publish(l, img, &cur); }
            }
            free(d);
        }
        l->drm_shown = have;
        l->drm_shown_at = l->n;
    }
    if (!have) l->drm_shown = 0;
}

static void *engine(void *arg)
{
    live *l = arg;
    float *tmp = malloc(RING * sizeof(float));
    struct timespec nap = { 0, 20000000 };
    while (atomic_load(&l->run)) {
        /* take everything the capture thread has delivered */
        pthread_mutex_lock(&l->rmx);
        unsigned long avail = l->wr - l->rd;
        if (avail > RING) { l->rd = l->wr - RING; avail = RING; }     /* overrun: keep the newest */
        for (unsigned long i = 0; i < avail; i++) tmp[i] = l->ring[(l->rd + i) & (RING - 1)];
        l->rd += avail;
        pthread_mutex_unlock(&l->rmx);

        int muted = atomic_load(&l->mute);
        int rst = atomic_exchange(&l->reset_req, 0);
        if ((muted && !l->muted_now) || rst) {
            /* transmitting (or new stream): do not decode our own signal */
            live_status st; pthread_mutex_lock(&l->omx); st = l->st; pthread_mutex_unlock(&l->omx);
            if (st.state == LIVE_RX) finish_rx(l, 2);
            buf_trim(l, l->n);
            l->search_from = l->sync_from = 0;
            fmdemod_free(l->dm); l->dm = fmdemod_new(l->fs);
            drm_rx_reset(&l->drm);
        }
        l->muted_now = muted;
        if (muted) { nanosleep(&nap, NULL); continue; }

        for (unsigned long i = 0; i < avail; i++) {
            float f = fmdemod_process(l->dm, tmp[i]);
            buf_append(l, f);
            fskid_process(&l->fid, f);
        }
        if (avail) { drm_rx_process(&l->drm, tmp, (int)avail); drm_status(l); }

        live_status st; pthread_mutex_lock(&l->omx); st = l->st; pthread_mutex_unlock(&l->omx);
        if (st.state == LIVE_RX && atomic_exchange(&l->stop_req, 0)) finish_rx(l, 2);
        else if (st.state == LIVE_RX) {
            if (l->n - l->last_work >= (long)(0.5 * l->fs)) { l->last_work = l->n; work_rx(l); }
        } else {
            atomic_store(&l->stop_req, 0);
            if (l->n - l->last_work >= (long)(0.1 * l->fs)) { l->last_work = l->n; work_idle(l); }
        }
        nanosleep(&nap, NULL);
    }
    free(tmp);
    return NULL;
}

/* ---------------------------------------------------------------- API --- */

live *live_new(double fs, const char *save_dir)
{
    live *l = calloc(1, sizeof *l);
    l->fs = fs;
    l->ring = calloc(RING, sizeof(float));
    l->dm = fmdemod_new(fs);
    pthread_mutex_init(&l->rmx, NULL);
    pthread_mutex_init(&l->omx, NULL);
    snprintf(l->dir, sizeof l->dir, "%s", save_dir);
    snprintf(l->st.msg, sizeof l->st.msg, "Waiting for an SSTV signal");
    drm_rx_init(&l->drm, (int)fs, drm_done, l);
    fskid_init(&l->fid, fs, fskid_cb, l);
    atomic_store(&l->nr, 1);
    atomic_store(&l->run, 1);
    pthread_create(&l->th, NULL, engine, l);
    return l;
}

void live_free(live *l)
{
    if (!l) return;
    atomic_store(&l->run, 0);
    pthread_join(l->th, NULL);
    fmdemod_free(l->dm);
    drm_rx_free(&l->drm);
    fskid_free(&l->fid);
    free(l->ring); free(l->f); free(l->P); free(l->Q);
    image_free(l->img);
    free(l);
}

void live_push(live *l, const float *x, int n)
{
    pthread_mutex_lock(&l->rmx);
    for (int i = 0; i < n; i++) l->ring[(l->wr + (unsigned long)i) & (RING - 1)] = x[i];
    l->wr += (unsigned long)n;
    pthread_mutex_unlock(&l->rmx);
}

void live_mute(live *l, int on) { atomic_store(&l->mute, on); }
void live_set_nr(live *l, int on) { atomic_store(&l->nr, on); }
void live_stop(live *l) { atomic_store(&l->stop_req, 1); }
void live_reset(live *l) { atomic_store(&l->reset_req, 1); }

void live_set_force(live *l, const sstv_mode *m)
{
    pthread_mutex_lock(&l->omx); l->force = m; pthread_mutex_unlock(&l->omx);
}

void live_set_dir(live *l, const char *save_dir)
{
    pthread_mutex_lock(&l->omx); snprintf(l->dir, sizeof l->dir, "%s", save_dir); pthread_mutex_unlock(&l->omx);
}

int live_snapshot(live *l, unsigned *gen, image **img, live_status *st)
{
    int r = 0;
    pthread_mutex_lock(&l->omx);
    *st = l->st;
    if (l->img && l->gen != *gen) {
        image *c = image_new(l->img->w, l->img->h);
        memcpy(c->px, l->img->px, (size_t)c->w * c->h * 3);
        *img = c; *gen = l->gen; r = 1;
    }
    pthread_mutex_unlock(&l->omx);
    return r;
}

int live_take_saved(live *l, char *path, size_t n)
{
    int r = 0;
    pthread_mutex_lock(&l->omx);
    if (l->sv_tail < l->sv_head) { snprintf(path, n, "%s", l->saved[l->sv_tail % NSAVED]); l->sv_tail++; r = 1; }
    pthread_mutex_unlock(&l->omx);
    return r;
}

int live_bsr_request(live *l, char *out, int max)
{
    pthread_mutex_lock(&l->omx);
    int n = l->bsr_req_len < max ? l->bsr_req_len : 0;
    if (n) memcpy(out, l->bsr_req, n);
    pthread_mutex_unlock(&l->omx);
    return n;
}

int live_last_bsr(live *l, unsigned *tid, int *list, int max, char *from, size_t fn)
{
    pthread_mutex_lock(&l->omx);
    int n = l->bsr_n < max ? l->bsr_n : max;
    *tid = l->bsr_tid;
    memcpy(list, l->bsr_list, sizeof(int) * n);
    snprintf(from, fn, "%s", l->bsr_from);
    pthread_mutex_unlock(&l->omx);
    return n;
}

/* audio.c - ALSA capture/playback (capture thread taken over from DigiPan Next) */
#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <alsa/asoundlib.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "audio.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "wav.h"

#ifndef _WIN32
static snd_pcm_t *audio_pcm = NULL;
#endif
static pthread_mutex_t pcm_mutex = PTHREAD_MUTEX_INITIALIZER;   /* guards audio_pcm */
static pthread_t audio_thread;
static atomic_int audio_run = 0, audio_thread_up = 0;
static atomic_int level_milli = 0;
static audio_cb the_cb = NULL;
static int in_chan = 0;                 /* 0 mix, 1 left, 2 right */
static int in_nch = 1;                  /* channels the device was opened with */
void audio_set_channel(int ch) { in_chan = ch >= 0 && ch <= 2 ? ch : 0; }
static void *the_user = NULL;

/* file source */
static float *file_buf = NULL;
static long file_n = 0;
static double file_speed = 1.0;
static unsigned file_rate = 11025;

static void nap(long ns) { struct timespec ts = { ns / 1000000000L, ns % 1000000000L }; nanosleep(&ts, NULL); }

static void deliver(const float *x, int n)
{
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)x[i] * x[i];
    atomic_store(&level_milli, (int)(sqrt(sum / (n > 0 ? n : 1)) * 1000.0));
    if (the_cb) the_cb(x, n, the_user);
}

#ifdef _WIN32
/* ---- Windows (WinMM) ------------------------------------------------------ */
#define WBUFS 8
#define WFRAMES 512
static HWAVEIN win_in;
static HANDLE win_ev;
static WAVEHDR win_hdr[WBUFS];
static int16_t win_buf[WBUFS][WFRAMES * 2];
static char win_names[32][128];
static int win_n;

static void win_format(WAVEFORMATEX *f, unsigned rate, int nch)
{
    memset(f, 0, sizeof *f);
    f->wFormatTag = WAVE_FORMAT_PCM; f->nChannels = (WORD)nch; f->nSamplesPerSec = rate;
    f->wBitsPerSample = 16; f->nBlockAlign = (WORD)(2 * nch); f->nAvgBytesPerSec = rate * 2 * nch;
}

static void to_utf8(const WCHAR *w, char *o, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, o, n, NULL, NULL); }

int audio_list(char devs[][128], int max)
{
    int n = 0;
    snprintf(devs[n++], 128, "default");
    UINT nd = waveInGetNumDevs();
    win_n = 0;
    for (UINT i = 0; i < nd && n < max && win_n < 32; i++) {
        WAVEINCAPSW c;
        if (waveInGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        char name[128]; to_utf8(c.szPname, name, sizeof name);
        snprintf(win_names[win_n], 128, "%s", name);
        snprintf(devs[n++], 128, "%s", name);
        win_n++;
    }
    return n;
}

static UINT in_id(const char *device)
{
    UINT nd = waveInGetNumDevs();
    for (UINT i = 0; i < nd; i++) {
        WAVEINCAPSW c; char name[128];
        if (waveInGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        to_utf8(c.szPname, name, sizeof name);
        if (!strcmp(name, device)) return i;
    }
    return WAVE_MAPPER;
}

/* the playback device of the same sound card: "Microphone (USB Audio CODEC)" -> "Speakers (USB Audio CODEC)" */
static UINT out_id(const char *device, char *chosen, size_t nc)
{
    const char *p = device ? strchr(device, '(') : NULL;
    char key[128] = "";
    if (p) { snprintf(key, sizeof key, "%s", p); char *e = strrchr(key, ')'); if (e) e[1] = 0; }
    UINT nd = waveOutGetNumDevs();
    for (int pass = 0; pass < 2; pass++)
        for (UINT i = 0; i < nd; i++) {
            WAVEOUTCAPSW c; char name[128];
            if (waveOutGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
            to_utf8(c.szPname, name, sizeof name);
            if ((pass == 0 && device && !strcmp(name, device)) || (pass == 1 && key[0] && strstr(name, key))) {
                snprintf(chosen, nc, "%s", name); return i;
            }
        }
    snprintf(chosen, nc, "default output");
    return WAVE_MAPPER;
}

static void *capture_worker(void *unused)
{
    (void)unused;
    float tmp[WFRAMES];
    int cur = 0;
    while (atomic_load(&audio_run)) {
        WaitForSingleObject(win_ev, 100);
        pthread_mutex_lock(&pcm_mutex);
        if (!win_in) { pthread_mutex_unlock(&pcm_mutex); nap(10000000); continue; }
        int got[WBUFS], ng = 0;
        while (ng < WBUFS && (win_hdr[cur].dwFlags & WHDR_DONE)) {
            int k = (int)(win_hdr[cur].dwBytesRecorded / (2 * in_nch));
            got[ng++] = cur;
            for (int i = 0; i < k; i++) tmp[i] = win_buf[cur][i * in_nch + (in_nch > 1 ? in_chan - 1 : 0)] / 32768.0f;
            pthread_mutex_unlock(&pcm_mutex);
            if (k > 0) deliver(tmp, k);
            pthread_mutex_lock(&pcm_mutex);
            if (!win_in) break;
            win_hdr[cur].dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(win_in, &win_hdr[cur], sizeof(WAVEHDR));
            cur = (cur + 1) % WBUFS;
        }
        pthread_mutex_unlock(&pcm_mutex);
        (void)got;
    }
    return NULL;
}
#else
int audio_list(char devs[][128], int max)
{
    int n = 0;
    snprintf(devs[n++], 128, "default");
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0 && n < max)
        snprintf(devs[n++], 128, "plughw:%d,0", card);   /* plug: rate/channel conversion */
    return n;
}

static void *capture_worker(void *unused)
{
    (void)unused;
    int16_t buf[1024];
    float tmp[512];
    while (atomic_load(&audio_run)) {
        /* Hold the PCM handle only while waiting/reading. snd_pcm_wait() with a
           timeout keeps a stalled device from blocking forever. */
        pthread_mutex_lock(&pcm_mutex);
        snd_pcm_t *pcm = audio_pcm;
        if (!pcm) { pthread_mutex_unlock(&pcm_mutex); nap(10000000); continue; }
        /* A capture stream stays idle in PREPARED state (also after an xrun recovery)
           and snd_pcm_wait() would then time out forever: start it explicitly. */
        if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED) snd_pcm_start(pcm);
        int w = snd_pcm_wait(pcm, 100);
        if (w == 0) { pthread_mutex_unlock(&pcm_mutex); continue; }
        snd_pcm_sframes_t got = w < 0 ? w : snd_pcm_readi(pcm, buf, 512);
        int recover_failed = 0;
        if (got < 0 && snd_pcm_recover(pcm, (int)got, 1) < 0) recover_failed = 1;
        pthread_mutex_unlock(&pcm_mutex);
        if (got < 0) { if (recover_failed) nap(50000000); continue; }
        if (got == 0) continue;
        int nc = in_nch, off = nc > 1 ? in_chan - 1 : 0;
        for (snd_pcm_sframes_t i = 0; i < got; i++) tmp[i] = (float)buf[i * nc + off] / 32768.0f;
        deliver(tmp, (int)got);
    }
    return NULL;
}

#endif

static void *file_worker(void *unused)
{
    (void)unused;
    const int blk = 512;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    long pos = 0;
    while (atomic_load(&audio_run)) {
        /* pace: pos samples should have been delivered by now */
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
        long due = (long)(el * file_rate * file_speed);
        if (pos >= due) { nap(5000000); continue; }
        float tmp[512];
        for (int i = 0; i < blk; i++, pos++)
            tmp[i] = pos < file_n ? file_buf[pos] : 0.0f;   /* silence after the end */
        deliver(tmp, blk);
    }
    return NULL;
}

static void stop_thread(void)
{
    if (atomic_load(&audio_thread_up)) {
        atomic_store(&audio_run, 0);
        pthread_join(audio_thread, NULL);
        atomic_store(&audio_thread_up, 0);
    }
}

void audio_stop(void)
{
    stop_thread();
    pthread_mutex_lock(&pcm_mutex);
#ifdef _WIN32
    if (win_in) {
        waveInReset(win_in);
        for (int i = 0; i < WBUFS; i++) waveInUnprepareHeader(win_in, &win_hdr[i], sizeof(WAVEHDR));
        waveInClose(win_in);
        win_in = NULL;
    }
#else
    if (audio_pcm) { snd_pcm_drop(audio_pcm); snd_pcm_close(audio_pcm); audio_pcm = NULL; }
#endif
    pthread_mutex_unlock(&pcm_mutex);
    free(file_buf); file_buf = NULL; file_n = 0;
}

int audio_running(void) { return atomic_load(&audio_thread_up); }
float audio_level(void) { return atomic_load(&level_milli) / 1000.0f; }

#ifdef _WIN32
int audio_start(const char *device, unsigned rate, audio_cb cb, void *user, char *status, size_t ns)
{
    audio_stop();
    the_cb = cb; the_user = user;
    if (!win_ev) win_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    in_nch = in_chan ? 2 : 1;
    WAVEFORMATEX f; win_format(&f, rate, in_nch);
    HWAVEIN h = NULL;
    UINT id = strcmp(device, "default") ? in_id(device) : WAVE_MAPPER;
    MMRESULT r = waveInOpen(&h, id, &f, (DWORD_PTR)win_ev, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR && in_nch == 2) {      /* a mono device: its only channel */
        in_nch = 1; win_format(&f, rate, 1);
        r = waveInOpen(&h, id, &f, (DWORD_PTR)win_ev, 0, CALLBACK_EVENT);
    }
    if (r != MMSYSERR_NOERROR) { snprintf(status, ns, "Audio open failed (error %u)", (unsigned)r); return -1; }
    for (int i = 0; i < WBUFS; i++) {
        memset(&win_hdr[i], 0, sizeof(WAVEHDR));
        win_hdr[i].lpData = (LPSTR)win_buf[i];
        win_hdr[i].dwBufferLength = WFRAMES * 2 * in_nch;
        waveInPrepareHeader(h, &win_hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(h, &win_hdr[i], sizeof(WAVEHDR));
    }
    pthread_mutex_lock(&pcm_mutex); win_in = h; pthread_mutex_unlock(&pcm_mutex);
    atomic_store(&audio_run, 1);
    if (pthread_create(&audio_thread, NULL, capture_worker, NULL) != 0) {
        snprintf(status, ns, "Audio: cannot start thread"); audio_stop(); return -1;
    }
    atomic_store(&audio_thread_up, 1);
    waveInStart(h);
    snprintf(status, ns, "Audio OK: %.80s @ %u Hz%s", device, rate, in_nch == 2 ? (in_chan == 1 ? ", left" : ", right") : "");
    return 0;
}
#else
int audio_start(const char *device, unsigned rate, audio_cb cb, void *user, char *status, size_t ns)
{
    audio_stop();
    the_cb = cb; the_user = user;
    snd_pcm_t *p = NULL;
    int rc = snd_pcm_open(&p, device, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) { snprintf(status, ns, "Audio open failed: %.80s", snd_strerror(rc)); return -1; }
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(p, params);
    rc = snd_pcm_hw_params_set_access(p, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc >= 0) rc = snd_pcm_hw_params_set_format(p, params, SND_PCM_FORMAT_S16_LE);
    unsigned r = rate;
    if (rc >= 0) rc = snd_pcm_hw_params_set_rate_near(p, params, &r, 0);
    if (rc >= 0 && r != rate) {
        snprintf(status, ns, "Device runs at %u Hz, need %u (use a plughw: device)", r, rate);
        snd_pcm_close(p); return -1;
    }
    /* one channel of a stereo input, or mono (both mixed by the plug device) */
    int nch = in_chan ? 2 : 1;
    if (rc >= 0 && nch == 2 && snd_pcm_hw_params_set_channels(p, params, 2) < 0) nch = 1;   /* mono device */
    if (rc >= 0 && nch == 1) rc = snd_pcm_hw_params_set_channels(p, params, 1);
    snd_pcm_uframes_t period = 512, buffer = 4096;
    if (rc >= 0) rc = snd_pcm_hw_params_set_period_size_near(p, params, &period, 0);
    if (rc >= 0) rc = snd_pcm_hw_params_set_buffer_size_near(p, params, &buffer);
    if (rc >= 0) rc = snd_pcm_hw_params(p, params);
    if (rc >= 0) rc = snd_pcm_prepare(p);
    if (rc < 0) { snprintf(status, ns, "Audio setup failed: %.70s", snd_strerror(rc)); snd_pcm_close(p); return -1; }
    in_nch = nch;
    pthread_mutex_lock(&pcm_mutex); audio_pcm = p; pthread_mutex_unlock(&pcm_mutex);
    atomic_store(&audio_run, 1);
    if (pthread_create(&audio_thread, NULL, capture_worker, NULL) != 0) {
        snprintf(status, ns, "Audio: cannot start thread"); audio_stop(); return -1;
    }
    atomic_store(&audio_thread_up, 1);
    snprintf(status, ns, "Audio OK: %s @ %u Hz%s", device, rate, in_nch == 2 ? (in_chan == 1 ? ", left" : ", right") : "");
    return 0;
}

#endif

int audio_start_file(const char *path, unsigned rate, double speed, audio_cb cb, void *user, char *status, size_t ns)
{
    audio_stop();
    the_cb = cb; the_user = user;
    double fs; long n;
    float *x = wav_read_ch(path, &fs, &n, in_chan);
    if (!x) { snprintf(status, ns, "Cannot read %.100s", path); return -1; }
    if (fs > rate * 1.01) {
        /* anti-alias low-pass (Blackman windowed sinc, 0.45 * new rate) before decimating */
        int N = (int)(fs / rate * 32) | 1, M = N - 1;
        double fc = 0.45 * rate / fs, *h = malloc((size_t)N * sizeof(double)), sum = 0;
        for (int i = 0; i < N; i++) {
            double k = i - M / 2.0;
            h[i] = (k == 0 ? 2 * fc : sin(2 * M_PI * fc * k) / (M_PI * k)) *
                   (0.42 - 0.5 * cos(2 * M_PI * i / M) + 0.08 * cos(4 * M_PI * i / M));
            sum += h[i];
        }
        float *y = malloc((size_t)n * sizeof(float));
        for (long i = 0; i < n; i++) {
            double acc = 0;
            for (int j = 0; j < N; j++) { long t = i + j - M / 2; if (t >= 0 && t < n) acc += h[j] * x[t]; }
            y[i] = (float)(acc / sum);
        }
        free(h); free(x); x = y;
    }
    /* linear resampling to the working rate */
    long m = (long)((double)n * rate / fs);
    file_buf = malloc((size_t)(m > 0 ? m : 1) * sizeof(float));
    for (long i = 0; i < m; i++) {
        double s = i * fs / rate; long k = (long)s; double fr = s - k;
        file_buf[i] = (float)(k + 1 < n ? x[k] * (1 - fr) + x[k + 1] * fr : x[n - 1]);
    }
    free(x);
    file_n = m; file_rate = rate; file_speed = speed > 0 ? speed : 1;
    atomic_store(&audio_run, 1);
    if (pthread_create(&audio_thread, NULL, file_worker, NULL) != 0) {
        snprintf(status, ns, "Audio: cannot start thread"); audio_stop(); return -1;
    }
    atomic_store(&audio_thread_up, 1);
    const char *b = strrchr(path, '/');
    snprintf(status, ns, "Test input: %.70s (%.0fx speed)", b ? b + 1 : path, file_speed);
    return 0;
}

/* ---------------------------------------------------------------- TX --- */

struct audio_out {
#ifdef _WIN32
    HWAVEOUT wo;
    HANDLE ev;
    WAVEHDR hdr[WBUFS];
    int16_t buf16[WBUFS][1024];
    int next;
#else
    snd_pcm_t *pcm;
#endif
    char  *wav_path;
    float *buf;           /* WAV mode: collected samples */
    long   n, cap;
    unsigned rate;
};

audio_out *audio_out_open(const char *device, const char *wav_path, unsigned rate, char *err, size_t ne)
{
    audio_out *o = calloc(1, sizeof *o);
    o->rate = rate;
    if (wav_path && *wav_path) { o->wav_path = strdup(wav_path); return o; }
#ifdef _WIN32
    char chosen[128];
    UINT id = out_id(device, chosen, sizeof chosen);
    WAVEFORMATEX f; win_format(&f, rate, 1);          /* playback stays mono */
    o->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    MMRESULT r = waveOutOpen(&o->wo, id, &f, (DWORD_PTR)o->ev, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { snprintf(err, ne, "TX: playback open failed on %.60s (error %u)", chosen, (unsigned)r); CloseHandle(o->ev); free(o); return NULL; }
    for (int i = 0; i < WBUFS; i++) { o->hdr[i].lpData = (LPSTR)o->buf16[i]; o->hdr[i].dwFlags = WHDR_DONE; }
    return o;
#else
    int rc = snd_pcm_open(&o->pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) { snprintf(err, ne, "TX: playback open failed: %.80s", snd_strerror(rc)); free(o); return NULL; }
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(o->pcm, params);
    rc = snd_pcm_hw_params_set_access(o->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc >= 0) rc = snd_pcm_hw_params_set_format(o->pcm, params, SND_PCM_FORMAT_S16_LE);
    unsigned r = rate;
    if (rc >= 0) rc = snd_pcm_hw_params_set_rate_near(o->pcm, params, &r, 0);
    if (rc >= 0 && r != rate) {
        snprintf(err, ne, "TX: playback rate is not %u Hz (use a plughw: device)", rate);
        snd_pcm_close(o->pcm); free(o); return NULL;
    }
    if (rc >= 0) rc = snd_pcm_hw_params_set_channels(o->pcm, params, 1);
    snd_pcm_uframes_t period = 512, buffer = 4096;
    if (rc >= 0) rc = snd_pcm_hw_params_set_period_size_near(o->pcm, params, &period, 0);
    if (rc >= 0) rc = snd_pcm_hw_params_set_buffer_size_near(o->pcm, params, &buffer);
    if (rc >= 0) rc = snd_pcm_hw_params(o->pcm, params);
    if (rc >= 0) rc = snd_pcm_prepare(o->pcm);
    if (rc < 0) { snprintf(err, ne, "TX: playback setup failed: %.80s", snd_strerror(rc)); snd_pcm_close(o->pcm); free(o); return NULL; }
    return o;
#endif
}

int audio_out_write(audio_out *o, const float *x, int n)
{
    if (o->wav_path) {                       /* test mode: collect, pace like a sound card */
        if (o->n + n > o->cap) { o->cap = (o->n + n) * 2; o->buf = realloc(o->buf, (size_t)o->cap * sizeof(float)); }
        memcpy(o->buf + o->n, x, (size_t)n * sizeof(float));
        o->n += n;
        nap((long)(1e9 * n / o->rate / 20));   /* 20x real time: progress stays visible */
        return 0;
    }
#ifdef _WIN32
    int off = 0;
    while (off < n) {
        WAVEHDR *h = &o->hdr[o->next];
        while (!(h->dwFlags & WHDR_DONE)) WaitForSingleObject(o->ev, 200);
        if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(o->wo, h, sizeof(WAVEHDR));
        int k = n - off > 1024 ? 1024 : n - off;
        for (int i = 0; i < k; i++) {
            float v = x[off + i];
            if (v > 1.f) v = 1.f;
            if (v < -1.f) v = -1.f;
            o->buf16[o->next][i] = (int16_t)lrintf(v * 32767.f);
        }
        h->dwBufferLength = (DWORD)k * 2;
        h->dwFlags = 0;
        if (waveOutPrepareHeader(o->wo, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
            waveOutWrite(o->wo, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return -1;
        o->next = (o->next + 1) % WBUFS;
        off += k;
    }
    return 0;
#else
    int16_t b[512];
    int off = 0;
    while (off < n) {
        int k = n - off > 512 ? 512 : n - off;
        for (int i = 0; i < k; i++) {
            float v = x[off + i];
            if (v > 1.f) v = 1.f;
            if (v < -1.f) v = -1.f;
            b[i] = (int16_t)lrintf(v * 32767.f);
        }
        int done = 0;
        while (done < k) {
            snd_pcm_sframes_t w = snd_pcm_writei(o->pcm, b + done, (snd_pcm_uframes_t)(k - done));
            if (w < 0) { if (snd_pcm_recover(o->pcm, (int)w, 1) < 0) return -1; continue; }
            done += (int)w;
        }
        off += k;
    }
    return 0;
#endif
}

void audio_out_close(audio_out *o, int drain)
{
    if (!o) return;
    if (o->wav_path) {
        wav_write(o->wav_path, o->buf, o->n, (int)o->rate);
        free(o->wav_path); free(o->buf);
    } else {
#ifdef _WIN32
        if (drain)
            for (int i = 0; i < WBUFS; i++) while (!(o->hdr[i].dwFlags & WHDR_DONE)) WaitForSingleObject(o->ev, 200);
        else waveOutReset(o->wo);
        for (int i = 0; i < WBUFS; i++) if (o->hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(o->wo, &o->hdr[i], sizeof(WAVEHDR));
        waveOutClose(o->wo);
        CloseHandle(o->ev);
#else
        if (drain) snd_pcm_drain(o->pcm); else snd_pcm_drop(o->pcm);
        snd_pcm_close(o->pcm);
#endif
    }
    free(o);
}

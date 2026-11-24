/* adif.c - ADIF QSO log (taken over from DigiPan Next) */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include "adif.h"
#include "fileswap.h"
#include "version.h"

Qso *qsos = NULL;
int  nqso = 0;
char log_path[300] = "";
static int qcap = 0;

const char *band_of(double mhz)
{
    static const struct { double lo, hi; const char *n; } t[] = {
        {1.8,2.0,"160m"},{3.5,4.0,"80m"},{5.25,5.45,"60m"},{7.0,7.3,"40m"},{10.1,10.15,"30m"},{14.0,14.35,"20m"},
        {18.068,18.168,"17m"},{21.0,21.45,"15m"},{24.89,24.99,"12m"},{28.0,29.7,"10m"},{50.0,54.0,"6m"},{144.0,148.0,"2m"},{430.0,440.0,"70cm"}};
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (mhz >= t[i].lo && mhz <= t[i].hi) return t[i].n;
    return "";
}

static void log_add(const Qso *q)
{
    if (nqso == qcap) { int nc = qcap ? qcap * 2 : 256; Qso *nq = realloc(qsos, (size_t)nc * sizeof *nq); if (!nq) return; qsos = nq; qcap = nc; }
    qsos[nqso++] = *q;
}

static void cpy(char *d, size_t n, const char *s, size_t len) { if (len >= n) len = n - 1; memcpy(d, s, len); d[len] = 0; }

static void adif_parse(const char *buf, size_t len)
{
    size_t i = 0, rec = 0; Qso q; memset(&q, 0, sizeof q);
    while (i < len) {
        if (buf[i] != '<') { i++; continue; }
        size_t j = i + 1; while (j < len && buf[j] != '>') j++;
        if (j >= len) break;
        char tag[48]; cpy(tag, sizeof tag, buf + i + 1, j - i - 1);
        char *colon = strchr(tag, ':'); size_t fl = 0;
        if (colon) { *colon = 0; fl = (size_t)atoi(colon + 1); }
        for (char *p = tag; *p; p++) *p = (char)toupper((unsigned char)*p);
        i = j + 1;
        if (!strcmp(tag, "EOH")) { rec = i; memset(&q, 0, sizeof q); continue; }
        if (!strcmp(tag, "EOR")) {
            if (q.call[0]) {
                size_t s = rec; while (s < i && (buf[s] == '\n' || buf[s] == '\r' || buf[s] == ' ')) s++;
                q.raw = malloc(i - s + 2); if (q.raw) { memcpy(q.raw, buf + s, i - s); q.raw[i - s] = 0; log_add(&q); }
            }
            memset(&q, 0, sizeof q); rec = i; continue;
        }
        if (fl > 0 && i + fl <= len) {
            const char *v = buf + i;
            if (!strcmp(tag, "CALL")) cpy(q.call, sizeof q.call, v, fl);
            else if (!strcmp(tag, "QSO_DATE")) cpy(q.date, sizeof q.date, v, fl);
            else if (!strcmp(tag, "TIME_ON")) cpy(q.time_on, sizeof q.time_on, v, fl);
            else if (!strcmp(tag, "BAND")) cpy(q.band, sizeof q.band, v, fl);
            else if (!strcmp(tag, "MODE")) cpy(q.mode, sizeof q.mode, v, fl);
            else if (!strcmp(tag, "RST_SENT")) cpy(q.rst_s, sizeof q.rst_s, v, fl);
            else if (!strcmp(tag, "RST_RCVD")) cpy(q.rst_r, sizeof q.rst_r, v, fl);
            else if (!strcmp(tag, "COMMENT")) cpy(q.comment, sizeof q.comment, v, fl);
            else if (!strcmp(tag, "FREQ")) { char t[24]; cpy(t, sizeof t, v, fl); q.freq = atof(t); }
            i += fl;
        }
    }
}

void adif_open(void)
{
    if (!log_path[0]) { const char *h = getenv("HOME"); snprintf(log_path, sizeof log_path, "%s/digipan_log.adi", h && *h ? h : "."); }
    FILE *f = fopen(log_path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 64L * 1024 * 1024) { char *b = malloc((size_t)sz + 1); if (b) { size_t n = fread(b, 1, (size_t)sz, f); adif_parse(b, n); free(b); } }
    fclose(f);
}

static void adif_header(FILE *f)
{
    fprintf(f, "SSTV Next ADIF log\n<ADIF_VER:5>3.1.4 <PROGRAMID:8>SSTVNext <PROGRAMVERSION:%d>%s <EOH>\n\n",
            (int)strlen(SSTVNEXT_VERSION), SSTVNEXT_VERSION);
}

static int log_rewrite(void)
{
    char tmp[320]; snprintf(tmp, sizeof tmp, "%s.tmp", log_path);
    FILE *f = fopen(tmp, "w"); if (!f) return -1;
    adif_header(f);
    for (int i = 0; i < nqso; i++) { fputs(qsos[i].raw, f); fputc('\n', f); }
    int ok = (fflush(f) == 0 && fsync(fileno(f)) == 0); fclose(f);
    if (!ok || file_replace(tmp, log_path) != 0) return -1;
    return 0;
}

static int adif_put(char *buf, size_t n, int off, const char *tag, const char *val)
{
    if (!val || !*val) return off;
    int w = snprintf(buf + off, n - (size_t)off, "<%s:%zu>%s ", tag, strlen(val), val);
    return (w < 0 || (size_t)w >= n - (size_t)off) ? off : off + w;
}

int adif_log(const char *call, double mhz, const char *rst_s, const char *rst_r,
             const char *comment, const char *my_call, long start, long end, char *msg, size_t nmsg)
{
    if (!call || !call[0]) { snprintf(msg, nmsg, "Enter the other station's callsign first"); return -1; }
    time_t ts = (time_t)start, te = (time_t)end;
    struct tm a, b; gmtime_r(&ts, &a); gmtime_r(&te, &b);
    char d1[9], t1[7], d2[9], t2[7], fr[24];
    strftime(d1, sizeof d1, "%Y%m%d", &a); strftime(t1, sizeof t1, "%H%M%S", &a);
    strftime(d2, sizeof d2, "%Y%m%d", &b); strftime(t2, sizeof t2, "%H%M%S", &b);
    const char *band = band_of(mhz);
    Qso q; memset(&q, 0, sizeof q);
    snprintf(q.call, sizeof q.call, "%s", call); snprintf(q.date, sizeof q.date, "%s", d1); snprintf(q.time_on, sizeof q.time_on, "%s", t1);
    snprintf(q.band, sizeof q.band, "%s", band); snprintf(q.mode, sizeof q.mode, "SSTV");
    snprintf(q.rst_s, sizeof q.rst_s, "%s", rst_s); snprintf(q.rst_r, sizeof q.rst_r, "%s", rst_r);
    snprintf(q.comment, sizeof q.comment, "%s", comment ? comment : ""); q.freq = mhz;
    char raw[1024]; int o = 0;
    o = adif_put(raw, sizeof raw, o, "CALL", q.call); o = adif_put(raw, sizeof raw, o, "QSO_DATE", d1); o = adif_put(raw, sizeof raw, o, "TIME_ON", t1);
    o = adif_put(raw, sizeof raw, o, "QSO_DATE_OFF", d2); o = adif_put(raw, sizeof raw, o, "TIME_OFF", t2);
    o = adif_put(raw, sizeof raw, o, "BAND", band);
    if (mhz > 0) { snprintf(fr, sizeof fr, "%.6f", mhz); o = adif_put(raw, sizeof raw, o, "FREQ", fr); }
    o = adif_put(raw, sizeof raw, o, "MODE", "SSTV");
    o = adif_put(raw, sizeof raw, o, "RST_SENT", q.rst_s); o = adif_put(raw, sizeof raw, o, "RST_RCVD", q.rst_r);
    o = adif_put(raw, sizeof raw, o, "COMMENT", q.comment);
    o = adif_put(raw, sizeof raw, o, "STATION_CALLSIGN", my_call); o = adif_put(raw, sizeof raw, o, "OPERATOR", my_call);
    if (o + 8 < (int)sizeof raw) snprintf(raw + o, sizeof raw - (size_t)o, "<EOR>");
    FILE *f = fopen(log_path, "a");
    if (!f) { snprintf(msg, nmsg, "Cannot write the log file"); return -1; }
    fseek(f, 0, SEEK_END); if (ftell(f) == 0) adif_header(f);
    int ok = (fprintf(f, "%s\n", raw) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0); fclose(f);
    if (!ok) { snprintf(msg, nmsg, "Write error - QSO NOT saved"); return -1; }
    q.raw = strdup(raw); if (q.raw) log_add(&q);
    snprintf(msg, nmsg, "Logged %s %s %s", q.call, band, mhz > 0 ? "" : "(no frequency)");
    return 0;
}

int adif_delete(int k)
{
    if (k < 0 || k >= nqso) return -1;
    Qso keep = qsos[k];
    memmove(&qsos[k], &qsos[k + 1], (size_t)(nqso - k - 1) * sizeof *qsos); nqso--;
    if (log_rewrite() != 0) {
        memmove(&qsos[k + 1], &qsos[k], (size_t)(nqso - k) * sizeof *qsos); qsos[k] = keep; nqso++;
        return -1;
    }
    free(keep.raw);
    return 0;
}

int adif_dupe(const char *call, double mhz, char *msg, size_t nmsg)
{
    msg[0] = 0;
    if (!call || !call[0]) return 0;
    const char *cb = band_of(mhz);
    int r = 0;
    for (int i = nqso - 1; i >= 0; i--) {
        if (strcasecmp(qsos[i].call, call)) continue;
        if (cb[0] && !strcmp(cb, qsos[i].band) && !strcasecmp(qsos[i].mode, "SSTV")) {   /* PSK on the same band is not a dupe */
            snprintf(msg, nmsg, "DUPE: %s %s %s %.8s", qsos[i].call, qsos[i].band, qsos[i].mode, qsos[i].date);
            return 2;
        }
        if (!r) { snprintf(msg, nmsg, "Worked before: %s %s %.8s", qsos[i].band[0] ? qsos[i].band : "?", qsos[i].mode, qsos[i].date); r = 1; }
    }
    return r;
}

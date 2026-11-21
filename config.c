/* config.c - settings file */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "config.h"
#include "rig.h"

app_config cfg;

static const char *home(void) { const char *h = getenv("HOME"); return h && *h ? h : "/tmp"; }

static void cfg_path(char *out, size_t n, const char *app, int mk)
{
    char d[300];
    snprintf(d, sizeof d, "%s/.config", home()); if (mk) mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/.config/%s", home(), app); if (mk) mkdir(d, 0700);
    snprintf(out, n, "%s/.config/%s/config", home(), app);
}

/* keys shared with DigiPan (callsign, radio) and our own */
static int load_file(const char *path, int ours)
{
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char l[400];
    while (fgets(l, sizeof l, f)) {
        char *nl = strchr(l, '\n'); if (nl) *nl = 0;
        char *eq = strchr(l, '='); if (!eq) continue;
        *eq = 0; const char *k = l, *v = eq + 1;
        if (!strcmp(k, "call")) snprintf(cfg.call, sizeof cfg.call, "%.20s", v);
        else if (!strcmp(k, "model")) cfg_model = atoi(v);
        else if (!strcmp(k, "name")) snprintf(cfg_model_name, sizeof cfg_model_name, "%.60s", v);
        else if (!strcmp(k, "port")) snprintf(cfg_port, sizeof cfg_port, "%.90s", v);
        else if (!strcmp(k, "baud")) { cfg_baud = atoi(v); if (cfg_baud < 300 || cfg_baud > 4000000) cfg_baud = 9600; }
        else if (!strcmp(k, "cat")) cfg_cat = atoi(v) ? 1 : 0;
        else if (!ours) continue;
        else if (!strcmp(k, "audio")) snprintf(cfg.audio, sizeof cfg.audio, "%.120s", v);
        else if (!strcmp(k, "rxdir")) snprintf(cfg.rx_dir, sizeof cfg.rx_dir, "%.250s", v);
        else if (!strcmp(k, "txdir")) snprintf(cfg.tx_dir, sizeof cfg.tx_dir, "%.250s", v);
        else if (!strcmp(k, "overlay")) snprintf(cfg.overlay, sizeof cfg.overlay, "%.60s", v);
        else if (!strcmp(k, "txmode")) snprintf(cfg.tx_mode, sizeof cfg.tx_mode, "%.12s", v);
        else if (!strcmp(k, "rxmode")) snprintf(cfg.rx_mode, sizeof cfg.rx_mode, "%.12s", v);
        else if (!strcmp(k, "nr")) cfg.nr = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "fskid")) cfg.fskid = atoi(v);
        else if (!strcmp(k, "tpl_report")) cfg.tpl_report = atoi(v);
        else if (!strcmp(k, "template")) snprintf(cfg.tpl, sizeof cfg.tpl, "%.46s", v);
        else if (!strcmp(k, "drm")) sscanf(v, "%d,%d,%d,%d,%d,%d,%d", &cfg.drm_rob, &cfg.drm_qam, &cfg.drm_prot, &cfg.drm_so, &cfg.drm_size, &cfg.drm_kb, &cfg.drm_rs);
    }
    fclose(f);
    return 0;
}

void config_load(void)
{
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.rx_dir, sizeof cfg.rx_dir, "%s/sstv/rx", home());
    snprintf(cfg.tx_dir, sizeof cfg.tx_dir, "%s/sstv/tx", home());
    snprintf(cfg.overlay, sizeof cfg.overlay, "%%M");
    snprintf(cfg.tx_mode, sizeof cfg.tx_mode, "s2");
    snprintf(cfg.rx_mode, sizeof cfg.rx_mode, "auto");
    cfg.nr = 1;
    cfg.fskid = 1;
    cfg.drm_rob = 1; cfg.drm_qam = 1; cfg.drm_prot = 1; cfg.drm_so = 1; cfg.drm_size = 0; cfg.drm_kb = 3;
    char p[400];
    cfg_path(p, sizeof p, "sstvnext", 0);
    if (load_file(p, 1) != 0) {                  /* first start: borrow DigiPan's setup */
        cfg_path(p, sizeof p, "digipan", 0);
        load_file(p, 0);
    }
}

void config_save(void)
{
    char p[400]; cfg_path(p, sizeof p, "sstvnext", 1);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "call=%s\nmodel=%d\nname=%s\nport=%s\nbaud=%d\ncat=%d\n",
            cfg.call, cfg_model, cfg_model_name, cfg_port, cfg_baud, cfg_cat);
    fprintf(f, "audio=%s\nrxdir=%s\ntxdir=%s\noverlay=%s\ntxmode=%s\nrxmode=%s\nnr=%d\ntemplate=%s\n",
            cfg.audio, cfg.rx_dir, cfg.tx_dir, cfg.overlay, cfg.tx_mode, cfg.rx_mode, cfg.nr, cfg.tpl);
    fprintf(f, "fskid=%d\n", cfg.fskid);
    fprintf(f, "tpl_report=%d\n", cfg.tpl_report);
    fprintf(f, "drm=%d,%d,%d,%d,%d,%d,%d\n", cfg.drm_rob, cfg.drm_qam, cfg.drm_prot, cfg.drm_so, cfg.drm_size, cfg.drm_kb, cfg.drm_rs);
    fclose(f);
}

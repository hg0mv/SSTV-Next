/* config.h - settings file (~/.config/sstvnext/config)
 *
 * On the first start the callsign and the radio setup are taken over from
 * DigiPan Next (~/.config/digipan/config), so the radio need not be set again.
 */
#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    char call[24];
    char audio[128];        /* last used audio device */
    char rx_dir[256];       /* received images are saved here */
    char tx_dir[256];       /* images offered for transmission */
    char overlay[64];       /* text put on the TX image, %M = my call */
    char tx_mode[16];       /* mode id, e.g. "s2" */
    char rx_mode[16];       /* "auto" or a mode id (receive without VIS) */
    int  nr;                /* QRM speck removal on received pictures */
    char tpl[48];           /* template put on the transmitted picture */
    /* digital (HamDRM) transmission */
    int  drm_rob, drm_qam, drm_prot, drm_so;   /* mode A/B/E, 4/16/64-QAM, high/normal, 2.3/2.5 kHz */
    int  drm_size, drm_kb;                     /* picture size index, JPEG size limit (kB) */
    int  drm_rs;                               /* Reed-Solomon protection 0 (off) .. 4 */
    int  fskid;                                /* send FSK ID (callsign) after analog pictures */
} app_config;

extern app_config cfg;

void config_load(void);     /* also sets the rig cfg_* values */
void config_save(void);

#endif

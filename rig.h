/* rig.h - radio control through Hamlib rigctld (taken over from DigiPan Next)
 *
 * The program normally starts and supervises its own rigctld (model, port and
 * baud chosen in the Radio dialog); an external rigctld can be used instead.
 * All calls are thread safe.
 */
#ifndef RIG_H
#define RIG_H

#include <stddef.h>

typedef struct { int id; char name[80]; } RigModel;

/* settings (saved in the config file) */
extern int  cfg_model, cfg_baud, cfg_cat;
extern char cfg_port[96], cfg_model_name[80];

extern int  rig_enabled;             /* 1: PTT/CAT through rigctld */
extern char rig_status[160];         /* human readable state for the UI */

/* model list from "rigctl -l" and serial port scan (for the dialog) */
extern RigModel *rig_models;
extern int rig_nmodels;
extern char rig_ports[16][96];
extern int rig_nports;
extern const int rig_bauds[6];
void rig_load_models(void);
void rig_scan_ports(void);

void rig_use_external(const char *host, int port);   /* "-r host:port" */
int  rig_apply(void);        /* (re)start the radio link from cfg_*; 0 = ok */
void rig_shutdown(void);     /* stop rigctld, close the socket */

int  rig_cmd(const char *cmd);       /* 0 = "RPRT 0" */
int  rig_ptt(int on);                /* 0 = ok (also when no rig: VOX) */
int  rig_get_freq(double *hz);       /* 0 = ok */

#endif

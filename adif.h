/* adif.h - ADIF QSO log (taken over from DigiPan Next) */
#ifndef ADIF_H
#define ADIF_H

#include <stddef.h>

typedef struct {
    char call[24], date[9], time_on[7], band[8], mode[12], rst_s[8], rst_r[8], comment[48];
    double freq;
    char *raw;            /* the record as it is in the file */
} Qso;

extern Qso *qsos;
extern int  nqso;
extern char log_path[300];

const char *band_of(double mhz);
void adif_open(void);                         /* loads log_path (default ~/sstvnext_log.adi) */

/* Appends one QSO. start/end: UTC times. Returns 0, or -1 with a message. */
int  adif_log(const char *call, double mhz, const char *rst_s, const char *rst_r,
              const char *comment, const char *my_call, long start, long end,
              char *msg, size_t nmsg);
int  adif_delete(int k);                      /* rewrites the file; 0 = ok */

/* 2: same call on the same band, 1: worked before on another band, 0: new */
int  adif_dupe(const char *call, double mhz, char *msg, size_t nmsg);

#endif

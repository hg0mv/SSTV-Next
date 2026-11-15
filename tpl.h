/* tpl.h - picture templates (like MMSSTV's template editor)
 *
 * A template is a list of objects drawn over the picture to transmit: text
 * (TrueType, outline, shadow, macros), rectangle, ellipse, line and pasted
 * pictures. Positions are fractions of the picture size, so one template fits
 * every mode. Drawing is plain C (no X11), the editor lives in sstv_next.c.
 */
#ifndef TPL_H
#define TPL_H

#include <time.h>
#include "image.h"

typedef enum { OBJ_TEXT = 0, OBJ_RECT, OBJ_ELLIPSE, OBJ_LINE, OBJ_IMAGE } obj_type;

typedef struct {
    obj_type type;
    double x, y, w, h;          /* fractions of the picture; line: from (x,y) to (x+w,y+h);
                                   text: h = text height, w = measured width (set by tpl_render) */
    unsigned char fill[3];      /* text colour / shape fill */
    unsigned char line[3];      /* outline / border / line colour */
    int filled;                 /* rect, ellipse: filled (else only the border) */
    double lw;                  /* border / line / outline width, in 1/256 of the picture height */
    int alpha;                  /* 0..100 % */
    int shadow;                 /* text, shapes: drop shadow */
    int outline;                /* text: outline in 'line' colour */
    char font[64];              /* text: font file name (e.g. DejaVuSans-Bold.ttf) */
    char text[160];             /* text, may contain macros; image: file path */
    image *cache;               /* image: loaded picture (not saved) */
    char cache_path[160];
} tobj;

#define TPL_MAXOBJ 48

typedef struct {
    char name[48];
    int  n;
    tobj o[TPL_MAXOBJ];
} tpl;

typedef struct {
    const char *my_call, *his_call, *rst;
    double mhz;                 /* 0: unknown */
    time_t now;
} tpl_macros;

/* fonts */
int         tpl_font_count(void);
const char *tpl_font_name(int i);           /* file name without directory */
int         tpl_font_index(const char *name);

void tpl_init(tpl *t, const char *name);
void tpl_clear(tpl *t);                      /* frees cached pictures */
tobj *tpl_add(tpl *t, obj_type type);        /* with sensible defaults, centred */
void tpl_delete(tpl *t, int i);
void tpl_move(tpl *t, int i, int d);         /* d = +1: one layer up (drawn later) */

void tpl_expand(const char *src, const tpl_macros *m, char *out, size_t n);
void tpl_render(tpl *t, image *img, const tpl_macros *m);

/* top-most object under the point (fractions), -1: none */
int  tpl_hit(const tpl *t, double fx, double fy, double aspect);

/* files: <dir>/<name>.tpl */
int  tpl_save(const tpl *t, const char *dir);
int  tpl_load(tpl *t, const char *dir, const char *name);
int  tpl_list(const char *dir, char names[][48], int max);   /* sorted */
int  tpl_remove(const char *dir, const char *name);
void tpl_make_defaults(const char *dir, const char *old_overlay);   /* when the folder is empty */

#endif

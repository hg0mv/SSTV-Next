/* glob.h - minimal glob() for Windows: the wildcard may only be in the last path element */
#ifndef WIN_GLOB_H
#define WIN_GLOB_H
#include <stddef.h>
typedef struct { size_t gl_pathc; char **gl_pathv; size_t gl_offs; } glob_t;
#define GLOB_NOSORT 0
#define GLOB_NOMATCH 3
int  glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g);
void globfree(glob_t *g);
#endif

/* glob.c - minimal glob() for Windows (wildcards only in the last path element) */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "include/glob.h"

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g)
{
	(void)flags; (void)errfunc;
	memset(g, 0, sizeof *g);
	char dir[MAX_PATH];
	snprintf(dir, sizeof dir, "%s", pattern);
	char *s1 = strrchr(dir, '/'), *s2 = strrchr(dir, '\\'), *sl = s1 > s2 ? s1 : s2;
	if (sl) sl[1] = 0; else dir[0] = 0;
	int has_wild = strpbrk(pattern, "*?") != NULL;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) return GLOB_NOMATCH;
	size_t cap = 0;
	do {
		if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
		if (g->gl_pathc + 1 >= cap) {
			cap = cap ? cap * 2 : 16;
			g->gl_pathv = realloc(g->gl_pathv, cap * sizeof(char *));
		}
		size_t n = strlen(dir) + strlen(fd.cFileName) + 1;
		char *p = malloc(n);
		snprintf(p, n, "%s%s", has_wild ? dir : "", has_wild ? fd.cFileName : pattern);
		g->gl_pathv[g->gl_pathc++] = p;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	if (!g->gl_pathc) return GLOB_NOMATCH;
	qsort(g->gl_pathv, g->gl_pathc, sizeof(char *), cmp);
	g->gl_pathv[g->gl_pathc] = NULL;
	return 0;
}

void globfree(glob_t *g)
{
	for (size_t i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[i]);
	free(g->gl_pathv);
	memset(g, 0, sizeof *g);
}

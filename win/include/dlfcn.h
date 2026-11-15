/* dlfcn.h - dlopen() and friends for Windows */
#ifndef WIN_DLFCN_H
#define WIN_DLFCN_H
#include <windows.h>
#define RTLD_NOW 2
#define RTLD_LAZY 1
static inline void *dlopen(const char *name, int flags) { (void)flags; return (void *)LoadLibraryA(name); }
static inline void *dlsym(void *h, const char *s) { return (void *)(uintptr_t)GetProcAddress((HMODULE)h, s); }
static inline int dlclose(void *h) { return FreeLibrary((HMODULE)h) ? 0 : -1; }
#endif

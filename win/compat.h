/* compat.h - included first in every file of the Windows build (-include) */
#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H
#ifdef _WIN32
#define _USE_MATH_DEFINES
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L
#endif
#include <stdint.h>
#include <direct.h>
#include <io.h>
#include <math.h>
#define mkdir(path, mode) _mkdir(path)
#define fsync(fd) _commit(fd)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#endif
#endif

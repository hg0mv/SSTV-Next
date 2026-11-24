/* fileswap.h - put a finished temporary file in place of the old one
 *
 * rename() does this on POSIX; on Windows it fails when the target exists, so there
 * MoveFileEx with REPLACE_EXISTING is used (paths are UTF-8: the program runs with
 * the UTF-8 code page).
 */
#ifndef FILESWAP_H
#define FILESWAP_H

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static inline int file_replace(const char *tmp, const char *dst)
{
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}
#else
static inline int file_replace(const char *tmp, const char *dst) { return rename(tmp, dst); }
#endif

#endif

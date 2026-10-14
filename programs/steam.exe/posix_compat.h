#ifndef STEAM_HELPER_POSIX_COMPAT_H
#define STEAM_HELPER_POSIX_COMPAT_H

/* setenv is POSIX and absent from the mingw PE C runtime. steam.cpp uses it to
   publish anti-cheat and VR runtime paths into the environment; map it onto the
   mingw _putenv_s so the source builds unchanged. */

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value ? value : "");
}

#ifdef __cplusplus
}
#endif

#endif

/*
 * test_compat.h -- Portability shims for test code.
 *
 * Provides setenv/unsetenv wrappers for MinGW cross-compilation
 * (Ubuntu mingw-w64 headers lack these; native MSYS2 has them).
 */

#ifndef TT_TEST_COMPAT_H
#define TT_TEST_COMPAT_H

#include "platform.h"

#ifdef TT_PLATFORM_WINDOWS
#include <stdlib.h>

#ifndef setenv
static inline int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}
#endif

#ifndef unsetenv
static inline int unsetenv(const char *name)
{
    return _putenv_s(name, "");
}
#endif

#endif /* TT_PLATFORM_WINDOWS */

#endif /* TT_TEST_COMPAT_H */

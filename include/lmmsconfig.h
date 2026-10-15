#ifndef LMMSCONFIG_H
#define LMMSCONFIG_H

#if __has_include("../build/lmmsconfig.h")
#include "../build/lmmsconfig.h"
#elif __has_include("build/lmmsconfig.h")
#include "build/lmmsconfig.h"
#else
#if defined(__APPLE__)
#define LMMS_BUILD_APPLE 1
#elif defined(__linux__)
#define LMMS_BUILD_LINUX 1
#elif defined(_WIN32)
#define LMMS_BUILD_WIN32 1
#endif
#define LMMS_HAVE_PTHREAD_H 1
#define LMMS_HAVE_UNISTD_H 1
#endif

#endif // LMMSCONFIG_H

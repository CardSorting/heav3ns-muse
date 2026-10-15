#ifndef LMMS_EXPORT_H
#define LMMS_EXPORT_H

#ifndef LMMS_EXPORT
#  if defined(_WIN32) || defined(__CYGWIN__)
#    if defined(lmmsobjs_EXPORTS)
#      define LMMS_EXPORT __declspec(dllexport)
#    else
#      define LMMS_EXPORT __declspec(dllimport)
#    endif
#  elif defined(__GNUC__) && __GNUC__ >= 4
#    define LMMS_EXPORT __attribute__((visibility("default")))
#  else
#    define LMMS_EXPORT
#  endif
#endif

#ifndef LMMS_NO_EXPORT
#  if defined(__GNUC__) && __GNUC__ >= 4
#    define LMMS_NO_EXPORT __attribute__((visibility("hidden")))
#  else
#    define LMMS_NO_EXPORT
#  endif
#endif

#ifndef LMMS_DEPRECATED
#  if defined(__GNUC__) && __GNUC__ >= 4
#    define LMMS_DEPRECATED __attribute__ ((__deprecated__))
#  elif defined(_MSC_VER)
#    define LMMS_DEPRECATED __declspec(deprecated)
#  else
#    define LMMS_DEPRECATED
#  endif
#endif

#ifndef LMMS_DEPRECATED_EXPORT
#  define LMMS_DEPRECATED_EXPORT LMMS_EXPORT LMMS_DEPRECATED
#endif

#ifndef LMMS_DEPRECATED_NO_EXPORT
#  define LMMS_DEPRECATED_NO_EXPORT LMMS_NO_EXPORT LMMS_DEPRECATED
#endif

#endif // LMMS_EXPORT_H

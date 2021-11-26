#pragma once
/**
 * @file kuroko.h
 * @brief Top-level header with configuration macros.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

typedef int64_t krk_integer_type;

#if defined(__EMSCRIPTEN__) || defined(EFI_PLATFORM)
# define PRIkrk_int "%lld"
# define PRIkrk_hex "%llx"
# define parseStrInt strtoll
#elif defined(_WIN32)
# define PRIkrk_int "%I64d"
# define PRIkrk_hex "%I64x"
# define parseStrInt strtoll
# define ENABLE_THREADING
# else
# define PRIkrk_int "%ld"
# define PRIkrk_hex "%lx"
# define parseStrInt strtol
# define ENABLE_THREADING
#endif

#ifdef KRK_DISABLE_THREADS
# undef ENABLE_THREADING
#endif

#ifndef _WIN32
# define PATH_SEP "/"
# ifndef STATIC_ONLY
#  include <dlfcn.h>
#  define dlRefType void *
#  define dlSymType void *
#  define dlOpen(fileName) dlopen(fileName, RTLD_NOW)
#  define dlSym(dlRef, handlerName) dlsym(dlRef,handlerName)
#  define dlClose(dlRef) dlclose(dlRef)
# endif
#else
# include <windows.h>
# define PATH_SEP "\\"
# ifndef STATIC_ONLY
#  define dlRefType HINSTANCE
#  define dlSymType FARPROC
#  define dlOpen(fileName) LoadLibraryA(fileName)
#  define dlSym(dlRef, handlerName) GetProcAddress(dlRef, handlerName)
#  define dlClose(dlRef)
# endif
#endif


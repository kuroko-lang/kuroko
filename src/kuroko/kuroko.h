#pragma once
/**
 * @file kuroko.h
 * @brief Top-level header with configuration macros.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>

typedef int64_t krk_integer_type;

#define ENABLE_THREADING
#define PRIkrk_int "%" PRId64
#define PRIkrk_hex "%" PRIx64
#define parseStrInt strtoll

#if defined(KRK_DISABLE_THREADS) || defined(__EMSCRIPTEN__) || defined(EFI_PLATFORM)
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


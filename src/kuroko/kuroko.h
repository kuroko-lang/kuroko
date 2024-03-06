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

#ifndef _WIN32
# define KRK_PATH_SEP "/"
# ifndef KRK_STATIC_ONLY
#  include <dlfcn.h>
#  define krk_dlRefType void *
#  define krk_dlSymType void *
#  define krk_dlOpen(fileName) dlopen(fileName, RTLD_NOW)
#  define krk_dlSym(dlRef, handlerName) dlsym(dlRef,handlerName)
#  define krk_dlClose(dlRef) dlclose(dlRef)
# endif
#else
# include <windows.h>
# define KRK_PATH_SEP "\\"
# ifndef KRK_STATIC_ONLY
#  define krk_dlRefType HINSTANCE
#  define krk_dlSymType FARPROC
#  define krk_dlOpen(fileName) LoadLibraryA(fileName)
#  define krk_dlSym(dlRef, handlerName) GetProcAddress(dlRef, handlerName)
#  define krk_dlClose(dlRef)
# endif
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define KRK_NO_DOCUMENTATION 1
#define KRK_NO_GC_TRACING 1
typedef intptr_t ssize_t;
#pragma warning(disable : 4146) /* unary minus on unsigned */
#pragma warning(disable : 4996) /* sterror */
#pragma warning(disable : 4267) /* conversions to smaller types... */
#pragma warning(disable : 4244) /* conversions to smaller types... */
#include <math.h>
#define __builtin_floor floor
#define __builtin_unreachable abort
#define __builtin_expect(cond,expected) (cond)
#endif

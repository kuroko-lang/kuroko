#pragma once
/**
 * @file kuroko.h
 * @brief Top-level header with configuration macros.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#if defined(__EMSCRIPTEN__)
typedef int krk_integer_type;
# define PRIkrk_int "%d"
# define PRIkrk_hex "%x"
# define parseStrInt strtol
#elif defined(_WIN32)
typedef int krk_integer_type;
# define PRIkrk_int "%I32d"
# define PRIkrk_hex "%I32x"
# define parseStrInt strtol
# define ENABLE_THREADING
# else
typedef int krk_integer_type;
# define PRIkrk_int "%d"
# define PRIkrk_hex "%x"
# define parseStrInt strtol
# define ENABLE_THREADING
#endif

#ifdef DEBUG
#define ENABLE_DISASSEMBLY
#define ENABLE_TRACING
#define ENABLE_SCAN_TRACING
#define ENABLE_STRESS_GC
#endif


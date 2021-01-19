#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#if defined(__EMSCRIPTEN__) || defined(_WIN32)
typedef long long krk_integer_type;
#define PRIkrk_int "%lld"
#define PRIkrk_hex "%llx"
#define parseStrInt strtoll
#else
typedef long krk_integer_type;
#define PRIkrk_int "%ld"
#define PRIkrk_hex "%lx"
#define parseStrInt strtol
#endif

#ifdef DEBUG
#define ENABLE_DISASSEMBLY
#define ENABLE_TRACING
#define ENABLE_SCAN_TRACING
#define ENABLE_STRESS_GC
#endif

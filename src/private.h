/**
 * @file private.h
 * @brief Internal header.
 *
 * These functions are not part of the public API for Kuroko.
 * They are used internally by the interpreter library.
 */
#include "kuroko/kuroko.h"

extern void _createAndBind_numericClasses(void);
extern void _createAndBind_strClass(void);
extern void _createAndBind_listClass(void);
extern void _createAndBind_tupleClass(void);
extern void _createAndBind_bytesClass(void);
extern void _createAndBind_dictClass(void);
extern void _createAndBind_functionClass(void);
extern void _createAndBind_rangeClass(void);
extern void _createAndBind_setClass(void);
extern void _createAndBind_generatorClass(void);
extern void _createAndBind_sliceClass(void);
extern void _createAndBind_builtins(void);
extern void _createAndBind_type(void);
extern void _createAndBind_exceptions(void);
extern void _createAndBind_longClass(void);
extern void _createAndBind_compilerClass(void);

/**
 * @brief Index numbers for always-available interned strings representing important method and member names.
 *
 * The VM must look up many methods and members by fixed names. To avoid
 * continuously having to box and unbox these from C strings to the appropriate
 * interned @c KrkString, we keep an array of the @c KrkString pointers in the global VM state.
 *
 * These values are the offsets into that index for each of the relevant
 * function names (generally with extra underscores removed). For example
 * @c METHOD_INIT is the offset for the string value for @c "__init__".
 */
typedef enum {
	#define CACHED_METHOD(a,b,c) METHOD_ ## a,
	#define SPECIAL_ATTRS(a,b)   METHOD_ ## a,
	#include "methods.h"
	#undef CACHED_METHOD
	#undef SPECIAL_ATTRS
	METHOD__MAX,
} KrkSpecialMethods;


#define FORMAT_OP_EQ     (1 << 0)
#define FORMAT_OP_REPR   (1 << 1)
#define FORMAT_OP_STR    (1 << 2)
#define FORMAT_OP_FORMAT (1 << 3)

struct ParsedFormatSpec {
	const char * fill;
	char align;
	char sign;
	int  width;
	int  alt;
	char sep;
	int  prec;
	int hasWidth;
	int hasPrecision;
	int fillSize;
};

/* We inline hashing in a few places, so it's nice to have this in one place.
 * This is the "sdbm" hash. I've been using it in various places for many years,
 * and this specific version apparently traces to gawk. */
#define krk_hash_advance(hash,c) do { hash = (int)(c) + (hash << 6) + (hash << 16) - hash; } while (0)

#ifndef KRK_DISABLE_DEBUG
#include <kuroko/debug.h>
struct BreakpointEntry {
	KrkCodeObject * inFunction;
	size_t offset;
	int flags;
	uint8_t originalOpcode;
};

#define MAX_BREAKPOINTS 32
struct DebuggerState {
	int breakpointsCount;
	KrkDebugCallback debuggerHook;

	/* XXX This was previously thread-local; it probably should still be
	 *     specific to an individual thread... but we don't really do
	 *     much thread debugging, so... */
	int repeatStack_top;
	int repeatStack_bottom;
	int thisWasForced;

	struct BreakpointEntry breakpoints[MAX_BREAKPOINTS];
};
#endif

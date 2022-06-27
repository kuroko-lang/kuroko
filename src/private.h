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
extern void _createAndBind_gcMod(void);
extern void _createAndBind_timeMod(void);
extern void _createAndBind_osMod(void);
extern void _createAndBind_fileioMod(void);
#ifdef ENABLE_THREADING
extern void _createAndBind_threadsMod(void);
#endif


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


/**
 * @file private.h
 * @brief Internal header.
 *
 * These functions are not part of the public API for Kuroko.
 * They are used internally by the interpreter library.
 */
#include "kuroko/kuroko.h"

extern void _createAndBind_numericClasses(KrkThreadState*);
extern void _createAndBind_strClass(KrkThreadState*);
extern void _createAndBind_listClass(KrkThreadState*);
extern void _createAndBind_tupleClass(KrkThreadState*);
extern void _createAndBind_bytesClass(KrkThreadState*);
extern void _createAndBind_dictClass(KrkThreadState*);
extern void _createAndBind_functionClass(KrkThreadState*);
extern void _createAndBind_rangeClass(KrkThreadState*);
extern void _createAndBind_setClass(KrkThreadState*);
extern void _createAndBind_generatorClass(KrkThreadState*);
extern void _createAndBind_sliceClass(KrkThreadState*);
extern void _createAndBind_builtins(KrkThreadState*);
extern void _createAndBind_type(KrkThreadState*);
extern void _createAndBind_exceptions(KrkThreadState*);
extern void _createAndBind_longClass(KrkThreadState*);

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

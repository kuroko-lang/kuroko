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
extern void _createAndBind_gcMod(void);
extern void _createAndBind_timeMod(void);
extern void _createAndBind_osMod(void);
extern void _createAndBind_fileioMod(void);
#ifdef ENABLE_THREADING
extern void _createAndBind_threadsMod(void);
#endif



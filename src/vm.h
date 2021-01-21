#pragma once

#include <stdarg.h>
#include <sys/types.h>
#include "kuroko.h"
#include "value.h"
#include "table.h"
#include "object.h"

#define FRAMES_MAX 64

typedef struct {
	KrkClosure * closure;
	uint8_t * ip;
	size_t slots;
	size_t outSlots;
	KrkTable * globals;
} CallFrame;

typedef enum {
	METHOD_INIT,
	METHOD_STR,
	METHOD_REPR,
	METHOD_GET,
	METHOD_SET,
	METHOD_CLASS,
	METHOD_NAME,
	METHOD_FILE,
	METHOD_INT,
	METHOD_FLOAT,
	METHOD_CHR,
	METHOD_LEN,
	METHOD_DOC,
	METHOD_BASE,
	METHOD_GETSLICE,
	METHOD_LIST_INT,
	METHOD_DICT_INT,
	METHOD_INREPR,
	METHOD_ORD,
	METHOD_CALL,
	METHOD_EQ,
	METHOD_ENTER,
	METHOD_EXIT,
	METHOD_DELITEM,
	METHOD_ITER,
	METHOD_GETATTR,
	METHOD_DIR,

	METHOD__MAX,
} KrkSpecialMethods;

typedef struct {
	CallFrame frames[FRAMES_MAX];
	size_t frameCount;
	size_t stackSize;
	KrkValue * stack;
	KrkValue * stackTop;
	KrkInstance * module;
	KrkTable strings;
	KrkTable modules;
	KrkUpvalue * openUpvalues;
	KrkObj * objects;
	size_t bytesAllocated;
	size_t nextGC;
	size_t grayCount;
	size_t grayCapacity;
	ssize_t exitOnFrame;
	KrkObj** grayStack;
	KrkValue specialMethodNames[METHOD__MAX];

	KrkClass * objectClass;
	KrkClass * moduleClass;
	KrkInstance * builtins;
	KrkInstance * system;

	struct {
		KrkClass * baseException;
		KrkClass * typeError;
		KrkClass * argumentError;
		KrkClass * indexError;
		KrkClass * keyError;
		KrkClass * attributeError;
		KrkClass * nameError;
		KrkClass * importError;
		KrkClass * ioError;
		KrkClass * valueError;
		KrkClass * keyboardInterrupt;
		KrkClass * zeroDivisionError;
		KrkClass * notImplementedError;
		KrkClass * syntaxError;
	} exceptions;

	struct {
		KrkClass * typeClass; /* Class */
		KrkClass * intClass; /* Integer */
		KrkClass * floatClass; /* Floating */
		KrkClass * boolClass; /* Boolean */
		KrkClass * noneTypeClass; /* None */
		KrkClass * strClass; /* String */
		KrkClass * functionClass; /* Functions, Closures */
		KrkClass * methodClass; /* BoundMethod */
		KrkClass * tupleClass; /* Tuple */
		KrkClass * bytesClass; /* Bytes */

		/* Other useful stuff */
		KrkClass * listiteratorClass;
		KrkClass * rangeClass;
		KrkClass * rangeiteratorClass;
		KrkClass * striteratorClass;
		KrkClass * tupleiteratorClass;

		/* These are actually defined in builtins.krk and are real instances */
		KrkClass * listClass;
		KrkClass * dictClass;
	} baseClasses;

	KrkValue currentException;
	int flags;
	long watchdog;
} KrkVM;

#define KRK_ENABLE_TRACING      (1 << 0)
#define KRK_ENABLE_DISASSEMBLY  (1 << 1)
#define KRK_ENABLE_SCAN_TRACING (1 << 2)
#define KRK_ENABLE_STRESS_GC    (1 << 3)
#define KRK_NO_ESCAPE           (1 << 4)

#define KRK_GC_PAUSED           (1 << 10)
#define KRK_HAS_EXCEPTION       (1 << 11)

extern KrkVM krk_vm;
#define vm krk_vm

extern void krk_initVM(int flags);
extern void krk_freeVM(void);
extern void krk_resetStack(void);
extern KrkValue krk_interpret(const char * src, int newScope, char *, char *);
extern KrkValue krk_runfile(const char * fileName, int newScope, char *, char *);
extern void krk_push(KrkValue value);
extern KrkValue krk_pop(void);
extern KrkValue krk_peek(int distance);
extern const char * krk_typeName(KrkValue value);
extern void krk_defineNative(KrkTable * table, const char * name, NativeFn function);
extern void krk_attachNamedObject(KrkTable * table, const char name[], KrkObj * obj);
extern void krk_attachNamedValue(KrkTable * table, const char name[], KrkValue obj);
extern void krk_runtimeError(KrkClass * type, const char * fmt, ...);

#define KRK_PAUSE_GC() do { vm.flags |= KRK_GC_PAUSED; } while (0)
#define KRK_RESUME_GC() do { vm.flags &= ~(KRK_GC_PAUSED); } while (0)

extern KrkInstance * krk_dictCreate(void);
extern KrkValue  krk_runNext(void);
extern KrkValue krk_typeOf(int argc, KrkValue argv[]);
extern int krk_bindMethod(KrkClass * _class, KrkString * name);
extern int krk_callValue(KrkValue callee, int argCount, int extra);

extern KrkValue krk_list_of(int argc, KrkValue argv[]);
extern KrkValue krk_dict_of(int argc, KrkValue argv[]);
extern KrkValue krk_callSimple(KrkValue value, int argCount, int isMethod);
extern void krk_finalizeClass(KrkClass * _class);
extern void krk_dumpTraceback();
extern KrkInstance * krk_startModule(const char * name);
extern KrkValue krk_dirObject(int argc, KrkValue argv[]);
extern int krk_loadModule(KrkString * name, KrkValue * moduleOut, KrkString * runAs);

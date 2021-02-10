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
	METHOD_SETSLICE,
	METHOD_DELSLICE,

	METHOD__MAX,
} KrkSpecialMethods;

struct Exceptions {
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
};

/**
 * Be sure not to reorder this, even if it looks better, to maintain
 * ABI compatibility with existing binaries.
 */
struct BaseClasses {
	KrkClass * objectClass; /* Root of everything */
	KrkClass * moduleClass; /* Simple class mostly to provide __repr__ for modules */

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

	KrkClass * listiteratorClass;
	KrkClass * rangeClass;
	KrkClass * rangeiteratorClass;
	KrkClass * striteratorClass;
	KrkClass * tupleiteratorClass;

	KrkClass * listClass;
	KrkClass * dictClass;
	KrkClass * dictitemsClass;
	KrkClass * dictkeysClass;
};

/**
 * Thread state represents everything that changes during execution
 * and isn't a global property of the shared garbage collector.
 */
typedef struct ThreadState {
	struct ThreadState * next;

	CallFrame * frames;
	size_t frameCount;
	size_t stackSize;
	KrkValue * stack;
	KrkValue * stackTop;
	KrkUpvalue * openUpvalues;
	ssize_t exitOnFrame;

	KrkInstance * module;
	KrkValue currentException;
	int flags;
	long watchdog;

#define THREAD_SCRATCH_SIZE 3
	KrkValue scratchSpace[THREAD_SCRATCH_SIZE];
} KrkThreadState;

typedef struct {
	int globalFlags;                        /* Global VM state flags */
	char * binpath;                   /* A string representing the name of the interpreter binary. */
	KrkTable strings;                 /* Strings table */
	KrkTable modules;                 /* Module cache */
	KrkInstance * builtins;           /* '__builtins__' module */
	KrkInstance * system;             /* 'kuroko' module */
	KrkValue * specialMethodNames;     /* Cached strings of important method and function names */
	struct BaseClasses * baseClasses; /* Pointer to a (static) namespacing struct for the KrkClass*'s of built-in object types */
	struct Exceptions * exceptions;   /* Pointer to a (static) namespacing struct for the KrkClass*'s of basic exception types */

	/* Garbage collector state */
	KrkObj * objects;                 /* Linked list of all objects in the GC */
	size_t bytesAllocated;            /* Running total of bytes allocated */
	size_t nextGC;                    /* Point at which we should sweep again */
	size_t grayCount;                 /* Count of objects marked by scan. */
	size_t grayCapacity;              /* How many objects we can fit in the scan list. */
	KrkObj** grayStack;               /* Scan list */

	KrkThreadState * threads;         /* All the threads. */
} KrkVM;

/* Thread-specific flags */
#define KRK_ENABLE_TRACING      (1 << 0)
#define KRK_ENABLE_DISASSEMBLY  (1 << 1)
#define KRK_ENABLE_SCAN_TRACING (1 << 2)
#define KRK_HAS_EXCEPTION       (1 << 3)

/* Global flags */
#define KRK_ENABLE_STRESS_GC    (1 << 8)
#define KRK_GC_PAUSED           (1 << 9)
#define KRK_CLEAN_OUTPUT        (1 << 10)

#ifdef ENABLE_THREADING
#define krk_currentThread (*(krk_getCurrentThread()))
extern void _createAndBind_threadsMod(void);
#else
extern KrkThreadState krk_currentThread;
#endif

extern KrkVM krk_vm;
#define vm krk_vm

extern void krk_initVM(int flags);
extern void krk_freeVM(void);
extern void krk_resetStack(void);
extern KrkValue krk_interpret(const char * src, int newScope, char *, char *);
extern KrkValue krk_runfile(const char * fileName, int newScope, char *, char *);
extern KrkValue krk_callfile(const char * fileName, char * fromName, char * fromFile);
extern void krk_push(KrkValue value);
extern KrkValue krk_pop(void);
extern KrkValue krk_peek(int distance);
extern const char * krk_typeName(KrkValue value);
extern KrkNative * krk_defineNative(KrkTable * table, const char * name, NativeFn function);
extern KrkProperty * krk_defineNativeProperty(KrkTable * table, const char * name, NativeFn func);
extern void krk_attachNamedObject(KrkTable * table, const char name[], KrkObj * obj);
extern void krk_attachNamedValue(KrkTable * table, const char name[], KrkValue obj);
extern KrkValue krk_runtimeError(KrkClass * type, const char * fmt, ...);
extern KrkThreadState * krk_getCurrentThread(void);

extern KrkInstance * krk_dictCreate(void);
extern KrkValue  krk_runNext(void);
extern KrkClass * krk_getType(KrkValue);
extern int krk_isInstanceOf(KrkValue obj, KrkClass * type);
extern int krk_bindMethod(KrkClass * _class, KrkString * name);
extern int krk_callValue(KrkValue callee, int argCount, int extra);

extern KrkValue krk_list_of(int argc, KrkValue argv[]);
extern KrkValue krk_dict_of(int argc, KrkValue argv[]);
extern KrkValue krk_callSimple(KrkValue value, int argCount, int isMethod);
extern KrkClass * krk_makeClass(KrkInstance * module, KrkClass ** _class, const char * name, KrkClass * base);
extern void krk_finalizeClass(KrkClass * _class);
extern void krk_dumpTraceback();
extern KrkInstance * krk_startModule(const char * name);
extern KrkValue krk_dirObject(int argc, KrkValue argv[]);
extern int krk_loadModule(KrkString * name, KrkValue * moduleOut, KrkString * runAs);

/* obj_str.h */
extern void krk_addObjects(void);
extern KrkValue krk_string_get(int argc, KrkValue argv[], int hasKw);
extern KrkValue krk_string_int(int argc, KrkValue argv[], int hasKw);
extern KrkValue krk_string_float(int argc, KrkValue argv[], int hasKw);
extern KrkValue krk_string_split(int argc, KrkValue argv[], int hasKw);
extern KrkValue krk_string_format(int argc, KrkValue argv[], int hasKw);

/* obj_dict.h */
extern KrkValue krk_dict_nth_key_fast(size_t capacity, KrkTableEntry * entries, size_t index);

extern KrkValue krk_tuple_of(int argc, KrkValue argv[]);

extern int krk_isFalsey(KrkValue value);

extern void _createAndBind_numericClasses(void);
extern void _createAndBind_strClass(void);
extern void _createAndBind_listClass(void);
extern void _createAndBind_tupleClass(void);
extern void _createAndBind_bytesClass(void);
extern void _createAndBind_dictClass(void);
extern void _createAndBind_functionClass(void);
extern void _createAndBind_rangeClass(void);
extern void _createAndBind_setClass(void);
extern void _createAndBind_builtins(void);
extern void _createAndBind_type(void);
extern void _createAndBind_exceptions(void);
extern void _createAndBind_gcMod(void);

extern int krk_doRecursiveModuleLoad(KrkString * name);

extern KrkValue krk_operator_lt(KrkValue,KrkValue);
extern KrkValue krk_operator_gt(KrkValue,KrkValue);



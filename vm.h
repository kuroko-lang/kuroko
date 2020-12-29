#pragma once

#include <stdarg.h>
#include "kuroko.h"
#include "value.h"
#include "table.h"
#include "object.h"

#define FRAMES_MAX 64

typedef struct {
	KrkClosure * closure;
	uint8_t * ip;
	size_t slots;
} CallFrame;

typedef enum {
	METHOD_INIT,
	METHOD_STR,
	METHOD_GET,
	METHOD_SET,
	METHOD_CLASS,
	METHOD_NAME,
	METHOD_FILE,

	METHOD__MAX,
} KrkSpecialMethods;

typedef struct {
	CallFrame frames[FRAMES_MAX];
	size_t frameCount;
	size_t stackSize;
	KrkValue * stack;
	KrkValue * stackTop;
	KrkTable globals;
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

	KrkClass * object_class;
	KrkInstance * builtins;

	KrkValue currentException;
	int flags;
} KrkVM;

#define KRK_ENABLE_TRACING      (1 << 0)
#define KRK_ENABLE_DEBUGGING    (1 << 1)
#define KRK_ENABLE_SCAN_TRACING (1 << 2)
#define KRK_ENABLE_STRESS_GC    (1 << 3)

#define KRK_GC_PAUSED           (1 << 10)
#define KRK_HAS_EXCEPTION       (1 << 11)

extern KrkVM vm;

extern void krk_initVM(int flags);
extern void krk_freeVM(void);
extern KrkValue krk_interpret(const char * src, int newScope, char *, char *);
extern KrkValue krk_runfile(const char * fileName, int newScope, char *, char *);
extern void krk_push(KrkValue value);
extern KrkValue krk_pop(void);
extern const char * krk_typeName(KrkValue value);
extern void krk_defineNative(KrkTable * table, const char * name, NativeFn function);
extern void krk_attachNamedObject(KrkTable * table, const char name[], KrkObj * obj);
extern void krk_runtimeError(const char * fmt, ...);

#define KRK_PAUSE_GC() do { vm.flags |= KRK_GC_PAUSED; } while (0)
#define KRK_RESUME_GC() do { vm.flags &= ~(KRK_GC_PAUSED); } while (0)

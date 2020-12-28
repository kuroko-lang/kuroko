#pragma once

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
	size_t exitOnFrame;
	KrkObj** grayStack;
	KrkValue specialMethodNames[METHOD__MAX];

	KrkClass * object_class;
	KrkInstance * builtins;

	unsigned char enableDebugging:1;
	unsigned char enableTracing:1;
	unsigned char enableScanTracing:1;
	unsigned char enableStressGC:1;
} KrkVM;

extern KrkVM vm;

extern void krk_initVM(void);
extern void krk_freeVM(void);
extern KrkValue krk_interpret(const char * src, int newScope, char *, char *);
extern KrkValue krk_runfile(const char * fileName, int newScope, char *, char *);
extern void krk_push(KrkValue value);
extern KrkValue krk_pop(void);
extern const char * krk_typeName(KrkValue value);
extern void krk_defineNative(KrkTable * table, const char * name, NativeFn function);
extern void krk_attachNamedObject(KrkTable * table, const char name[], KrkObj * obj);

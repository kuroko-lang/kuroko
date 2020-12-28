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

	char enableDebugging;
	char enableTracing;
} KrkVM;

extern KrkVM vm;

extern void krk_initVM(void);
extern void krk_freeVM(void);
extern KrkValue krk_interpret(const char * src, int newScope);
extern KrkValue krk_runfile(const char * fileName, int newScope);
extern void krk_push(KrkValue value);
extern KrkValue krk_pop(void);
extern const char * typeName(KrkValue value);

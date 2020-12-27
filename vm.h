#pragma once

#include "kuroko.h"
#include "value.h"
#include "table.h"
#include "object.h"

#define FRAMES_MAX 64

typedef struct {
	KrkClosure * closure;
	uint8_t * ip;
	KrkValue * slots;
} CallFrame;

typedef struct {
	CallFrame frames[FRAMES_MAX];
	size_t frameCount;
	size_t stackSize;
	KrkValue * stack;
	KrkValue * stackTop;
	KrkTable globals;
	KrkTable strings;
	KrkString * __init__;
	KrkUpvalue * openUpvalues;
	KrkObj * objects;
	size_t bytesAllocated;
	size_t nextGC;
	size_t grayCount;
	size_t grayCapacity;
	KrkObj** grayStack;
} KrkVM;

extern KrkVM vm;

extern void krk_initVM(void);
extern void krk_freeVM(void);
extern int krk_interpret(const char * src);
extern void krk_push(KrkValue value);
extern KrkValue krk_pop(void);
extern const char * typeName(KrkValue value);

#pragma once

#include "kuroko.h"
#include "chunk.h"
#include "value.h"

typedef struct {
	KrkChunk * chunk;
	uint8_t * ip;
	size_t stackSize;
	KrkValue * stack;
	KrkValue * stackTop;
	KrkObj * objects;
} KrkVM;

extern KrkVM vm;

extern void krk_initVM(void);
extern void krk_freeVM(void);
extern int krk_interpret(const char * src);
extern void krk_push(KrkValue value);
extern KrkValue krk_pop(void);
extern const char * typeName(KrkValue value);

#pragma once

#include <stdio.h>

#include "kuroko.h"
#include "value.h"
#include "chunk.h"

#define OBJECT_TYPE(value) (AS_OBJECT(value)->type)
#define IS_STRING(value)   isObjType(value, OBJ_STRING)
#define AS_STRING(value)   ((KrkString *)AS_OBJECT(value))
#define AS_CSTRING(value)  (((KrkString *)AS_OBJECT(value))->chars)
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define AS_FUNCTION(value) ((KrkFunction *)AS_OBJECT(value))
#define IS_NATIVE(value)   isObjType(value, OBJ_NATIVE)
#define AS_NATIVE(value)   (((KrkNative *)AS_OBJECT(value))->function)
#define IS_CLOSURE(value)  isObjType(value, OBJ_CLOSURE)
#define AS_CLOSURE(value)  ((KrkClosure *)AS_OBJECT(value))

typedef enum {
	OBJ_FUNCTION,
	OBJ_NATIVE,
	OBJ_CLOSURE,
	OBJ_STRING,
	OBJ_UPVALUE,
} ObjType;

struct Obj {
	ObjType type;
	struct Obj * next;
};

struct ObjString {
	KrkObj obj;
	size_t length;
	char * chars;
	uint32_t hash;
};

typedef struct KrkUpvalue {
	KrkObj obj;
	KrkValue * location;
	KrkValue   closed;
	struct KrkUpvalue * next;
} KrkUpvalue;

typedef struct {
	KrkObj obj;
	int arity;
	size_t upvalueCount;
	KrkChunk chunk;
	KrkString * name;
} KrkFunction;

typedef struct {
	KrkObj obj;
	KrkFunction * function;
	KrkUpvalue ** upvalues;
	size_t upvalueCount;
} KrkClosure;


typedef KrkValue (*NativeFn)(int argCount, KrkValue* args);
typedef struct {
	KrkObj obj;
	NativeFn function;
} KrkNative;

static inline int isObjType(KrkValue value, ObjType type) {
	return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

extern KrkString * takeString(char * chars, size_t length);
extern KrkString * copyString(const char * chars, size_t length);
extern void krk_printObject(FILE * f, KrkValue value);

extern KrkFunction * newFunction();
extern KrkNative * newNative(NativeFn function);

extern KrkClosure * newClosure(KrkFunction * function);

extern KrkUpvalue * newUpvalue(KrkValue * slot);
